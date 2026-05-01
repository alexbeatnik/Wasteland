#ifndef WASTELAND_INFERENCE_H
#define WASTELAND_INFERENCE_H

/* ---------------------------------------------------------------------------
 * Wasteland Inference Engine
 *
 * Thin thread-safe wrapper around llama.cpp (llama.h C API).
 * --------------------------------------------------------------------------- */

#include <stddef.h>

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
void   inference_submit_prompt(inference_ctx_t *ctx, const char *prompt);
size_t inference_read_output(inference_ctx_t *ctx, char *buf, size_t size);
int    inference_is_generating(inference_ctx_t *ctx);

/* Request the worker to stop emitting tokens for the current prompt.
 * Has no effect if no generation is in flight. */
void   inference_cancel_generation(inference_ctx_t *ctx);

/* Worker thread entry point ----------------------------------------------- */
void* inference_worker_thread(void *arg);

#endif /* WASTELAND_INFERENCE_H */
