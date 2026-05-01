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
#include <dirent.h>
#include <sys/stat.h>

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
 * Filesystem helpers
 * --------------------------------------------------------------------------- */
static void ensure_models_dir(void)
{
    struct stat st = {0};
    if (stat("models", &st) == -1) {
        if (mkdir("models", 0755) != 0) {
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

    /* ---- SDL2 ---- */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "[main] SDL_Init error: %s\n", SDL_GetError());
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
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) {
        fprintf(stderr, "[main] SDL_CreateWindow error: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(win);
    if (!gl_ctx) {
        fprintf(stderr, "[main] SDL_GL_CreateContext error: %s\n", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    SDL_GL_SetSwapInterval(1); /* VSync — caps us roughly at 60 FPS */

    /* ---- Nuklear + SDL2 + OpenGL2 backend ---- */
    struct nk_context *nk = nk_sdl_init(win);
    if (!nk) {
        fprintf(stderr, "[main] nk_sdl_init failed\n");
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    /* ---- Application state ---- */
    app_state_t state = {0};
    state.running = 1;
    state.selected_model = -1;
    pthread_mutex_init(&state.chat_mutex, NULL);

    ensure_models_dir();
    state.model_count = scan_local_models(state.models, WASTELAND_MAX_MODELS);
    state.selected_hub_model = -1;
    state.custom_hf_id[0] = '\0';
    state.download_cancel = 0;

    /* ---- Inference engine ---- */
    state.inference = inference_init();
    if (!state.inference) {
        fprintf(stderr, "[main] inference_init failed\n");
        nk_sdl_shutdown();
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return EXIT_FAILURE;
    }

    /* If a local model exists on boot: load it and immediately lockdown. */
    if (state.model_count > 0) {
        state.selected_model = 0;
        struct stat fst;
        char sz_str[32] = "";
        if (stat(state.models[0], &fst) == 0) {
            if (fst.st_size >= 1024LL * 1024 * 1024)
                snprintf(sz_str, sizeof(sz_str), "%.2f GB",
                         fst.st_size / (1024.0 * 1024 * 1024));
            else
                snprintf(sz_str, sizeof(sz_str), "%.2f MB",
                         fst.st_size / (1024.0 * 1024));
        }
        if (inference_load_model(state.inference, state.models[0]) == 0) {
            if (lockdown_network() == 0) {
                snprintf(state.status_msg, sizeof(state.status_msg),
                         "Loaded %s. Lockdown active.", sz_str);
            } else {
                snprintf(state.status_msg, sizeof(state.status_msg),
                         "Loaded %s. Lockdown FAILED!", sz_str);
            }
        } else {
            snprintf(state.status_msg, sizeof(state.status_msg),
                     "Failed to load model (%s).", sz_str);
        }
    } else {
        strncpy(state.status_msg,
                "No models found. Place .gguf files in ./models/ or download.",
                sizeof(state.status_msg) - 1);
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
    while (state.running) {
        /* --- Input --- */
        SDL_Event evt;
        nk_input_begin(nk);
        while (SDL_PollEvent(&evt)) {
            if (evt.type == SDL_QUIT)
                state.running = 0;
            nk_sdl_handle_event(&evt);
        }
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
     * ----------------------------------------------------------------------- */
    state.running = 0;

    /* Wake the worker so it can exit its cond-wait. */
    inference_submit_prompt(state.inference, "");

    /* Wait up to 10 seconds for graceful worker exit.
       Do NOT use pthread_cancel — llama.cpp contains C++ code (allocators,
       mutexes) that is not async-cancel-safe and will SIGABRT if interrupted. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 10;
    int join_err = pthread_timedjoin_np(worker_thread, NULL, &ts);
    if (join_err == ETIMEDOUT) {
        fprintf(stderr, "[main] Worker did not finish in 10s, detaching thread.\n");
        pthread_detach(worker_thread);
        /* Skip inference_shutdown() — worker may still touch ctx/model.
           The OS reclaims all memory when the process exits. */
    } else {
        inference_shutdown(state.inference);
    }
    pthread_mutex_destroy(&state.chat_mutex);

    nk_sdl_shutdown();
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();

    return EXIT_SUCCESS;
}
