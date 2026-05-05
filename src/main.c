/* ============================================================================
 * main.c — Wasteland Entry Point
 * ============================================================================
 *
 * Responsibilities:
 *   - SDL2 + OpenGL initialization
 *   - Nuklear backend initialization (via nuklear_sdl_gl2.h)
 *   - Application state setup (model scanning, inference engine init)
 *   - Spawn the inference worker pthread
 *   - Main 60 FPS event/render loop
 *   - Graceful teardown
 * ============================================================================ */

/* SDL2 headers: use the unprefixed form so the include path provided by both
 * Debian's libsdl2-dev (-I/usr/include/SDL2) and Homebrew's SDL2Config.cmake
 * (-I/opt/homebrew/include/SDL2) resolves them. The <SDL2/SDL.h> form only
 * works on Linux because /usr/include is a system path; on macOS Homebrew
 * lives outside the default search path and the include fails. */
#include <SDL.h>
#include <SDL_opengl.h>
/* <SDL_opengl.h> drags in the correct GL header per platform
 * (<GL/gl.h> on Linux/Windows, <OpenGL/gl.h> on macOS) — don't add a
 * second hard-coded GL include or the macOS build breaks. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  include <shellscalingapi.h>  /* SetProcessDpiAwareness fallback */
#  include <shellapi.h>
#  ifndef S_ISDIR
#    define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#  endif
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <unistd.h>
#  include <sys/types.h>
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <dirent.h>
#endif

/* ---------------------------------------------------------------------------
 * Per-monitor DPI awareness on Windows
 *
 * Without this, a 1280x800 window on a 150%/200%-scaled display gets
 * virtualized by the OS — the window is rendered at logical size then
 * stretched up so it overflows the screen, and Windows pins it as if it
 * were maximised, killing the user's ability to drag-resize. Linux/macOS
 * handle DPI correctly without our involvement.
 * --------------------------------------------------------------------------- */
static void platform_enable_dpi_awareness(void)
{
#ifdef _WIN32
    /* Try the Win10 1703+ API first (per-monitor v2: best behaviour). It is
     * resolved dynamically because older Win10/Win8.1 SDKs don't ship the
     * import lib stub. Fall back to the Win8.1 API, then the legacy
     * Win Vista API, so the binary keeps working on every Windows >= 7. */
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContext_t)(DPI_AWARENESS_CONTEXT);
        SetProcessDpiAwarenessContext_t p =
            (SetProcessDpiAwarenessContext_t)
            GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (p && p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }

    HMODULE shcore = LoadLibraryA("shcore.dll");
    if (shcore) {
        typedef HRESULT (WINAPI *SetProcessDpiAwareness_t)(int);
        SetProcessDpiAwareness_t p =
            (SetProcessDpiAwareness_t)
            GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (p && SUCCEEDED(p(2 /* PROCESS_PER_MONITOR_DPI_AWARE */))) {
            FreeLibrary(shcore);
            return;
        }
        FreeLibrary(shcore);
    }

    SetProcessDPIAware();
#endif
}

/* Nuklear declaration-only includes for main.c */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#include <nuklear.h>

#include "nuklear_sdl_gl2.h"
#include "ui.h"
#include "inference.h"
#include "network.h"

#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 800

/* ---------------------------------------------------------------------------
 * Cross-platform helpers
 * --------------------------------------------------------------------------- */
static int platform_mkdir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

/* Portable millisecond sleep */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static void platform_sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)ms * 1000);
#endif
}

/* Portable thread join with timeout (ms).
   Returns 0 on success, -1 on timeout. */
static int platform_thread_join_timeout(pthread_t thread, int timeout_ms)
{
    /* Use polling with pthread_kill for portability across glibc, musl,
     * macOS, and Windows. pthread_timedjoin_np is glibc-specific and
     * breaks on Alpine/musl. */
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        if (pthread_kill(thread, 0) == ESRCH) {
            pthread_join(thread, NULL); /* reap it */
            return 0;
        }
        platform_sleep_ms(100);
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * Filesystem helpers
 * --------------------------------------------------------------------------- */

/* Pick (and chdir to) a writable per-user data directory. When the app is
 * launched from /Applications/Wasteland.app on macOS or from a system path
 * on Linux the working directory is `/`, where mkdir/fopen always fail —
 * so all model downloads silently die. We resolve a per-user location and
 * make it the new CWD so the rest of the code (which uses relative paths
 * like "models/foo.gguf" and "chats/...txt") just works.
 *
 * Order of preference:
 *   1. $WASTELAND_HOME (escape hatch — dev / user override)
 *   2. CWD already writable (dev runs from build/ — keep current behaviour)
 *   3. macOS: ~/Library/Application Support/Wasteland
 *      Linux: $XDG_DATA_HOME/wasteland or ~/.local/share/wasteland
 *      Windows: %APPDATA%\Wasteland
 */
static int dir_writable(const char *p)
{
    if (!p || !*p) return 0;
    struct stat st;
    if (stat(p, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return 0;
#ifdef _WIN32
    return 1; /* good enough — actual write test on first download */
#else
    return access(p, W_OK) == 0;
#endif
}

static int ensure_dir(const char *p)
{
    struct stat st;
    if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    return platform_mkdir(p);
}

static void platform_pick_data_dir(char *out, size_t out_size)
{
    out[0] = '\0';

    const char *override = getenv("WASTELAND_HOME");
    if (override && *override) {
        snprintf(out, out_size, "%s", override);
        ensure_dir(out);
        return;
    }

    /* Dev path: if "models" already exists right here OR the cwd is
     * writable AND not the filesystem root, stay put. */
    char cwd[1024] = "";
#ifdef _WIN32
    if (_getcwd(cwd, sizeof(cwd))) {
        if (cwd[0] && strcmp(cwd, "C:\\") != 0 && dir_writable(cwd)) {
            snprintf(out, out_size, "%s", cwd);
            return;
        }
    }
#else
    if (getcwd(cwd, sizeof(cwd))) {
        if (cwd[0] && strcmp(cwd, "/") != 0 && dir_writable(cwd)) {
            snprintf(out, out_size, "%s", cwd);
            return;
        }
    }
#endif

#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    if (appdata && *appdata) {
        snprintf(out, out_size, "%s\\Wasteland", appdata);
        ensure_dir(out);
        return;
    }
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (home && *home) {
        char libdir[1024];
        snprintf(libdir, sizeof(libdir),
                 "%s/Library/Application Support", home);
        ensure_dir(libdir);
        snprintf(out, out_size, "%s/Wasteland", libdir);
        ensure_dir(out);
        return;
    }
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(out, out_size, "%s/wasteland", xdg);
        ensure_dir(out);
        return;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        /* base[] sized smaller than out so the suffix can't truncate. */
        char base[512];
        snprintf(base, sizeof(base), "%s/.local/share", home);
        ensure_dir(base);
        snprintf(out, out_size, "%s/wasteland", base);
        ensure_dir(out);
        return;
    }
#endif
    /* Last resort: temp dir. Better than aborting. */
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp || !*tmp) tmp = getenv("TMP");
    if (!tmp || !*tmp) tmp = "C:\\Windows\\Temp";
    snprintf(out, out_size, "%s\\wasteland", tmp);
#else
    snprintf(out, out_size, "/tmp/wasteland");
#endif
    ensure_dir(out);
}

static void ensure_models_dir(void)
{
    /* Pick (and chdir to) a writable per-user data dir before anything
     * touches the disk. After this returns, all relative paths in the
     * rest of the codebase ("models/...", "chats/...", "system_prompt.txt")
     * resolve under that directory. */
    char data_dir[1024];
    platform_pick_data_dir(data_dir, sizeof(data_dir));
    if (data_dir[0]) {
#ifdef _WIN32
        if (_chdir(data_dir) != 0)
#else
        if (chdir(data_dir) != 0)
#endif
        {
            fprintf(stderr, "[main] chdir(%s) failed: %s\n",
                    data_dir, strerror(errno));
        } else {
            fprintf(stderr, "[main] data dir: %s\n", data_dir);
        }
    }

    struct stat st = {0};
    if (stat("models", &st) == -1) {
        if (platform_mkdir("models") != 0) {
            perror("[main] mkdir(models)");
        }
    }
    if (stat("chats", &st) == -1) {
        if (platform_mkdir("chats") != 0) {
            perror("[main] mkdir(chats)");
        }
    }
}

/* scan_local_models() is now defined in ui.c and declared in ui.h */

/* ---------------------------------------------------------------------------
 * Semver comparison — returns 1 if a > b, 0 otherwise.
 * Accepts "X.Y" or "X.Y.Z" strings with optional leading 'v'.
 * --------------------------------------------------------------------------- */
#ifdef TESTING
int version_newer_than(const char *a, const char *b)
#else
static int version_newer_than(const char *a, const char *b)
#endif
{
    int amaj = 0, amin = 0, apatch = 0;
    int bmaj = 0, bmin = 0, bpatch = 0;
    const char *ap = a;
    const char *bp = b;
    while (*ap && (*ap < '0' || *ap > '9')) ap++;
    while (*bp && (*bp < '0' || *bp > '9')) bp++;
    sscanf(ap, "%d.%d.%d", &amaj, &amin, &apatch);
    sscanf(bp, "%d.%d.%d", &bmaj, &bmin, &bpatch);
    if (amaj != bmaj) return amaj > bmaj;
    if (amin != bmin) return amin > bmin;
    return apatch > bpatch;
}

/* ---------------------------------------------------------------------------
 * Background update check thread
 * --------------------------------------------------------------------------- */
static void* update_check_thread(void *arg)
{
    app_state_t *st = (app_state_t *)arg;
    char latest[32] = {0};
    if (network_check_update(latest, sizeof(latest)) == 0) {
        if (version_newer_than(latest, WASTELAND_VERSION)) {
            snprintf(st->update_version, sizeof(st->update_version),
                     "%s", latest);
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Update download thread
 * --------------------------------------------------------------------------- */
#ifdef TESTING
void build_update_filename(const char *version, char *fname, size_t fsize)
#else
static void build_update_filename(const char *version, char *fname, size_t fsize)
#endif
{
#ifdef _WIN32
    snprintf(fname, fsize, "Wasteland-windows.exe");
#elif defined(__APPLE__)
    snprintf(fname, fsize, "Wasteland-macos.dmg");
#elif defined(__linux__)
#   if defined(__x86_64__)
    snprintf(fname, fsize, "wasteland_%s_amd64.deb", version);
#   elif defined(__aarch64__) || defined(__arm64__)
    snprintf(fname, fsize, "wasteland_%s_arm64.deb", version);
#   else
    snprintf(fname, fsize, "wasteland_%s.deb", version);
#   endif
#else
    fname[0] = '\0';
#endif
}

void* update_download_thread(void *arg)
{
    app_state_t *st = (app_state_t *)arg;
    char fname[256];
    build_update_filename(st->update_version, fname, sizeof(fname));
    if (fname[0] == '\0') {
        st->update_state = 2;
        return NULL;
    }

    char url[1024];
    snprintf(url, sizeof(url),
             "https://github.com/alexbeatnik/wasteland/releases/download/v%s/%s",
             st->update_version, fname);

    /* Ensure downloads/ dir exists */
#ifdef _WIN32
    _mkdir("downloads");
#else
    mkdir("downloads", 0755);
#endif

    st->update_active = 1;
    st->update_progress = 0;
    st->update_cancel = 0;
    int rc = network_download_model(url, "downloads",
                                     &st->update_progress,
                                     &st->update_active,
                                     &st->update_cancel);
    snprintf(st->update_file, sizeof(st->update_file),
             "downloads/%s", fname);
    st->update_state = (rc == 0) ? 1 : 2;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Updater launcher — generates a platform-specific script that waits for
 * this process to exit, replaces the binary, and restarts.
 * --------------------------------------------------------------------------- */
#ifdef _WIN32
void launch_updater(const char *new_file)
{
    char current_exe[MAX_PATH];
    if (!GetModuleFileNameA(NULL, current_exe, MAX_PATH)) return;

    /* Resolve `new_file` to an absolute path. The batch script runs from
     * %TEMP% (set by ShellExecute), so a relative path like
     * "downloads/foo.exe" would not resolve. */
    char abs_new[MAX_PATH];
    if (!_fullpath(abs_new, new_file, sizeof(abs_new))) {
        snprintf(abs_new, sizeof(abs_new), "%s", new_file);
    }

    char batch_path[MAX_PATH];
    GetTempPathA(sizeof(batch_path), batch_path);
    strncat(batch_path, "wst_updater.bat",
            sizeof(batch_path) - strlen(batch_path) - 1);

    DWORD pid = GetCurrentProcessId();

    FILE *f = fopen(batch_path, "w");
    if (!f) return;
    fprintf(f, "@echo off\n");
    fprintf(f, ":wait\n");
    fprintf(f, "timeout /t 1 /nobreak >nul\n");
    fprintf(f, "tasklist /FI \"PID eq %lu\" 2>nul | findstr /C:\"%lu\" >nul\n", pid, pid);
    fprintf(f, "if %%errorlevel%% == 0 goto wait\n");
    /* Try to replace directly; if no write access (Program Files), elevate via runas */
    fprintf(f, "copy /Y \"%s\" \"%s\" >nul 2>&1\n", abs_new, current_exe);
    fprintf(f, "if %%errorlevel%% neq 0 (\n");
    fprintf(f, "    powershell -Command \"Start-Process -Verb runAs -FilePath 'cmd' -ArgumentList '/c copy /Y \\\"%s\\\" \\\"%s\\\" && start \\\"\\\" \\\"%s\\\" && del \\\"%s\\\"'\"\n",
            abs_new, current_exe, current_exe, batch_path);
    fprintf(f, "    exit /b\n");
    fprintf(f, ")\n");
    fprintf(f, "start \"\" \"%s\"\n", current_exe);
    fprintf(f, "del \"%s\"\n", abs_new);
    fprintf(f, "del \"%%~f0\"\n");
    fclose(f);

    ShellExecuteA(NULL, "open", batch_path, NULL, NULL, SW_HIDE);
}
#else
void launch_updater(const char *new_file)
{
    /* POSIX updater shell script */
    char script_path[] = "/tmp/wst_updater.sh";
    char current_exe[1024];
#ifdef __APPLE__
    uint32_t size = sizeof(current_exe);
    if (_NSGetExecutablePath(current_exe, &size) != 0) return;
    /* Find the .app bundle path (e.g. .../Wasteland.app/Contents/MacOS/Wasteland) */
    char *dotapp = strstr(current_exe, ".app/");
    if (dotapp) dotapp[4] = '\0';
#else
    ssize_t len = readlink("/proc/self/exe", current_exe, sizeof(current_exe) - 1);
    if (len <= 0) return;
    current_exe[len] = '\0';
#endif

    /* Resolve new_file to an absolute path. pkexec / osascript / hdiutil
     * all reset CWD (pkexec → /root, osascript → /, hdiutil unclear), so
     * a relative path like "downloads/foo.deb" passed verbatim into the
     * generated script would not resolve. realpath() handles `.` /
     * symlinks; if it fails (file just got deleted), fall back to
     * `getcwd()/relative` which is correct as long as we run before any
     * thread changes CWD. */
    char abs_new[2048];
    {
        char *rp = realpath(new_file, NULL);
        if (rp) {
            snprintf(abs_new, sizeof(abs_new), "%s", rp);
            free(rp);
        } else if (new_file[0] == '/') {
            snprintf(abs_new, sizeof(abs_new), "%s", new_file);
        } else {
            char cwd[1024];
            if (getcwd(cwd, sizeof(cwd))) {
                snprintf(abs_new, sizeof(abs_new), "%s/%s", cwd, new_file);
            } else {
                snprintf(abs_new, sizeof(abs_new), "%s", new_file);
            }
        }
    }

    pid_t pid = getpid();

    FILE *f = fopen(script_path, "w");
    if (!f) return;
    fprintf(f, "#!/bin/bash\n");
    fprintf(f, "PID=%d\n", (int)pid);
    fprintf(f, "NEW=\"%s\"\n", abs_new);
    fprintf(f, "OLD=\"%s\"\n", current_exe);
    fprintf(f, "while kill -0 \"$PID\" 2>/dev/null; do sleep 1; done\n");

#ifdef __APPLE__
    /* macOS: mount DMG, copy .app, open */
    fprintf(f, "MOUNT=$(hdiutil attach \"$NEW\" | grep Volumes | awk '{print $3}')\n");
    fprintf(f, "APP_SRC=\"$MOUNT/Wasteland.app\"\n");
    fprintf(f, "APP_DST=\"$(dirname \"$OLD\")\"\n");
    fprintf(f, "if [ -w \"$APP_DST\" ]; then\n");
    fprintf(f, "    rm -rf \"$OLD\"\n");
    fprintf(f, "    cp -R \"$APP_SRC\" \"$APP_DST/\"\n");
    fprintf(f, "    open \"$APP_DST/Wasteland.app\"\n");
    fprintf(f, "else\n");
    fprintf(f, "    osascript -e \"do shell script \\\"rm -rf '$OLD' && cp -R '$APP_SRC' '$APP_DST/'\\\" with administrator privileges\"\n");
    fprintf(f, "    open \"$APP_DST/Wasteland.app\"\n");
    fprintf(f, "fi\n");
    fprintf(f, "hdiutil detach \"$MOUNT\"\n");
#else
    /* Linux: install the .deb via dpkg. NEW is a Debian package archive
     * (ar format containing data.tar), NOT an ELF binary — the obvious
     * `cp $NEW $OLD` would replace the running executable with a tarball
     * and brick the install. dpkg -i does the right thing: extracts to
     * /usr/bin/Wasteland, runs maintainer scripts, registers in dpkg DB.
     *
     * Elevation: prefer pkexec (GUI password dialog from policykit, ships
     * by default on Ubuntu/Debian/Mint with a desktop environment). Fall
     * back to a notification telling the user the manual command — better
     * than a silent failure they can't diagnose. */
    fprintf(f, "INSTALLED=\"/usr/bin/Wasteland\"\n");
    fprintf(f, "RC=1\n");
    fprintf(f, "if command -v pkexec >/dev/null 2>&1; then\n");
    fprintf(f, "    pkexec dpkg -i \"$NEW\"\n");
    fprintf(f, "    RC=$?\n");
    fprintf(f, "elif command -v gksudo >/dev/null 2>&1; then\n");
    fprintf(f, "    gksudo \"dpkg -i $NEW\"\n");
    fprintf(f, "    RC=$?\n");
    fprintf(f, "elif command -v kdesu >/dev/null 2>&1; then\n");
    fprintf(f, "    kdesu -c \"dpkg -i $NEW\"\n");
    fprintf(f, "    RC=$?\n");
    fprintf(f, "fi\n");
    fprintf(f, "if [ \"$RC\" -eq 0 ] && [ -x \"$INSTALLED\" ]; then\n");
    fprintf(f, "    \"$INSTALLED\" &\n");
    fprintf(f, "else\n");
    fprintf(f, "    MSG=\"Wasteland update could not be installed automatically.\\n\\nRun manually:\\n  sudo dpkg -i $NEW\"\n");
    fprintf(f, "    if command -v notify-send >/dev/null 2>&1; then\n");
    fprintf(f, "        notify-send \"Wasteland Update\" \"$MSG\"\n");
    fprintf(f, "    elif command -v zenity >/dev/null 2>&1; then\n");
    fprintf(f, "        zenity --info --title=\"Wasteland Update\" --text=\"$MSG\"\n");
    fprintf(f, "    elif command -v xmessage >/dev/null 2>&1; then\n");
    fprintf(f, "        xmessage \"$MSG\"\n");
    fprintf(f, "    fi\n");
    fprintf(f, "fi\n");
#endif
    /* Only delete the downloaded artefact if the install actually succeeded.
     * On failure we keep it around so the user can follow the manual-install
     * notification (RC == 0 is set inside the install branch on Linux; on
     * macOS we treat any reach-this-point as success — the osascript / cp
     * branches both block until done and would have errored visibly). */
#ifdef __linux__
    fprintf(f, "if [ \"$RC\" -eq 0 ]; then rm -f \"$NEW\"; fi\n");
#else
    fprintf(f, "rm -f \"$NEW\"\n");
#endif
    fprintf(f, "rm -f \"%s\"\n", script_path);
    fclose(f);
    chmod(script_path, 0755);

    /* Launch detached */
    if (fork() == 0) {
        setsid();
        execl("/bin/sh", "sh", script_path, (char *)NULL);
        _Exit(1);
    }
}
#endif

/* ---------------------------------------------------------------------------
 * main()
 * --------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* Must run before SDL_Init / CreateWindow so SDL queries the monitor at
     * its real DPI and reports physical pixel sizes. No-op on non-Windows. */
    platform_enable_dpi_awareness();

    /* ---- Pre-SDL: load model so llama.cpp mmap does not break NVIDIA GL ---- */
    ensure_models_dir();
    char models_tmp[WASTELAND_MAX_MODELS][WASTELAND_MAX_MODEL_PATH_LEN] = {0};
    int model_count = scan_local_models(models_tmp, WASTELAND_MAX_MODELS);

    inference_ctx_t *inference = inference_init();
    if (!inference) {
        fprintf(stderr, "[main] inference_init failed\n");
        return EXIT_FAILURE;
    }

    /* No auto-load and no auto-lockdown on boot. The seccomp filter is
     * irreversible for the lifetime of the process, so applying it before
     * the user has a chance to download is a one-way trap. The user picks
     * a model (and thereby triggers lockdown) from the UI after launch. */
    char status_msg[256] = "";
    int  selected_model  = -1;

    /* Force X11 on Linux to avoid NVIDIA EGL/Wayland issues. The hint macro
     * SDL_HINT_VIDEODRIVER was only added in SDL 2.0.22; Ubuntu 22.04 ships
     * SDL 2.0.20, so we use the underlying string name (stable since 2.0.0)
     * to keep the build portable across distros. */
    #ifdef __linux__
    SDL_SetHint("SDL_VIDEODRIVER", "x11");
    #endif

    /* ---- SDL2 ---- */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[main] SDL_Init error: %s\n", SDL_GetError());
        inference_shutdown(inference);
        return EXIT_FAILURE;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_Window *win = SDL_CreateWindow(
        "Wasteland Terminal",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_WIDTH, WINDOW_HEIGHT,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        fprintf(stderr, "[main] SDL_CreateWindow error: %s\n", SDL_GetError());
        inference_shutdown(inference);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
    if (!gl_ctx) {
        fprintf(stderr, "[main] SDL_GL_CreateContext error: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        inference_shutdown(inference);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_GL_SetSwapInterval(1); /* VSync */
    SDL_ShowWindow(win);

    /* ---- Nuklear + SDL2 + OpenGL2 backend ---- */
    /* Scale the base 15 px font by the drawable/logical pixel ratio so text
     * appears the same logical size on standard, Retina, and Windows HiDPI
     * displays.  SDL_GL_GetDrawableSize returns physical pixels;
     * SDL_GetWindowSize returns logical pixels. */
    float dpi_scale;
    {
        int lw = 0, dw = 0;
        SDL_GetWindowSize(win, &lw, NULL);
        SDL_GL_GetDrawableSize(win, &dw, NULL);
        dpi_scale = (lw > 0 && dw > 0) ? (float)dw / (float)lw : 1.0f;

        /* On Windows with per-monitor DPI awareness the drawable/logical
         * ratio is sometimes 1.0 even though the display is scaled (SDL does
         * not separate the two for OpenGL windows in every configuration).
         * Fall back to the system-reported DPI so text stays readable. */
        if (dpi_scale <= 1.01f) {
            float ddpi = 96.0f;
            int display = SDL_GetWindowDisplayIndex(win);
            if (display < 0) display = 0;
            if (SDL_GetDisplayDPI(display, &ddpi, NULL, NULL) == 0 && ddpi > 96.0f) {
                dpi_scale = ddpi / 96.0f;
            }
        }

        if (dpi_scale < 1.0f) dpi_scale = 1.0f;
        if (dpi_scale > 4.0f) dpi_scale = 4.0f;
    }
    struct nk_context *nk = nk_sdl_init(win, 15.0f * dpi_scale);
    if (!nk) {
        fprintf(stderr, "[main] nk_sdl_init failed\n");
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(win);
        inference_shutdown(inference);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    /* ---- Application state ---- */
    app_state_t state = {0};
    state.running = 1;
    state.selected_model = selected_model;
    pthread_mutex_init(&state.chat_mutex, NULL);
    state.model_count = model_count;
    memcpy(state.models, models_tmp, sizeof(models_tmp));
    
    char chats_tmp[WASTELAND_MAX_CHATS][WASTELAND_CHAT_NAME_LEN] = {0};
    state.chat_count = scan_local_chats(chats_tmp, WASTELAND_MAX_CHATS);
    memcpy(state.chats, chats_tmp, sizeof(chats_tmp));
    if (state.chat_count > 0) {
        state.selected_chat = 0;
        load_chat_history(state.chats[0], state.chat_history, WASTELAND_MAX_CHAT_HISTORY);
        state.chat_last_len = strlen(state.chat_history);
        state.chat_scroll_y = (nk_uint)0x7FFFFFFF;
    } else {
        state.selected_chat = -1;
    }

    state.selected_hub_model = -1;
    state.loading_model_index = -1;
    state.custom_hf_id[0] = '\0';
    state.download_cancel = 0;
    state.inference = inference;
    state.context_tokens = 0;
    state.context_max = 0;
    state.settings_n_ctx       = 4096;
    state.settings_temperature = 0.8f;
    strncpy(state.status_msg, status_msg, sizeof(state.status_msg) - 1);
    state.update_version[0]    = '\0';

    /* Load system prompt */
    state.system_prompt[0] = '\0';
    FILE *sys_fp = fopen("system_prompt.txt", "r");
    if (sys_fp) {
        size_t n = fread(state.system_prompt, 1, sizeof(state.system_prompt) - 1, sys_fp);
        state.system_prompt[n] = '\0';
        fclose(sys_fp);
    }
    char last_system_prompt[1024];
    strcpy(last_system_prompt, state.system_prompt);

    /* Load persistent agent settings (mode toggle + workspace path).
     * Stored as a tiny key=value file alongside system_prompt.txt. */
    state.agent_mode = 0;
    state.agent_workspace[0] = '\0';
    FILE *cfg_fp = fopen("wasteland.cfg", "r");
    if (cfg_fp) {
        char line[1280];
        while (fgets(line, sizeof(line), cfg_fp)) {
            /* Trim trailing newline / CR */
            size_t L = strlen(line);
            while (L > 0 && (line[L-1] == '\n' || line[L-1] == '\r'))
                line[--L] = '\0';
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            const char *key = line;
            const char *val = eq + 1;
            if (strcmp(key, "agent_mode") == 0) {
                state.agent_mode = (val[0] == '1') ? 1 : 0;
            } else if (strcmp(key, "agent_workspace") == 0) {
                strncpy(state.agent_workspace, val,
                        sizeof(state.agent_workspace) - 1);
                state.agent_workspace[sizeof(state.agent_workspace) - 1] = '\0';
            } else if (strcmp(key, "n_ctx") == 0) {
                int v = atoi(val);
                if (v >= 512 && v <= 262144) state.settings_n_ctx = v;
            } else if (strcmp(key, "temperature") == 0) {
                float v = (float)atof(val);
                if (v >= 0.01f && v <= 5.0f) state.settings_temperature = v;
            }
        }
        fclose(cfg_fp);
    }
    int  last_agent_mode = state.agent_mode;
    char last_agent_ws[1024];
    snprintf(last_agent_ws, sizeof(last_agent_ws), "%s", state.agent_workspace);
    int   last_n_ctx       = state.settings_n_ctx;
    float last_temperature = state.settings_temperature;

    /* Push initial settings into the inference module so the very first
     * model load (triggered by the user clicking [LOAD]) sees them. */
    inference_set_n_ctx(state.inference,       state.settings_n_ctx);
    inference_set_temperature(state.inference, state.settings_temperature);

    /* Do not auto-load model on boot — loading llama.cpp model breaks
       NVIDIA GL context. Model must be loaded manually via UI after
       the window is visible. */
    if (state.model_count > 0) {
        snprintf(state.status_msg, sizeof(state.status_msg),
                 "%d model(s) found. Select one to load.", state.model_count);
    } else {
        snprintf(state.status_msg, sizeof(state.status_msg),
                 "No models found. Place .gguf files in ./models/ or download.");
    }

    /* ---- Background update check (before lockdown, so curl can open sockets) ---- */
    pthread_t update_thread;
    if (pthread_create(&update_thread, NULL, update_check_thread, &state) == 0) {
        pthread_detach(update_thread);
    }

    /* ---- Spawn inference worker thread ---- */
    pthread_t worker_thread;
    if (pthread_create(&worker_thread, NULL,
                       inference_worker_thread, state.inference) != 0) {
        fprintf(stderr, "[main] Failed to create worker thread\n");
        inference_shutdown(state.inference);
        nk_sdl_shutdown();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    /* -----------------------------------------------------------------------
     * Main loop
     * ----------------------------------------------------------------------- */
    int startup_frames = 30; /* ignore SDL_QUIT for first ~500 ms */
    while (state.running) {
        /* --- Input --- */
        SDL_Event evt;
        nk_input_begin(nk);
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT && startup_frames <= 0)
                state.running = 0;
            nk_sdl_handle_event(&evt);
        }
        if (startup_frames > 0) startup_frames--;
        nk_input_end(nk);

        /* Push current agent settings to the inference module each frame so
         * the worker sees them at the moment the next prompt is dequeued. */
        inference_set_agent(state.inference, state.agent_mode,
                            state.agent_workspace);
        inference_set_n_ctx(state.inference,       state.settings_n_ctx);
        inference_set_temperature(state.inference, state.settings_temperature);

        /* --- Drain inference output into chat history --- */
        char chunk[1024];
        size_t n = inference_read_output(state.inference, chunk, sizeof(chunk));
        if (n > 0) {
            pthread_mutex_lock(&state.chat_mutex);
            size_t hist_len = strlen(state.chat_history);
            size_t room = WASTELAND_MAX_CHAT_HISTORY - hist_len - 1;
            if (room > 0) {
                if (n > room) n = room;
                memcpy(state.chat_history + hist_len, chunk, n);
                state.chat_history[hist_len + n] = '\0';
            }
            pthread_mutex_unlock(&state.chat_mutex);
        }
        state.is_generating = inference_is_generating(state.inference);

        /* --- Handle completed background downloads --- */
        if (state.download_complete_flag) {
            state.download_complete_flag = 0;
            if (state.download_success) {
                state.model_count = scan_local_models(
                    state.models, WASTELAND_MAX_MODELS);
                snprintf(state.status_msg, sizeof(state.status_msg),
                         "Download complete. %d model(s) available.",
                         state.model_count);
            } else {
                snprintf(state.status_msg, sizeof(state.status_msg),
                         "Download failed.");
            }
        }

        /* --- UI layout --- */
        /* Auto-save system prompt if changed */
        if (strcmp(state.system_prompt, last_system_prompt) != 0) {
            strcpy(last_system_prompt, state.system_prompt);
            FILE *f = fopen("system_prompt.txt", "w");
            if (f) {
                fputs(state.system_prompt, f);
                fclose(f);
            }
        }

        /* Persist agent toggle + workspace + tunables whenever they change.
         * Tiny file so rewriting on every change is fine. */
        if (state.agent_mode != last_agent_mode ||
            strcmp(state.agent_workspace, last_agent_ws) != 0 ||
            state.settings_n_ctx       != last_n_ctx ||
            state.settings_temperature != last_temperature)
        {
            last_agent_mode    = state.agent_mode;
            last_n_ctx         = state.settings_n_ctx;
            last_temperature   = state.settings_temperature;
            snprintf(last_agent_ws, sizeof(last_agent_ws),
                     "%s", state.agent_workspace);
            FILE *f = fopen("wasteland.cfg", "w");
            if (f) {
                fprintf(f, "agent_mode=%d\n",      state.agent_mode);
                fprintf(f, "agent_workspace=%s\n", state.agent_workspace);
                fprintf(f, "n_ctx=%d\n",           state.settings_n_ctx);
                fprintf(f, "temperature=%.3f\n",   state.settings_temperature);
                fclose(f);
            }
        }

        int win_w, win_h;
        SDL_GetWindowSize(win, &win_w, &win_h);
        ui_render(nk, &state, win_w, win_h);

        /* --- Render --- */
        glViewport(0, 0, win_w, win_h);
        glClearColor(0.05f, 0.05f, 0.05f, 1.0f); /* very dark charcoal */
        glClear(GL_COLOR_BUFFER_BIT);

        nk_sdl_render(NK_ANTI_ALIASING_ON, 512 * 1024, 128 * 1024);

        SDL_GL_SwapWindow(win);
    }

    /* -----------------------------------------------------------------------
     * Teardown
     *
     * Goal: window disappears the instant the user clicks X. We hide it
     * first, then make a brief best-effort attempt to drain the worker so
     * llama.cpp can free its context cleanly. If the worker is mid-decode
     * (or a model load is still in flight), we just exit — the OS reclaims
     * memory and kills the threads, which is safe because nothing in our
     * code path can run between _Exit and process termination.
     * ----------------------------------------------------------------------- */
    state.running = 0;
    
    /* Save current chat on exit */
    if (state.selected_chat >= 0 && state.selected_chat < state.chat_count) {
        save_chat_history(state.chats[state.selected_chat], state.chat_history);
    }
    
    SDL_HideWindow(win);

    /* Tell the worker to stop and wake any cond-wait. We do NOT submit an
     * empty prompt — with a model loaded, the chat template would still
     * produce non-empty tokens and the worker would burn a generation
     * cycle on the way out. */
    inference_request_stop(state.inference);

    int join_ok = platform_thread_join_timeout(worker_thread, 1500);
    if (join_ok == 0) {
        inference_shutdown(state.inference);
        pthread_mutex_destroy(&state.chat_mutex);
        nk_sdl_shutdown();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return EXIT_SUCCESS;
    }

    fprintf(stderr, "[main] Worker still busy, fast-exit.\n");
    SDL_Quit();
    _Exit(0);
}
