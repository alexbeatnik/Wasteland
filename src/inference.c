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
#include "agent.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>

/* usleep lives in <unistd.h> on POSIX; Windows MSVC has no <unistd.h>, so we
 * fall back to Sleep(ms) from <windows.h>. The 5 ms inter-token cadence
 * doesn't need microsecond precision. */
#ifdef _WIN32
#  include <windows.h>
#  define wst_usleep(us) Sleep((DWORD)((us) / 1000))
#else
#  include <unistd.h>
#  define wst_usleep(us) usleep((useconds_t)(us))
#endif

/* Agent loop knobs. Bounded so a runaway model can't lock the worker. */
#define AGENT_MAX_TURNS         10
#define AGENT_HISTORY_SLOTS     8   /* max history messages (4 turns) carried into agent mode */
#define AGENT_MAX_MSGS          (1 + AGENT_HISTORY_SLOTS + 1 + AGENT_MAX_TURNS * 2)  /* sys + hist + user + tool loop */
#define AGENT_ACCUM_BUFFER      8192
#define AGENT_TOOL_RESULT_BUF   AGENT_MAX_TOOL_RESULT

#include "llama.h"

#define OUTPUT_BUFFER_SIZE 65536
#define MAX_PROMPT_LEN     4096
#define MAX_NEW_TOKENS     2048

/* Longest literal we strip ("</think>" = 8 bytes). The carry buffer must
 * hold up to (longest - 1) trailing bytes between pieces so a tag split
 * across two tokens still matches. */
#define TAG_CARRY_MAX 7

struct inference_ctx {
    /* Output buffer (producer: worker, consumer: main) */
    pthread_mutex_t output_mutex;
    char   output_buffer[OUTPUT_BUFFER_SIZE];
    size_t output_write_pos;
    size_t output_read_pos;
    int    generating;
    volatile int cancel_generation; /* set by UI, polled by worker */

    /* Per-prompt state for stripping <think>/</think> markers from the
     * stream while keeping the reasoning text itself. */
    char   tag_carry[TAG_CARRY_MAX];
    size_t tag_carry_len;
    /* Last char emitted this turn; '\n' at turn start so an initial <think>
     * is accepted. Updated by output_append_locked to detect line-start. */
    char   tag_prev_char;

    /* Prompt queue (producer: main, consumer: worker) */
    pthread_mutex_t prompt_mutex;
    pthread_cond_t  prompt_cond;
    char   prompt[MAX_PROMPT_LEN];
    char   sys_prompt[MAX_PROMPT_LEN];
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

    /* Agent mode (set by main between prompts; consumed by worker on each
     * dequeue so toggling mid-prompt has no half-state effect) */
    pthread_mutex_t agent_mutex;
    volatile int    agent_mode;
    char            agent_workspace[1024];

    /* Pending mutating-tool action awaiting user APPLY/REJECT in the UI.
     * pending_kind = 0 (none) / 1 (write_file) / 2 (apply_edit). The worker
     * polls `pending_approval` while > 0; UI flips it via inference_set_pending_approval. */
    pthread_mutex_t pending_mutex;
    volatile int    pending_kind;
    char            pending_path[AGENT_MAX_PATH_LEN];
    char           *pending_content;   /* heap, write_file body */
    char           *pending_search;    /* heap, apply_edit search */
    char           *pending_replace;   /* heap, apply_edit replace */
    volatile int    pending_approval;  /* 0=waiting, +1=apply, -1=reject */

    /* Full chat history (mirrored from app_state so the worker can build
     * a multi-turn message list for the chat template). */
    pthread_mutex_t history_mutex;
    char chat_history[WASTELAND_MAX_CHAT_HISTORY];

    /* Tunables. n_ctx is consumed at model-load time; temperature is read at
     * sampler-build time on every prompt. Both protected by settings_mutex. */
    pthread_mutex_t settings_mutex;
    int   pending_n_ctx;
    float temperature;

    volatile int running;
};

#define WST_DEFAULT_N_CTX        4096
#define WST_MIN_N_CTX             512
#define WST_MAX_N_CTX          262144   /* 256k — large enough for 35B+ models */
#define WST_DEFAULT_TEMPERATURE  0.8f

#define MAX_HISTORY_MSGS 1024

/* ---------------------------------------------------------------------------
 * Chat-history parser
 *
 * Turns the flat UI history string into user/assistant pairs.
 * Lines starting with "> " are user messages; everything up to the next
 * "> " line (or EOF) is the assistant reply.
 * An unterminated user message at the very end is ignored — the current
 * prompt is supplied separately via ictx->prompt.
 * --------------------------------------------------------------------------- */
static int parse_chat_history(const char *history,
                              struct llama_chat_message *msgs,
                              char **owned,
                              int max_msgs)
{
    int n = 0;
    const char *p = history;
    const char *end = history + strlen(history);

    while (p < end && n < max_msgs) {
        while (p < end && (*p == '\n' || *p == '\r')) p++;
        if (p >= end) break;

        if (p + 2 > end || p[0] != '>' || p[1] != ' ') {
            const char *nl = memchr(p, '\n', (size_t)(end - p));
            p = nl ? nl + 1 : end;
            continue;
        }

        const char *user_start = p + 2;
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        size_t ulen = (size_t)(line_end - user_start);

        char *uc = (char *)malloc(ulen + 1);
        if (!uc) break;
        memcpy(uc, user_start, ulen);
        uc[ulen] = '\0';

        msgs[n].role = "user";
        msgs[n].content = uc;
        owned[n] = uc;
        n++;

        p = line_end + 1;
        if (p >= end) {
            /* User line at the very end of history with no assistant reply
             * — this is the current prompt about to be submitted. Drop it
             * so the worker doesn't double-add the same user turn. */
            n--;
            free(owned[n]);
            owned[n] = NULL;
            break;
        }

        const char *assist_start = p;
        const char *next_user = NULL;
        while (p < end) {
            const char *pnl = memchr(p, '\n', (size_t)(end - p));
            if (!pnl) pnl = end;
            if (pnl + 2 < end && pnl[1] == '>' && pnl[2] == ' ') {
                next_user = pnl;
                break;
            }
            p = (pnl < end) ? pnl + 1 : end;
        }

        if (next_user) {
            size_t alen = (size_t)(next_user - assist_start);
            while (alen > 0 && (assist_start[alen - 1] == '\n' ||
                                assist_start[alen - 1] == '\r')) alen--;
            if (alen > 0 && n < max_msgs) {
                char *ac = (char *)malloc(alen + 1);
                if (ac) {
                    memcpy(ac, assist_start, alen);
                    ac[alen] = '\0';
                    msgs[n].role = "assistant";
                    msgs[n].content = ac;
                    owned[n] = ac;
                    n++;
                }
            }
            p = next_user + 1;
        } else {
            if (assist_start < end) {
                /* Last user message HAS an assistant reply — keep both. */
                size_t alen = (size_t)(end - assist_start);
                while (alen > 0 && (assist_start[alen - 1] == '\n' ||
                                    assist_start[alen - 1] == '\r')) alen--;
                if (alen > 0 && n < max_msgs) {
                    char *ac = (char *)malloc(alen + 1);
                    if (ac) {
                        memcpy(ac, assist_start, alen);
                        ac[alen] = '\0';
                        msgs[n].role = "assistant";
                        msgs[n].content = ac;
                        owned[n] = ac;
                        n++;
                    }
                }
            } else {
                /* No assistant reply — this is the current prompt, discard. */
                n--;
                free(owned[n]);
                owned[n] = NULL;
            }
            break;
        }
    }

    return n;
}

static void free_parsed_msgs(char **owned, int n)
{
    for (int i = 0; i < n; i++) {
        free(owned[i]);
        owned[i] = NULL;
    }
}

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
    pthread_mutex_init(&ictx->agent_mutex, NULL);
    pthread_mutex_init(&ictx->pending_mutex, NULL);
    pthread_mutex_init(&ictx->history_mutex, NULL);
    pthread_mutex_init(&ictx->settings_mutex, NULL);

    ictx->pending_n_ctx = WST_DEFAULT_N_CTX;
    ictx->temperature   = WST_DEFAULT_TEMPERATURE;

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
    pthread_mutex_destroy(&ictx->agent_mutex);
    pthread_mutex_destroy(&ictx->pending_mutex);
    pthread_mutex_destroy(&ictx->history_mutex);
    pthread_mutex_destroy(&ictx->settings_mutex);

    free(ictx->pending_content);
    free(ictx->pending_search);
    free(ictx->pending_replace);
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

    pthread_mutex_lock(&ictx->settings_mutex);
    int wanted_ctx = ictx->pending_n_ctx;
    pthread_mutex_unlock(&ictx->settings_mutex);
    if (wanted_ctx < WST_MIN_N_CTX) wanted_ctx = WST_MIN_N_CTX;
    if (wanted_ctx > WST_MAX_N_CTX) wanted_ctx = WST_MAX_N_CTX;

    struct llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx    = (uint32_t)wanted_ctx;
    /* n_batch must be >= the largest prompt we ever feed in one llama_decode().
     * Agent mode can build prompts of several thousand tokens (system + tool
     * instructions + multi-turn history), so we match n_batch to n_ctx and
     * keep n_ubatch small to bound peak memory. */
    cparams.n_batch  = (uint32_t)wanted_ctx;
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
void inference_submit_prompt(inference_ctx_t *ictx, const char *sys_prompt, const char *prompt)
{
    if (!ictx || !prompt) return;

    pthread_mutex_lock(&ictx->prompt_mutex);
    if (sys_prompt) {
        strncpy(ictx->sys_prompt, sys_prompt, MAX_PROMPT_LEN - 1);
        ictx->sys_prompt[MAX_PROMPT_LEN - 1] = '\0';
    } else {
        ictx->sys_prompt[0] = '\0';
    }
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

void inference_cancel_generation(inference_ctx_t *ictx)
{
    if (!ictx) return;
    ictx->cancel_generation = 1;
}

void inference_request_stop(inference_ctx_t *ictx)
{
    if (!ictx) return;
    ictx->running = 0;
    ictx->cancel_generation = 1;
    pthread_mutex_lock(&ictx->prompt_mutex);
    pthread_cond_broadcast(&ictx->prompt_cond);
    pthread_mutex_unlock(&ictx->prompt_mutex);
}

/* ---------------------------------------------------------------------------
 * Chat history mirror
 * --------------------------------------------------------------------------- */
void inference_set_chat_history(inference_ctx_t *ictx, const char *history)
{
    if (!ictx || !history) return;
    pthread_mutex_lock(&ictx->history_mutex);
    strncpy(ictx->chat_history, history, sizeof(ictx->chat_history) - 1);
    ictx->chat_history[sizeof(ictx->chat_history) - 1] = '\0';
    pthread_mutex_unlock(&ictx->history_mutex);
}

/* ---------------------------------------------------------------------------
 * Context usage stats
 * --------------------------------------------------------------------------- */
int inference_get_context_stats(inference_ctx_t *ictx,
                                const char *history,
                                int *tokens_out,
                                int *max_out)
{
    if (!ictx || !tokens_out || !max_out) return -1;
    *tokens_out = 0;
    *max_out = 0;

    pthread_mutex_lock(&ictx->model_mutex);
    struct llama_model  *model = ictx->model;
    struct llama_context *ctx  = ictx->ctx;
    pthread_mutex_unlock(&ictx->model_mutex);

    if (!model || !ctx) return -1;

    struct llama_chat_message msgs[MAX_HISTORY_MSGS];
    char *owned[MAX_HISTORY_MSGS] = {0};
    int n_msgs = parse_chat_history(history, msgs, owned, MAX_HISTORY_MSGS);

    const char *tmpl = llama_model_chat_template(model, NULL);
    if (!tmpl) {
        free_parsed_msgs(owned, n_msgs);
        return -1;
    }

    static char fmtd[65536];
    int32_t fl = llama_chat_apply_template(tmpl, msgs, n_msgs, true,
                                           fmtd, (int32_t)sizeof(fmtd));
    if (fl <= 0 || fl >= (int32_t)sizeof(fmtd)) {
        free_parsed_msgs(owned, n_msgs);
        return -1;
    }
    fmtd[fl] = '\0';

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    /* Probe-tokenise: with tokens=NULL/n_tokens_max=0 llama_tokenize returns
     * the negated count it would have written. INT32_MIN signals overflow. */
    int n_tokens = llama_tokenize(vocab, fmtd, fl, NULL, 0, true, true);
    free_parsed_msgs(owned, n_msgs);

    if (n_tokens == INT32_MIN) return -1;
    if (n_tokens < 0) n_tokens = -n_tokens;
    *tokens_out = n_tokens;
    *max_out    = llama_n_ctx(ctx);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Tunables
 * --------------------------------------------------------------------------- */
void inference_set_n_ctx(inference_ctx_t *ictx, int n_ctx)
{
    if (!ictx) return;
    if (n_ctx < WST_MIN_N_CTX) n_ctx = WST_MIN_N_CTX;
    if (n_ctx > WST_MAX_N_CTX) n_ctx = WST_MAX_N_CTX;
    pthread_mutex_lock(&ictx->settings_mutex);
    ictx->pending_n_ctx = n_ctx;
    pthread_mutex_unlock(&ictx->settings_mutex);
}

void inference_set_temperature(inference_ctx_t *ictx, float temperature)
{
    if (!ictx) return;
    if (temperature < 0.01f) temperature = 0.01f;
    if (temperature > 5.0f)  temperature = 5.0f;
    pthread_mutex_lock(&ictx->settings_mutex);
    ictx->temperature = temperature;
    pthread_mutex_unlock(&ictx->settings_mutex);
}

int inference_get_n_ctx(inference_ctx_t *ictx)
{
    if (!ictx) return WST_DEFAULT_N_CTX;
    pthread_mutex_lock(&ictx->settings_mutex);
    int v = ictx->pending_n_ctx;
    pthread_mutex_unlock(&ictx->settings_mutex);
    return v;
}

float inference_get_temperature(inference_ctx_t *ictx)
{
    if (!ictx) return WST_DEFAULT_TEMPERATURE;
    pthread_mutex_lock(&ictx->settings_mutex);
    float v = ictx->temperature;
    pthread_mutex_unlock(&ictx->settings_mutex);
    return v;
}

/* ---------------------------------------------------------------------------
 * Agent mode public API
 * --------------------------------------------------------------------------- */
void inference_set_agent(inference_ctx_t *ictx, int mode, const char *workspace)
{
    if (!ictx) return;
    pthread_mutex_lock(&ictx->agent_mutex);
    ictx->agent_mode = mode ? 1 : 0;
    if (workspace) {
        strncpy(ictx->agent_workspace, workspace,
                sizeof(ictx->agent_workspace) - 1);
        ictx->agent_workspace[sizeof(ictx->agent_workspace) - 1] = '\0';
    } else {
        ictx->agent_workspace[0] = '\0';
    }
    pthread_mutex_unlock(&ictx->agent_mutex);
}

int inference_get_pending(inference_ctx_t *ictx,
                          const char **path_out,
                          const char **content_out,
                          const char **search_out,
                          const char **replace_out)
{
    if (!ictx) return 0;
    pthread_mutex_lock(&ictx->pending_mutex);
    int kind = ictx->pending_kind;
    if (kind != 0) {
        if (path_out)    *path_out    = ictx->pending_path;
        if (content_out) *content_out = ictx->pending_content;
        if (search_out)  *search_out  = ictx->pending_search;
        if (replace_out) *replace_out = ictx->pending_replace;
    }
    pthread_mutex_unlock(&ictx->pending_mutex);
    return kind;
}

void inference_set_pending_approval(inference_ctx_t *ictx, int decision)
{
    if (!ictx) return;
    pthread_mutex_lock(&ictx->pending_mutex);
    if (ictx->pending_kind != 0) {
        ictx->pending_approval = (decision >= 0) ? +1 : -1;
    }
    pthread_mutex_unlock(&ictx->pending_mutex);
}

/* ---------------------------------------------------------------------------
 * <think>-tag stripper
 *
 * Qwen3-style models emit reasoning wrapped in literal "<think>...</think>"
 * markers. The user wants the reasoning text but not the markup, so we drop
 * just the tag bytes from the byte stream. A carry buffer holds the last
 * few unprocessed bytes between calls so a tag split across two token
 * pieces (e.g. "<th" then "ink>") is still detected.
 * --------------------------------------------------------------------------- */
static void output_append_locked(inference_ctx_t *ictx,
                                 const char *src, size_t n)
{
    if (n == 0) return;
    size_t space = OUTPUT_BUFFER_SIZE - ictx->output_write_pos - 1;
    if (n > space) n = space;
    if (n == 0) return;
    memcpy(ictx->output_buffer + ictx->output_write_pos, src, n);
    ictx->output_write_pos += n;
    ictx->output_buffer[ictx->output_write_pos] = '\0';
    ictx->tag_prev_char = src[n - 1];
}

static void emit_filtered_piece(inference_ctx_t *ictx,
                                const char *piece, size_t n)
{
    static const char  tag_open[]  = "<think>";
    static const char  tag_close[] = "</think>";
    static const size_t open_len   = sizeof(tag_open)  - 1; /* 7 */
    static const size_t close_len  = sizeof(tag_close) - 1; /* 8 */
    static const size_t reserve    = TAG_CARRY_MAX;         /* 7 */

    /* Stage = previous carry + new piece. Cap at the local buffer size. */
    char buf[256];
    size_t blen = 0;
    if (ictx->tag_carry_len > 0) {
        memcpy(buf, ictx->tag_carry, ictx->tag_carry_len);
        blen = ictx->tag_carry_len;
    }
    size_t take = sizeof(buf) - blen;
    if (n < take) take = n;
    memcpy(buf + blen, piece, take);
    blen += take;

    pthread_mutex_lock(&ictx->output_mutex);

    size_t pos = 0;
    while (pos < blen) {
        /* Find the earliest occurrence of either tag at or after pos. */
        const char *m_open  = NULL;
        const char *m_close = NULL;
        for (size_t i = pos; i + open_len <= blen; i++) {
            if (memcmp(buf + i, tag_open, open_len) == 0) {
                m_open = buf + i; break;
            }
        }
        for (size_t i = pos; i + close_len <= blen; i++) {
            if (memcmp(buf + i, tag_close, close_len) == 0) {
                m_close = buf + i; break;
            }
        }

        const char *m = NULL;
        size_t      mlen = 0;
        if (m_open && (!m_close || m_open <= m_close)) {
            m = m_open; mlen = open_len;
        } else if (m_close) {
            m = m_close; mlen = close_len;
        }

        if (m) {
            size_t safe = (size_t)(m - buf);
            /* Only treat as a think tag if it's at the start of a line.
             * This prevents `<think>` used as literal prose from triggering
             * a false think block (e.g. "`<think>`" in markdown). */
            char char_before = (m > buf) ? *(m - 1) : ictx->tag_prev_char;
            if (char_before != '\0' && char_before != '\n') {
                /* Not at line-start — emit content up to and including '<'
                 * as plain text, then keep scanning for real tags. */
                output_append_locked(ictx, buf + pos, safe - pos + 1);
                pos = safe + 1;
                continue;
            }

            if (safe > pos) output_append_locked(ictx, buf + pos, safe - pos);
            if (m == m_open) {
                const char *marker = "\n-- THINK --\n";
                output_append_locked(ictx, marker, strlen(marker));
            } else {
                const char *marker = "\n-- END THINK --\n";
                output_append_locked(ictx, marker, strlen(marker));
            }
            pos = safe + mlen;
        } else {
            /* No tag in [pos, blen). Anything before (blen - reserve) can't
             * be the start of a future tag, so it's safe to emit. The last
             * up-to-`reserve` bytes go into carry for next call. */
            size_t safe_end = pos;
            if (blen > reserve && blen - reserve > pos) safe_end = blen - reserve;
            if (safe_end > pos) {
                output_append_locked(ictx, buf + pos, safe_end - pos);
                pos = safe_end;
            }
            break;
        }
    }

    pthread_mutex_unlock(&ictx->output_mutex);

    /* Save unprocessed tail as the new carry. */
    size_t tail = blen - pos;
    if (tail > TAG_CARRY_MAX) tail = TAG_CARRY_MAX;
    if (tail > 0) memcpy(ictx->tag_carry, buf + pos, tail);
    ictx->tag_carry_len = tail;
}

static void emit_filtered_flush(inference_ctx_t *ictx)
{
    /* End of generation: any leftover carry can't be the start of a tag,
     * so flush it verbatim. */
    if (ictx->tag_carry_len == 0) return;
    pthread_mutex_lock(&ictx->output_mutex);
    output_append_locked(ictx, ictx->tag_carry, ictx->tag_carry_len);
    pthread_mutex_unlock(&ictx->output_mutex);
    ictx->tag_carry_len = 0;
}

/* ---------------------------------------------------------------------------
 * Single-turn inference helper
 *
 * Renders the conversation through the model's chat template, tokenises,
 * decodes, and streams generated tokens through emit_filtered_piece(). At
 * end of generation, copies the new bytes appended to output_buffer into
 * `accum` (so the caller can parse the assistant's reply for tool calls).
 *
 * Returns 0 on success, -1 on tokenisation/decode failure, -2 on cancel.
 * --------------------------------------------------------------------------- */
static int run_one_turn(inference_ctx_t *ictx,
                        struct llama_model *model,
                        struct llama_context *ctx,
                        const struct llama_chat_message *msgs, int n_msgs,
                        char *accum, size_t accum_size)
{
    /* Per-turn state reset (do NOT clobber output_buffer — it's the running
     * chat history visible to the user). */
    ictx->tag_carry_len  = 0;
    ictx->tag_prev_char  = '\n'; /* treat start of turn as "after newline" */

    pthread_mutex_lock(&ictx->output_mutex);
    size_t turn_start = ictx->output_write_pos;
    pthread_mutex_unlock(&ictx->output_mutex);

    const char *tmpl = llama_model_chat_template(model, NULL);
    if (!tmpl) {
        fprintf(stderr, "[inference] No chat template available\n");
        return -1;
    }

    static char fmtd[65536];
    int32_t fl = llama_chat_apply_template(tmpl, msgs, n_msgs, true,
                                           fmtd, (int32_t)sizeof(fmtd));
    if (fl <= 0 || fl >= (int32_t)sizeof(fmtd)) {
        fprintf(stderr, "[inference] chat_apply_template rc=%d\n", fl);
        return -1;
    }
    fmtd[fl] = '\0';

    /* Wipe KV cache: each turn re-tokenises the entire conversation, so KV
     * residue from a prior turn would just bloat memory without helping. */
    llama_memory_clear(llama_get_memory(ctx), true);

    const struct llama_vocab *vocab = llama_model_get_vocab(model);
    static llama_token prompt_tokens[8192];
    int n_tokens = llama_tokenize(vocab, fmtd, fl,
                                  prompt_tokens, 8192,
                                  true, true);
    if (n_tokens <= 0) {
        fprintf(stderr, "[inference] Tokenisation failed (n=%d, fl=%d)\n",
                n_tokens, fl);
        return -1;
    }

    /* Sampler stack: penalties stop the model from looping on small (~1B)
     * models, then standard top_k/top_p/temp/dist for varied output. Pure
     * greedy on these models reliably degenerates into paragraph repeats. */
    pthread_mutex_lock(&ictx->settings_mutex);
    float temp_now = ictx->temperature;
    pthread_mutex_unlock(&ictx->settings_mutex);
    if (temp_now < 0.01f) temp_now = 0.01f;
    if (temp_now > 5.0f)  temp_now = 5.0f;

    struct llama_sampler *smpl = llama_sampler_chain_init(
                                     llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
                                      64,    /* last_n */
                                      1.1f,  /* repeat */
                                      0.0f,  /* freq */
                                      0.0f));/* present */
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(temp_now));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    /* Chunk the prompt by n_batch so we never trip the
     * GGML_ASSERT(n_tokens_all <= cparams.n_batch) inside llama-context.
     * llama_batch_get_one() auto-tracks position, so consecutive calls
     * continue the same logical sequence. */
    {
        /* Chunk to match the configured n_batch — cparams.n_batch is set to
         * n_ctx at load time, so just read it back. */
        int chunk = (int)llama_n_batch(ctx);
        if (chunk <= 0) chunk = 4096;
        for (int off = 0; off < n_tokens; off += chunk) {
            int n = (n_tokens - off < chunk) ? (n_tokens - off) : chunk;
            if (llama_decode(ctx,
                             llama_batch_get_one(prompt_tokens + off, n)) != 0)
            {
                fprintf(stderr,
                        "[inference] Prompt decode failed at offset %d/%d\n",
                        off, n_tokens);
                llama_sampler_free(smpl);
                return -1;
            }
        }
    }

    int rc = 0;
    for (int i = 0; i < MAX_NEW_TOKENS && ictx->running; i++) {
        if (ictx->cancel_generation) { rc = -2; break; }

        llama_token new_token = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, new_token)) break;

        char piece[128];
        int n = llama_token_to_piece(vocab, new_token,
                                     piece, sizeof(piece) - 1,
                                     0, true);
        if (n > 0) emit_filtered_piece(ictx, piece, (size_t)n);

        if (llama_decode(ctx, llama_batch_get_one(&new_token, 1)) != 0) break;
        wst_usleep(5000); /* 5 ms typing cadence */
    }

    llama_sampler_free(smpl);
    emit_filtered_flush(ictx);

    /* Trailing newline so next chat line / tool result starts cleanly. */
    pthread_mutex_lock(&ictx->output_mutex);
    if (ictx->output_write_pos == 0 ||
        ictx->output_buffer[ictx->output_write_pos - 1] != '\n') {
        output_append_locked(ictx, "\n", 1);
    }

    /* Snapshot what we just emitted into the caller's accum buffer. */
    if (accum && accum_size > 0) {
        size_t turn_len = ictx->output_write_pos - turn_start;
        if (turn_len >= accum_size) turn_len = accum_size - 1;
        memcpy(accum, ictx->output_buffer + turn_start, turn_len);
        accum[turn_len] = '\0';
    }
    pthread_mutex_unlock(&ictx->output_mutex);

    return rc;
}

/* ---------------------------------------------------------------------------
 * Agent helpers
 * --------------------------------------------------------------------------- */
static const char* tool_name_of(agent_tool_t k)
{
    switch (k) {
        case AGENT_TOOL_READ_FILE:  return "read_file";
        case AGENT_TOOL_LIST_DIR:   return "list_dir";
        case AGENT_TOOL_WRITE_FILE: return "write_file";
        case AGENT_TOOL_APPLY_EDIT: return "apply_edit";
        default:                    return "unknown";
    }
}

/* Append `s` (length n) to the output buffer under lock. Bypasses the
 * <think> filter — used for agent UI markers (TOOL: ..., RESULT: ..., etc.) */
static void emit_raw(inference_ctx_t *ictx, const char *s, size_t n)
{
    if (!s || n == 0) return;
    pthread_mutex_lock(&ictx->output_mutex);
    output_append_locked(ictx, s, n);
    pthread_mutex_unlock(&ictx->output_mutex);
}

static void emit_raw_str(inference_ctx_t *ictx, const char *s)
{
    if (!s) return;
    emit_raw(ictx, s, strlen(s));
}

/* Execute one tool call. For reads/list, runs immediately and returns the
 * result. For writes/edits, queues a pending action and polls until the UI
 * thread sets pending_approval (or the user cancels generation). The tool's
 * textual outcome is written to `result_out` (this is what the model sees on
 * the next turn) AND a human-readable summary is appended to output_buffer. */
static void process_tool_call(inference_ctx_t *ictx,
                              const char *workspace,
                              const agent_call_t *call,
                              char *result_out, size_t result_size)
{
    char hdr[1280];
    int hn = snprintf(hdr, sizeof(hdr), "\n[ TOOL: %s | %s ]\n",
                      tool_name_of(call->kind), call->path);
    emit_raw(ictx, hdr, (size_t)hn);

    if (!workspace || !*workspace) {
        const char *err = "  ERROR: agent workspace not configured\n";
        emit_raw_str(ictx, err);
        snprintf(result_out, result_size, "ERROR: agent workspace not set");
        return;
    }

    if (call->kind == AGENT_TOOL_READ_FILE) {
        agent_exec_read_file(workspace, call->path, result_out, result_size);
        return;
    }
    if (call->kind == AGENT_TOOL_LIST_DIR) {
        agent_exec_list_dir(workspace, call->path, result_out, result_size);
        return;
    }

    /* Mutating tool — queue for approval and wait. */
    pthread_mutex_lock(&ictx->pending_mutex);
    ictx->pending_kind = (call->kind == AGENT_TOOL_WRITE_FILE) ? 1 : 2;
    snprintf(ictx->pending_path, sizeof(ictx->pending_path), "%s", call->path);
    free(ictx->pending_content);
    free(ictx->pending_search);
    free(ictx->pending_replace);
    ictx->pending_content = call->content ? strdup(call->content) : NULL;
    ictx->pending_search  = call->search  ? strdup(call->search)  : NULL;
    ictx->pending_replace = call->replace ? strdup(call->replace) : NULL;
    ictx->pending_approval = 0;
    pthread_mutex_unlock(&ictx->pending_mutex);

    emit_raw_str(ictx, "  AWAITING APPROVAL — review in right panel and click [APPLY] or [REJECT]\n");

    int decision = 0;
    while (ictx->running && decision == 0) {
        wst_usleep(50000); /* 50 ms */
        if (ictx->cancel_generation) { decision = -1; break; }
        pthread_mutex_lock(&ictx->pending_mutex);
        decision = ictx->pending_approval;
        pthread_mutex_unlock(&ictx->pending_mutex);
    }

    /* Detach the pending action so the UI stops rendering it. We keep
     * local copies of the strings to actually execute below. */
    pthread_mutex_lock(&ictx->pending_mutex);
    ictx->pending_kind = 0;
    ictx->pending_approval = 0;
    char *content_local = ictx->pending_content; ictx->pending_content = NULL;
    char *search_local  = ictx->pending_search;  ictx->pending_search  = NULL;
    char *replace_local = ictx->pending_replace; ictx->pending_replace = NULL;
    pthread_mutex_unlock(&ictx->pending_mutex);

    if (decision > 0) {
        char err[512] = "";
        int rc;
        if (call->kind == AGENT_TOOL_WRITE_FILE) {
            rc = agent_exec_write_file(workspace, call->path, content_local,
                                       err, sizeof(err));
        } else {
            rc = agent_exec_apply_edit(workspace, call->path,
                                       search_local, replace_local,
                                       err, sizeof(err));
        }
        if (rc == 0) {
            emit_raw_str(ictx, "  APPLIED\n");
            snprintf(result_out, result_size, "Successfully applied %s to %s",
                     tool_name_of(call->kind), call->path);
        } else {
            char emsg[640];
            int en = snprintf(emsg, sizeof(emsg), "  FAILED: %s\n", err);
            emit_raw(ictx, emsg, (size_t)en);
            snprintf(result_out, result_size, "ERROR applying %s: %s",
                     tool_name_of(call->kind), err);
        }
    } else {
        emit_raw_str(ictx, "  REJECTED by user\n");
        snprintf(result_out, result_size, "User REJECTED the proposed %s on %s",
                 tool_name_of(call->kind), call->path);
    }

    free(content_local);
    free(search_local);
    free(replace_local);
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
        char sys_prompt[MAX_PROMPT_LEN];
        strncpy(prompt, ictx->prompt, MAX_PROMPT_LEN - 1);
        prompt[MAX_PROMPT_LEN - 1] = '\0';
        strncpy(sys_prompt, ictx->sys_prompt, MAX_PROMPT_LEN - 1);
        sys_prompt[MAX_PROMPT_LEN - 1] = '\0';
        ictx->has_prompt = 0;
        pthread_mutex_unlock(&ictx->prompt_mutex);

        /* ---- Snapshot model + agent settings ---- */
        pthread_mutex_lock(&ictx->model_mutex);
        struct llama_model  *model = ictx->model;
        struct llama_context *ctx  = ictx->ctx;
        pthread_mutex_unlock(&ictx->model_mutex);
        if (!model || !ctx) continue;

        char workspace[1024];
        int  agent_mode_now;
        pthread_mutex_lock(&ictx->agent_mutex);
        agent_mode_now = ictx->agent_mode;
        snprintf(workspace, sizeof(workspace), "%s", ictx->agent_workspace);
        pthread_mutex_unlock(&ictx->agent_mutex);
        if (agent_mode_now && workspace[0] == '\0') {
            /* Agent toggled ON but no workspace set — treat as plain chat */
            agent_mode_now = 0;
        }

        /* ---- Reset for the new user prompt ---- */
        ictx->cancel_generation = 0;
        ictx->tag_carry_len  = 0;
        ictx->tag_prev_char  = '\n';
        pthread_mutex_lock(&ictx->output_mutex);
        ictx->output_write_pos = 0;
        ictx->output_read_pos  = 0;
        ictx->output_buffer[0] = '\0';
        ictx->generating = 1;
        pthread_mutex_unlock(&ictx->output_mutex);

        if (!agent_mode_now) {
            /* ---- Multi-turn chat with full history ---- */
            char local_history[WASTELAND_MAX_CHAT_HISTORY];
            pthread_mutex_lock(&ictx->history_mutex);
            strncpy(local_history, ictx->chat_history, sizeof(local_history) - 1);
            local_history[sizeof(local_history) - 1] = '\0';
            pthread_mutex_unlock(&ictx->history_mutex);

            struct llama_chat_message msgs[MAX_HISTORY_MSGS];
            char *owned[MAX_HISTORY_MSGS] = {0};
            int n_msgs = 0;

            if (sys_prompt[0]) {
                msgs[n_msgs].role = "system";
                msgs[n_msgs].content = sys_prompt;
                n_msgs++;
            }

            int sys_offset = n_msgs;
            int n_hist = parse_chat_history(local_history,
                                            msgs + n_msgs,
                                            owned + n_msgs,
                                            MAX_HISTORY_MSGS - n_msgs - 1);
            n_msgs += n_hist;

            msgs[n_msgs].role = "user";
            msgs[n_msgs].content = prompt;
            n_msgs++;

            fprintf(stderr, "[worker] n_msgs=%d n_hist=%d hist_len=%zu\n",
                    n_msgs, n_hist, strlen(local_history));
            for (int i = 0; i < n_msgs; i++) {
                fprintf(stderr, "  msg[%d] %s: %.40s...\n",
                        i, msgs[i].role,
                        msgs[i].content ? msgs[i].content : "(null)");
            }
            run_one_turn(ictx, model, ctx, msgs, n_msgs, NULL, 0);
            free_parsed_msgs(owned + sys_offset, n_hist);
        } else {
            /* ---- Agent ReAct loop ---- */
            struct llama_chat_message msgs[AGENT_MAX_MSGS];
            char *owned[AGENT_MAX_MSGS] = {0};
            int n_msgs = 0;

            /* Combine user system prompt with the agent tool instructions. */
            char combined_sys[8192];
            if (sys_prompt[0]) {
                snprintf(combined_sys, sizeof(combined_sys), "%s\n\n%s",
                         sys_prompt, agent_system_prompt());
            } else {
                snprintf(combined_sys, sizeof(combined_sys), "%s",
                         agent_system_prompt());
            }
            msgs[n_msgs].role    = "system";
            msgs[n_msgs].content = combined_sys;
            n_msgs++;

            /* Include conversation history so the model remembers previous
             * exchanges (same as non-agent mode), capped at AGENT_HISTORY_SLOTS
             * to leave room for tool-call turns inside the ReAct loop. */
            char local_history[WASTELAND_MAX_CHAT_HISTORY];
            pthread_mutex_lock(&ictx->history_mutex);
            strncpy(local_history, ictx->chat_history, sizeof(local_history) - 1);
            local_history[sizeof(local_history) - 1] = '\0';
            pthread_mutex_unlock(&ictx->history_mutex);

            int n_hist = parse_chat_history(local_history,
                                            msgs + n_msgs, owned + n_msgs,
                                            AGENT_HISTORY_SLOTS);
            n_msgs += n_hist;

            msgs[n_msgs].role    = "user";
            msgs[n_msgs].content = prompt;
            n_msgs++;

            for (int turn = 0;
                 turn < AGENT_MAX_TURNS && ictx->running &&
                 !ictx->cancel_generation && n_msgs + 2 <= AGENT_MAX_MSGS;
                 turn++)
            {
                static char accum[AGENT_ACCUM_BUFFER];
                if (run_one_turn(ictx, model, ctx, msgs, n_msgs,
                                 accum, sizeof(accum)) != 0) break;

                agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
                int n_calls = agent_parse_calls(accum, calls,
                                                AGENT_MAX_CALLS_PER_TURN);
                if (n_calls == 0) break; /* model finished — no more tools */

                /* Record assistant turn for next round. */
                char *acopy = strdup(accum);
                if (!acopy) { agent_free_calls(calls, n_calls); break; }
                msgs[n_msgs].role    = "assistant";
                msgs[n_msgs].content = acopy;
                owned[n_msgs] = acopy;
                n_msgs++;

                /* Execute every parsed tool call, accumulating results
                 * for the next user turn. */
                static char tool_results[AGENT_TOOL_RESULT_BUF];
                tool_results[0] = '\0';
                size_t tr_len = 0;
                for (int c = 0; c < n_calls && ictx->running &&
                                !ictx->cancel_generation; c++)
                {
                    static char one_result[AGENT_TOOL_RESULT_BUF];
                    process_tool_call(ictx, workspace, &calls[c],
                                      one_result, sizeof(one_result));
                    int wrote = snprintf(tool_results + tr_len,
                                         sizeof(tool_results) - tr_len,
                                         "<tool_result tool=\"%s\" path=\"%s\">\n%s\n</tool_result>\n\n",
                                         tool_name_of(calls[c].kind),
                                         calls[c].path, one_result);
                    if (wrote > 0) tr_len += (size_t)wrote;
                    if (tr_len >= sizeof(tool_results) - 1) break;
                }
                agent_free_calls(calls, n_calls);

                if (n_msgs >= AGENT_MAX_MSGS) break;
                char *rcopy = strdup(tool_results);
                if (!rcopy) break;
                msgs[n_msgs].role    = "user";
                msgs[n_msgs].content = rcopy;
                owned[n_msgs] = rcopy;
                n_msgs++;
            }

            for (int i = 0; i < n_msgs; i++) free(owned[i]);
        }

        pthread_mutex_lock(&ictx->output_mutex);
        ictx->generating = 0;
        pthread_mutex_unlock(&ictx->output_mutex);
    }

    return NULL;
}
