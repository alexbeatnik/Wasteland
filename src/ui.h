#ifndef WASTELAND_UI_H
#define WASTELAND_UI_H

/* ---------------------------------------------------------------------------
 * Wasteland UI Module
 *
 * Defines the shared application state and the Nuklear layout renderer.
 * --------------------------------------------------------------------------- */

#include <pthread.h>

/* Nuklear configuration macros must be defined before every include. */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#include <nuklear.h>

#include "inference.h"

#define WASTELAND_MAX_CHAT_HISTORY 65536
#define WASTELAND_MAX_MODELS       64
#define WASTELAND_MAX_MODEL_PATH_LEN 512
#define WASTELAND_MAX_PROMPT_LEN   4096
#define WASTELAND_MAX_HUB_MODELS   4

/**
 * @brief Global application state shared between UI and worker threads.
 *
 * All mutable fields that are touched from more than one thread are protected
 * by explicit mutexes (chat_mutex, or the internal locks inside inference_ctx_t).
 */
typedef struct {
    char chat_history[WASTELAND_MAX_CHAT_HISTORY];
    pthread_mutex_t chat_mutex;

    char input_buffer[WASTELAND_MAX_PROMPT_LEN];

    /* Local model vault */
    char models[WASTELAND_MAX_MODELS][WASTELAND_MAX_MODEL_PATH_LEN];
    int  model_count;
    int  selected_model; /* index of currently loaded model, -1 if none */

    /* Hub / download state */
    int  selected_hub_model; /* 0..WASTELAND_MAX_HUB_MODELS-1, -1 if none */
    char custom_hf_id[256];
    char hf_model_id[256];   /* ID passed to downloader */

    int  network_lockdown;
    int  is_generating;
    int  download_progress;      /* 0-100 */
    int  download_active;
    int  download_cancel;        /* set by UI, read by worker */
    int  download_complete_flag; /* set by download thread, cleared by main */
    int  download_success;
    char status_msg[256];

    inference_ctx_t *inference;

    volatile int running;
} app_state_t;

/* Scan models/ directory for .gguf files. Returns count (0..max_models). */
int scan_local_models(char models_list[][WASTELAND_MAX_MODEL_PATH_LEN], int max_models);

/* Apply the retro amber-on-black Pip-Boy aesthetic. */
void ui_apply_amber_theme(struct nk_context *nk);

/* Render one complete frame of the UI. Called from the main thread. */
void ui_render(struct nk_context *nk, app_state_t *state, int width, int height);

#endif /* WASTELAND_UI_H */
