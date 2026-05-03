#ifndef WASTELAND_INFERENCE_H
#define WASTELAND_INFERENCE_H

/* ---------------------------------------------------------------------------
 * Wasteland Inference Engine
 *
 * Thin thread-safe wrapper around llama.cpp (llama.h C API).
 * --------------------------------------------------------------------------- */

#include <stddef.h>

#define WASTELAND_MAX_CHAT_HISTORY 65536

/* Opaque handle. */
typedef struct inference_ctx inference_ctx_t;

/* Lifecycle --------------------------------------------------------------- */
inference_ctx_t* inference_init(void);
void             inference_shutdown(inference_ctx_t *ctx);

/* Model management -------------------------------------------------------- */
int  inference_load_model(inference_ctx_t *ctx, const char *path);
void inference_unload_model(inference_ctx_t *ctx);
int  inference_is_model_loaded(inference_ctx_t *ctx);

/* Async load. Returns 0 if a load was started, -1 if one is already in
 * flight or args are bad. UI polls inference_is_loading(); once it goes
 * back to 0, inference_take_load_result() yields 1 (success), -1 (fail),
 * or 0 (no pending result). Taking the result clears it. */
int  inference_load_model_async(inference_ctx_t *ctx, const char *path);
int  inference_is_loading(inference_ctx_t *ctx);
int  inference_take_load_result(inference_ctx_t *ctx);

/* Prompt / output --------------------------------------------------------- */
void   inference_submit_prompt(inference_ctx_t *ctx, const char *sys_prompt, const char *prompt);
size_t inference_read_output(inference_ctx_t *ctx, char *buf, size_t size);
int    inference_is_generating(inference_ctx_t *ctx);

/* Pass the full chat history so the worker can feed previous turns to the
 * model. Must be called before inference_submit_prompt(). */
void   inference_set_chat_history(inference_ctx_t *ctx, const char *history);

/* Tokenise the current chat history and return usage. Returns 0 on success,
 * -1 if no model is loaded. */
int    inference_get_context_stats(inference_ctx_t *ctx,
                                   const char *history,
                                   int *tokens_out,
                                   int *max_out);

/* Tunables. Defaults: n_ctx=4096, temperature=0.8. n_ctx takes effect on the
 * NEXT model load (changing it for an already-loaded model would require
 * re-allocating the KV cache). temperature is read at the start of every
 * generation so it can be tweaked between turns. */
void   inference_set_n_ctx(inference_ctx_t *ctx, int n_ctx);
void   inference_set_temperature(inference_ctx_t *ctx, float temperature);
int    inference_get_n_ctx(inference_ctx_t *ctx);
float  inference_get_temperature(inference_ctx_t *ctx);

/* Request the worker to stop emitting tokens for the current prompt.
 * Has no effect if no generation is in flight. */
void   inference_cancel_generation(inference_ctx_t *ctx);

/* Tell the worker to exit its loop and unblock any cond_wait. Safe to call
 * from any thread; intended for shutdown. After this returns the worker is
 * either already exiting or about to. */
void   inference_request_stop(inference_ctx_t *ctx);

/* Agent mode -------------------------------------------------------------- */

/**
 * Configure agent mode for the next prompt. Set `mode` to 1 to enable the
 * tool-using ReAct loop with sandboxing under `workspace`; set to 0 to
 * fall back to plain single-shot chat. Safe to call between prompts.
 */
void   inference_set_agent(inference_ctx_t *ctx, int mode, const char *workspace);

/**
 * Probe whether the worker is currently waiting for the user to approve a
 * mutating tool call. If yes, returns the pending tool kind (1=write_file,
 * 2=apply_edit) and fills the *_out args (pointers stay valid until the
 * next call to inference_set_pending_approval). Returns 0 if nothing pending.
 *
 * The strings remain owned by the inference module — do NOT free them.
 */
int    inference_get_pending(inference_ctx_t *ctx,
                             const char **path_out,
                             const char **content_out,   /* may be NULL */
                             const char **search_out,    /* may be NULL */
                             const char **replace_out);  /* may be NULL */

/**
 * Resolve a pending approval. `decision` = +1 to APPLY, -1 to REJECT.
 * Wakes the worker out of its approval-wait poll. No-op if no pending action.
 */
void   inference_set_pending_approval(inference_ctx_t *ctx, int decision);

/* Worker thread entry point ----------------------------------------------- */
void* inference_worker_thread(void *arg);

#endif /* WASTELAND_INFERENCE_H */
