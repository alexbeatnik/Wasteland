/* ============================================================================
 * inference.c — llama.cpp Wrapper & Worker Thread
 * ============================================================================
 *
 * Architecture:
 *   - One dedicated pthread executes inference_worker_thread().
 *   - The worker waits on a condition variable for a new prompt.
 *   - It tokenises the prompt, calls llama_decode(), then generates tokens
 *     one-by-one, appending each to a mutex-protected ring buffer.
 *   - The main thread polls inference_read_output() at 60 Hz and appends the
 *     new text to the chat history, creating a smooth typing effect.
 * ============================================================================ */

#include "inference.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "llama.h"

#define OUTPUT_BUFFER_SIZE 65536
#define MAX_PROMPT_LEN     4096
#define MAX_NEW_TOKENS     2048

struct inference_ctx {
    /* Output buffer (producer: worker, consumer: main) */
    pthread_mutex_t output_mutex;
    char   output_buffer[OUTPUT_BUFFER_SIZE];
    size_t output_write_pos;
    size_t output_read_pos;
    int    generating;

    /* Prompt queue (producer: main, consumer: worker) */
    pthread_mutex_t prompt_mutex;
    pthread_cond_t  prompt_cond;
    char   prompt[MAX_PROMPT_LEN];
    int    has_prompt;

    /* Model / context (loaded from main, consumed by worker) */
    pthread_mutex_t model_mutex;
    struct llama_model  *model;
    struct llama_context *ctx;

    /* Async load state */
    pthread_t    load_thread;
    char         load_path[1024];
    volatile int loading;       /* 1 while a load is in flight */
    volatile int load_result;   /* 0 = none, 1 = ok, -1 = fail */
    volatile int load_thread_started; /* 1 if load_thread is joinable */

    volatile int running;
};

/* ---------------------------------------------------------------------------
 * Lifecycle
 * --------------------------------------------------------------------------- */
inference_ctx_t* inference_init(void)
{
    inference_ctx_t *ictx = calloc(1, sizeof(inference_ctx_t));
    if (!ictx) return NULL;

    pthread_mutex_init(&ictx->output_mutex, NULL);
    pthread_mutex_init(&ictx->prompt_mutex, NULL);
    pthread_cond_init(&ictx->prompt_cond, NULL);
    pthread_mutex_init(&ictx->model_mutex, NULL);

    ictx->running = 1;
    return ictx;
}

void inference_shutdown(inference_ctx_t *ictx)
{
    if (!ictx) return;

    ictx->running = 0;

    /* Wake worker if it is blocked on cond-wait so it can exit cleanly. */
    pthread_mutex_lock(&ictx->prompt_mutex);
    pthread_cond_broadcast(&ictx->prompt_cond);
    pthread_mutex_unlock(&ictx->prompt_mutex);

    /* If a model load is still in flight, wait for it before tearing
     * down — llama.cpp internals are not async-cancel-safe. */
    if (ictx->load_thread_started) {
        pthread_join(ictx->load_thread, NULL);
        ictx->load_thread_started = 0;
    }

    pthread_mutex_destroy(&ictx->output_mutex);
    pthread_mutex_destroy(&ictx->prompt_mutex);
    pthread_cond_destroy(&ictx->prompt_cond);
    pthread_mutex_destroy(&ictx->model_mutex);

    free(ictx);
}

/* ---------------------------------------------------------------------------
 * Model management
 * --------------------------------------------------------------------------- */
int inference_load_model(inference_ctx_t *ictx, const char *path)
{
    if (!ictx || !path) return -1;

    pthread_mutex_lock(&ictx->model_mutex);

    /* Evict existing model/context first. */
    if (ictx->ctx) {
        llama_free(ictx->ctx);
        ictx->ctx = NULL;
    }
    if (ictx->model) {
        llama_model_free(ictx->model);
        ictx->model = NULL;
    }

    struct llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0; /* CPU-only default for portability */
    mparams.use_mmap   = false; /* avoid mmap conflicting with NVIDIA GL driver */
    mparams.use_mlock  = false;

    ictx->model = llama_model_load_from_file(path, mparams);
    if (!ictx->model) {
        pthread_mutex_unlock(&ictx->model_mutex);
        fprintf(stderr, "[inference] Failed to load model: %s\n", path);
        return -1;
    }

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx    = 4096;
    cparams.n_batch  = 512;
    cparams.n_ubatch = 512;

    ictx->ctx = llama_init_from_model(ictx->model, cparams);
    if (!ictx->ctx) {
        llama_model_free(ictx->model);
        ictx->model = NULL;
        pthread_mutex_unlock(&ictx->model_mutex);
        fprintf(stderr, "[inference] Failed to init context: %s\n", path);
        return -1;
    }

    pthread_mutex_unlock(&ictx->model_mutex);
    fprintf(stderr, "[inference] Model + ctx loaded: %s\n", path);
    return 0;
}

void inference_unload_model(inference_ctx_t *ictx)
{
    if (!ictx) return;
    pthread_mutex_lock(&ictx->model_mutex);
    if (ictx->ctx) {
        llama_free(ictx->ctx);
        ictx->ctx = NULL;
    }
    if (ictx->model) {
        llama_model_free(ictx->model);
        ictx->model = NULL;
    }
    pthread_mutex_unlock(&ictx->model_mutex);
}

int inference_is_model_loaded(inference_ctx_t *ictx)
{
    if (!ictx) return 0;
    pthread_mutex_lock(&ictx->model_mutex);
    int loaded = (ictx->model != NULL && ictx->ctx != NULL);
    pthread_mutex_unlock(&ictx->model_mutex);
    return loaded;
}

/* ---------------------------------------------------------------------------
 * Async model loading — keeps the UI thread responsive during multi-second
 * GGUF mmap/decode work.
 * --------------------------------------------------------------------------- */
static void* inference_load_thread_fn(void *arg)
{
    inference_ctx_t *ictx = (inference_ctx_t *)arg;
    int rc = inference_load_model(ictx, ictx->load_path);
    ictx->load_result = (rc == 0) ? 1 : -1;
    ictx->loading = 0;
    return NULL;
}

int inference_load_model_async(inference_ctx_t *ictx, const char *path)
{
    if (!ictx || !path) return -1;
    if (ictx->loading) return -1;

    /* Reap a previously finished thread so we can reuse load_thread. */
    if (ictx->load_thread_started) {
        pthread_join(ictx->load_thread, NULL);
        ictx->load_thread_started = 0;
    }

    strncpy(ictx->load_path, path, sizeof(ictx->load_path) - 1);
    ictx->load_path[sizeof(ictx->load_path) - 1] = '\0';
    ictx->load_result = 0;
    ictx->loading = 1;

    if (pthread_create(&ictx->load_thread, NULL,
                       inference_load_thread_fn, ictx) != 0) {
        ictx->loading = 0;
        return -1;
    }
    ictx->load_thread_started = 1;
    return 0;
}

int inference_is_loading(inference_ctx_t *ictx)
{
    if (!ictx) return 0;
    return ictx->loading;
}

int inference_take_load_result(inference_ctx_t *ictx)
{
    if (!ictx) return 0;
    int r = ictx->load_result;
    ictx->load_result = 0;
    if (r != 0 && ictx->load_thread_started) {
        pthread_join(ictx->load_thread, NULL);
        ictx->load_thread_started = 0;
    }
    return r;
}

/* ---------------------------------------------------------------------------
 * Prompt / output
 * --------------------------------------------------------------------------- */
void inference_submit_prompt(inference_ctx_t *ictx, const char *prompt)
{
    if (!ictx || !prompt) return;

    pthread_mutex_lock(&ictx->prompt_mutex);
    strncpy(ictx->prompt, prompt, MAX_PROMPT_LEN - 1);
    ictx->prompt[MAX_PROMPT_LEN - 1] = '\0';
    ictx->has_prompt = 1;
    pthread_cond_signal(&ictx->prompt_cond);
    pthread_mutex_unlock(&ictx->prompt_mutex);
}

size_t inference_read_output(inference_ctx_t *ictx, char *buf, size_t size)
{
    if (!ictx || !buf || size == 0) return 0;

    pthread_mutex_lock(&ictx->output_mutex);
    size_t available = ictx->output_write_pos - ictx->output_read_pos;
    if (available > size - 1)
        available = size - 1;

    if (available > 0) {
        memcpy(buf, ictx->output_buffer + ictx->output_read_pos, available);
        buf[available] = '\0';
        ictx->output_read_pos += available;
    } else {
        buf[0] = '\0';
    }
    pthread_mutex_unlock(&ictx->output_mutex);

    return available;
}

int inference_is_generating(inference_ctx_t *ictx)
{
    if (!ictx) return 0;
    pthread_mutex_lock(&ictx->output_mutex);
    int gen = ictx->generating;
    pthread_mutex_unlock(&ictx->output_mutex);
    return gen;
}

/* ---------------------------------------------------------------------------
 * Worker thread
 * --------------------------------------------------------------------------- */
void* inference_worker_thread(void *arg)
{
    inference_ctx_t *ictx = (inference_ctx_t *)arg;

    while (ictx->running) {
        /* ---- Wait for a prompt ---- */
        pthread_mutex_lock(&ictx->prompt_mutex);
        while (!ictx->has_prompt && ictx->running) {
            pthread_cond_wait(&ictx->prompt_cond, &ictx->prompt_mutex);
        }
        if (!ictx->running) {
            pthread_mutex_unlock(&ictx->prompt_mutex);
            break;
        }
        char prompt[MAX_PROMPT_LEN];
        strncpy(prompt, ictx->prompt, MAX_PROMPT_LEN - 1);
        prompt[MAX_PROMPT_LEN - 1] = '\0';
        ictx->has_prompt = 0;
        pthread_mutex_unlock(&ictx->prompt_mutex);

        /* ---- Acquire model snapshot ---- */
        pthread_mutex_lock(&ictx->model_mutex);
        struct llama_model  *model = ictx->model;
        struct llama_context *ctx  = ictx->ctx;
        pthread_mutex_unlock(&ictx->model_mutex);

        if (!model || !ctx) continue;

        /* ---- Reset output buffer ---- */
        pthread_mutex_lock(&ictx->output_mutex);
        ictx->output_write_pos = 0;
        ictx->output_read_pos  = 0;
        ictx->output_buffer[0] = '\0';
        ictx->generating = 1;
        pthread_mutex_unlock(&ictx->output_mutex);

        /* ---- Tokenize raw prompt (chat template disabled for stability) ---- */
        const struct llama_vocab *vocab = llama_model_get_vocab(model);
        llama_token prompt_tokens[4096];
        int n_tokens = llama_tokenize(vocab, prompt, (int)strlen(prompt),
                                      prompt_tokens, 4096,
                                      true, true);
        if (n_tokens < 0) {
            fprintf(stderr, "[inference] Tokenization failed\n");
            pthread_mutex_lock(&ictx->output_mutex);
            ictx->generating = 0;
            pthread_mutex_unlock(&ictx->output_mutex);
            continue;
        }
        if (n_tokens == 0) {
            fprintf(stderr, "[inference] Empty prompt, skipping\n");
            pthread_mutex_lock(&ictx->output_mutex);
            ictx->generating = 0;
            pthread_mutex_unlock(&ictx->output_mutex);
            continue;
        }

        /* ---- Greedy sampler (deterministic, minimal overhead) ---- */
        struct llama_sampler *smpl = llama_sampler_chain_init(
                                         llama_sampler_chain_default_params());
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

        /* ---- Decode entire prompt as a single batch ---- */
        {
            llama_batch batch = llama_batch_get_one(prompt_tokens, n_tokens);
            if (llama_decode(ctx, batch) != 0) {
                fprintf(stderr, "[inference] Prompt decode failed\n");
                llama_sampler_free(smpl);
                pthread_mutex_lock(&ictx->output_mutex);
                ictx->generating = 0;
                pthread_mutex_unlock(&ictx->output_mutex);
                continue;
            }
        }

        /* ---- Generate response ---- */
        for (int i = 0; i < MAX_NEW_TOKENS && ictx->running; i++) {
            llama_token new_token = llama_sampler_sample(smpl, ctx, -1);

            /* Stop on end-of-generation tokens */
            if (llama_vocab_is_eog(vocab, new_token))
                break;

            /* Convert token to UTF-8 piece */
            char piece[128];
            int n = llama_token_to_piece(vocab, new_token,
                                         piece, sizeof(piece) - 1,
                                         0, true);
            if (n > 0) {
                piece[n] = '\0';

                pthread_mutex_lock(&ictx->output_mutex);
                size_t space = OUTPUT_BUFFER_SIZE - ictx->output_write_pos - 1;
                if ((size_t)n < space) {
                    memcpy(ictx->output_buffer + ictx->output_write_pos,
                           piece, n);
                    ictx->output_write_pos += n;
                    ictx->output_buffer[ictx->output_write_pos] = '\0';
                }
                pthread_mutex_unlock(&ictx->output_mutex);
            }

            /* Feed token back for autoregressive sampling */
            llama_batch next = llama_batch_get_one(&new_token, 1);
            if (llama_decode(ctx, next) != 0)
                break;

            /* Brief yield — creates a natural typing cadence */
            usleep(5000); /* 5 ms */
        }

        llama_sampler_free(smpl);

        pthread_mutex_lock(&ictx->output_mutex);
        ictx->generating = 0;
        pthread_mutex_unlock(&ictx->output_mutex);
    }

    return NULL;
}
