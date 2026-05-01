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

/* Prompt / output --------------------------------------------------------- */
void   inference_submit_prompt(inference_ctx_t *ctx, const char *prompt);
size_t inference_read_output(inference_ctx_t *ctx, char *buf, size_t size);
int    inference_is_generating(inference_ctx_t *ctx);

/* Worker thread entry point ----------------------------------------------- */
void* inference_worker_thread(void *arg);

#endif /* WASTELAND_INFERENCE_H */
