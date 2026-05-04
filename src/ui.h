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

#define WASTELAND_MAX_MODELS       64
#define WASTELAND_MAX_MODEL_PATH_LEN 512
#define WASTELAND_MAX_PROMPT_LEN   4096
#define WASTELAND_MAX_HUB_MODELS   5
#define WASTELAND_MAX_CHATS        64
#define WASTELAND_CHAT_NAME_LEN    256

/* Agent-mode UI buffer sizes (mirrored from agent.h to avoid the include
 * chain dragging agent internals into every UI compilation unit). */
#define AGENT_MAX_PATH_LEN_UI      1024

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
    char system_prompt[1024];

    /* Local model vault */
    char models[WASTELAND_MAX_MODELS][WASTELAND_MAX_MODEL_PATH_LEN];
    int  model_count;
    int  selected_model;        /* index of currently loaded model, -1 if none */
    int  loading_model_index;   /* index being loaded async, -1 if none */

    /* Hub / download state */
    int  selected_hub_model; /* 0..WASTELAND_MAX_HUB_MODELS-1, -1 if none */
    char custom_hf_id[256];
    char hf_model_id[256];   /* ID passed to downloader */

    int  network_lockdown;
    int  is_generating;
    int  left_panel_collapsed; /* 1 = left panel hidden, 0 = visible */

    /* Chat scroll state — kept for compatibility with old chat-rendering
     * code paths; nk_edit_string manages its own scrollbar internally now. */
    nk_uint chat_scroll_x;
    nk_uint chat_scroll_y;
    size_t  chat_last_len;

    /* Multiple chats */
    char chats[WASTELAND_MAX_CHATS][WASTELAND_CHAT_NAME_LEN];
    int  chat_count;
    int  selected_chat; /* index, -1 if no chat loaded */
    /* Cross-thread one-way flags — written by the detached download
     * pthread, read by the main UI thread (or vice versa for cancel). */
    volatile int download_progress;      /* 0-100 */
    volatile int download_active;
    volatile int download_cancel;        /* set by UI, read by worker */
    volatile int download_complete_flag; /* set by download thread, cleared by main */
    volatile int download_success;
    char status_msg[256];
    char last_status_msg[256];
    unsigned int status_timer;

    inference_ctx_t *inference;

    /* ---- Agent mode -----------------------------------------------------
     * When `agent_mode` is set, the worker injects a tools system prompt and
     * starts a multi-turn ReAct loop bounded to ~10 iterations. All file I/O
     * is sandboxed under `agent_workspace` (see agent.c → agent_resolve_path).
     *
     * Approval flow for mutating tools (write_file, apply_edit):
     *   1. Worker fills agent_pending_* and sets agent_pending = N.
     *   2. UI renders the proposed action with [APPLY]/[REJECT] buttons.
     *   3. User clicks → UI sets agent_approval = +1 (apply) or -1 (reject).
     *   4. Worker observes, executes (or skips), clears pending, continues. */
    int  agent_mode;                       /* 0=off, 1=on */
    char agent_workspace[1024];            /* user-set sandbox root */

    volatile int   agent_pending;          /* 0=none, 1=write_file, 2=apply_edit */
    char           agent_pending_path[AGENT_MAX_PATH_LEN_UI];
    char          *agent_pending_content;  /* heap, write_file body */
    char          *agent_pending_search;   /* heap, apply_edit search */
    char          *agent_pending_replace;  /* heap, apply_edit replace */
    volatile int   agent_approval;         /* 0=waiting, +1=apply, -1=reject */

    /* Context usage tracking (updated after each generation) */
    int context_tokens;
    int context_max;

    /* User-tunable runtime settings (mirrored into inference_ctx_t each frame
     * via inference_set_n_ctx / inference_set_temperature). n_ctx only takes
     * effect on the next model load. */
    int   settings_n_ctx;
    float settings_temperature;

    volatile int running;
} app_state_t;

/* Scan models/ directory for .gguf files. Returns count (0..max_models). */
int scan_local_models(char models_list[][WASTELAND_MAX_MODEL_PATH_LEN], int max_models);

/* Scan chats/ directory for .txt files. Returns count. */
int scan_local_chats(char chats_list[][WASTELAND_CHAT_NAME_LEN], int max_chats);

/* Save and load chat history */
void save_chat_history(const char *chat_name, const char *history);
void load_chat_history(const char *chat_name, char *history, size_t max_len);

/* Apply the retro amber-on-black vintage PC terminal aesthetic. */
void ui_apply_amber_theme(struct nk_context *nk);

/* Render one complete frame of the UI. Called from the main thread. */
void ui_render(struct nk_context *nk, app_state_t *state, int width, int height);

#endif /* WASTELAND_UI_H */
