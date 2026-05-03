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
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  include <shellscalingapi.h>  /* SetProcessDpiAwareness fallback */
#  ifndef S_ISDIR
#    define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#  endif
#else
#  include <unistd.h>
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
#ifdef __linux__
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    int rc = pthread_timedjoin_np(thread, NULL, &ts);
    return (rc == 0) ? 0 : -1;
#else
    /* macOS / Windows / others: poll with short sleeps */
    for (int waited = 0; waited < timeout_ms; waited += 100) {
        /* pthread_kill(t,0) returns 0 if alive, ESRCH if dead */
        if (pthread_kill(thread, 0) == ESRCH) {
            pthread_join(thread, NULL); /* reap it */
            return 0;
        }
        platform_sleep_ms(100);
    }
    return -1;
#endif
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
    /* Last resort: tmp. Better than aborting. */
    snprintf(out, out_size, "/tmp/wasteland");
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

    int wx, wy, ww, wh;
    (void)wx; (void)wy; (void)ww; (void)wh;

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
    struct nk_context *nk = nk_sdl_init(win);
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
