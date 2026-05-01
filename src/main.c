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

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <GL/gl.h>
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
#else
#  include <unistd.h>
#  include <dirent.h>
#endif

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
static void ensure_models_dir(void)
{
    struct stat st = {0};
    if (stat("models", &st) == -1) {
        if (platform_mkdir("models") != 0) {
            perror("[main] mkdir(models)");
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

    /* ---- Pre-SDL: load model so llama.cpp mmap does not break NVIDIA GL ---- */
    ensure_models_dir();
    char models_tmp[WASTELAND_MAX_MODELS][WASTELAND_MAX_MODEL_PATH_LEN] = {0};
    int model_count = scan_local_models(models_tmp, WASTELAND_MAX_MODELS);

    inference_ctx_t *inference = inference_init();
    if (!inference) {
        fprintf(stderr, "[main] inference_init failed\n");
        return EXIT_FAILURE;
    }

    char status_msg[256] = "";
    int selected_model = -1;
    if (model_count > 0) {
        selected_model = 0;
        struct stat fst;
        char sz_str[32] = "";
        if (stat(models_tmp[0], &fst) == 0) {
            if (fst.st_size >= 1024LL * 1024 * 1024)
                snprintf(sz_str, sizeof(sz_str), "%.2f GB",
                         fst.st_size / (1024.0 * 1024 * 1024));
            else
                snprintf(sz_str, sizeof(sz_str), "%.2f MB",
                         fst.st_size / (1024.0 * 1024));
        }
        if (inference_load_model(inference, models_tmp[0]) == 0) {
            if (lockdown_network() == 0) {
                snprintf(status_msg, sizeof(status_msg),
                         "Loaded %s. Lockdown active.", sz_str);
            } else {
                snprintf(status_msg, sizeof(status_msg),
                         "Loaded %s. Lockdown FAILED!", sz_str);
            }
        } else {
            snprintf(status_msg, sizeof(status_msg),
                     "Failed to load model (%s).", sz_str);
        }
    } else {
        snprintf(status_msg, sizeof(status_msg),
                 "No models found. Place .gguf files in ./models/ or download.");
    }

    /* Force X11 on Linux to avoid NVIDIA EGL/Wayland issues */
    #ifdef __linux__
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
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
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
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
    state.selected_hub_model = -1;
    state.loading_model_index = -1;
    state.custom_hf_id[0] = '\0';
    state.download_cancel = 0;
    state.inference = inference;
    strncpy(state.status_msg, status_msg, sizeof(state.status_msg) - 1);

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
    SDL_HideWindow(win);

    /* Wake the worker so any cond-wait returns. */
    inference_submit_prompt(state.inference, "");

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
