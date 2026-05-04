/* ============================================================================
 * network.c — libcurl Downloader & seccomp Lockdown
 * ============================================================================
 *
 * Cross-platform notes:
 *   - seccomp is Linux-only; on macOS/Windows lockdown_network() is a no-op.
 *   - curl and the HF API discovery work on all platforms.
 * ============================================================================ */

#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#ifdef __linux__
#  include <seccomp.h>
#  include <sys/syscall.h>
#  include <sys/socket.h>
#endif

/* ---------------------------------------------------------------------------
 * In-memory curl buffer (used by HuggingFace API discovery)
 * --------------------------------------------------------------------------- */
typedef struct {
    char  *data;
    size_t size;
    size_t cap;
} mem_buf_t;

static size_t mem_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    mem_buf_t *buf = (mem_buf_t *)userdata;
    size_t total = size * nmemb;
    if (buf->size + total + 1 > buf->cap) {
        buf->cap = (buf->size + total + 1) * 2;
        char *tmp = realloc(buf->data, buf->cap);
        if (!tmp) return 0;
        buf->data = tmp;
    }
    memcpy(buf->data + buf->size, ptr, total);
    buf->size += total;
    buf->data[buf->size] = '\0';
    return total;
}

/* ---------------------------------------------------------------------------
 * HuggingFace API: find the first .gguf file in repo / tree main
 * --------------------------------------------------------------------------- */
static int hf_find_first_gguf(const char *model_id, char *out_file, size_t out_size)
{
    char api_url[1024];
    snprintf(api_url, sizeof(api_url),
             "https://huggingface.co/api/models/%s/tree/main", model_id);

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    mem_buf_t buf = { malloc(4096), 0, 4096 };
    if (!buf.data) { curl_easy_cleanup(curl); return -1; }

    curl_easy_setopt(curl, CURLOPT_URL, api_url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Wasteland/0.1");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        fprintf(stderr, "[network] HF API error: %s (HTTP %ld)\n",
                curl_easy_strerror(res), http_code);
        if (buf.size > 0) {
            fprintf(stderr, "[network] API response: %.200s...\n", buf.data);
        }
        free(buf.data);
        return -1;
    }

    /* Simple text scan: look for "path":"... .gguf" in JSON */
    char *p = buf.data;
    while ((p = strstr(p, "\"path\":\"")) != NULL) {
        p += 8; /* skip past "path":" */
        char *end = strchr(p, '"');
        if (!end) break;
        size_t len = (size_t)(end - p);
        if (len > 5 && memcmp(end - 5, ".gguf", 5) == 0) {
            size_t copy = (len < out_size - 1) ? len : out_size - 1;
            memcpy(out_file, p, copy);
            out_file[copy] = '\0';
            free(buf.data);
            return 0;
        }
        p = end + 1;
    }

    fprintf(stderr, "[network] No .gguf found in API response (%.100s...)\n",
            buf.data);
    free(buf.data);
    return -1;
}

/* ---------------------------------------------------------------------------
 * File download helpers
 * --------------------------------------------------------------------------- */
typedef struct {
    FILE *fp;
    volatile int *progress;
    volatile int *cancel;
} download_ctx_t;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    download_ctx_t *ctx = (download_ctx_t *)userdata;
    return fwrite(ptr, size, nmemb, ctx->fp);
}

static int progress_cb(void *clientp,
                       curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    download_ctx_t *ctx = (download_ctx_t *)clientp;
    (void)ultotal; (void)ulnow;
    if (ctx->cancel && *ctx->cancel) {
        return 1; /* abort transfer */
    }
    if (dltotal > 0 && ctx->progress) {
        int pct = (int)((dlnow * 100) / dltotal);
        *ctx->progress = pct;
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * network_download_model
 * --------------------------------------------------------------------------- */
int network_download_model(const char *model_id, const char *output_dir,
                           volatile int *progress,
                           volatile int *active,
                           volatile int *cancel)
{
    if (!model_id || !output_dir || !active)
        return -1;

    char url[2048];
    char outpath[1024];
    char resolved_file[512];

    /* --- Resolve the effective download URL --- */
    if (strncmp(model_id, "http", 4) == 0) {
        /* Full URL: convert /blob/main/ (11 bytes) to /resolve/main/ (14 bytes).
         * Order matters — shift the tail right by 3 *first*, then overwrite,
         * otherwise the memcpy clobbers the first bytes of the filename
         * before we get a chance to move them. */
        strncpy(url, model_id, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        char *blob = strstr(url, "/blob/main/");
        if (blob) {
            size_t tail_len = strlen(blob + 11) + 1; /* includes terminator */
            size_t needed   = (size_t)(blob - url) + 14 + tail_len;
            if (needed > sizeof(url)) {
                fprintf(stderr, "[network] URL too long after rewrite\n");
                return -1;
            }
            memmove(blob + 14, blob + 11, tail_len);
            memcpy(blob, "/resolve/main/", 14);
            fprintf(stderr, "[network] Converted blob URL to resolve: %s\n", url);
        }
    } else if (strstr(model_id, ".gguf")) {
        /* Path that already includes a filename (e.g. org/repo/resolve/main/f.gguf) */
        snprintf(url, sizeof(url), "https://huggingface.co/%s", model_id);
    } else {
        /* Bare repo ID — ask HF API for the first .gguf file */
        if (hf_find_first_gguf(model_id, resolved_file, sizeof(resolved_file)) != 0) {
            fprintf(stderr, "[network] No .gguf file found in repo '%s'\n", model_id);
            return -1;
        }
        snprintf(url, sizeof(url),
                 "https://huggingface.co/%s/resolve/main/%s",
                 model_id, resolved_file);
        fprintf(stderr, "[network] Resolved '%s' -> %s\n", model_id, url);
    }

    /* --- Output filename --- */
    const char *filename = strrchr(url, '/');
    if (!filename) filename = model_id;
    else           filename++;

    snprintf(outpath, sizeof(outpath), "%s/%s", output_dir, filename);

    FILE *fp = fopen(outpath, "wb");
    if (!fp) {
        perror("[network] fopen");
        return -1;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        return -1;
    }

    download_ctx_t ctx = { fp, progress, cancel };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Wasteland/0.1");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    *active = 1;
    if (progress) *progress = 0;
    if (cancel) *cancel = 0;

    CURLcode res = curl_easy_perform(curl);
    *active = 0;

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (cancel && *cancel) {
        fprintf(stderr, "[network] Download cancelled by user\n");
        remove(outpath);
        return -1;
    }

    if (res != CURLE_OK || http_code != 200) {
        fprintf(stderr, "[network] Download failed: %s (HTTP %ld)\n",
                curl_easy_strerror(res), http_code);
        remove(outpath); /* delete incomplete file */
        return -1;
    }

    return 0;
}

/* ---------------------------------------------------------------------------
 * network_check_update
 * --------------------------------------------------------------------------- */
int network_check_update(char *out_version, size_t out_size)
{
    if (!out_version || out_size == 0) return -1;
    out_version[0] = '\0';

    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    mem_buf_t buf = { malloc(4096), 0, 4096 };
    if (!buf.data) { curl_easy_cleanup(curl); return -1; }

    curl_easy_setopt(curl, CURLOPT_URL,
        "https://api.github.com/repos/alexbeatnik/wasteland/releases/latest");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Wasteland-AutoUpdate/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        fprintf(stderr, "[update] GitHub API error: %s (HTTP %ld)\n",
                curl_easy_strerror(res), http_code);
        free(buf.data);
        return -1;
    }

    /* Simple text scan: look for "tag_name":"vX.Y" in JSON */
    char *p = strstr(buf.data, "\"tag_name\":\"");
    if (!p) {
        fprintf(stderr, "[update] No tag_name in GitHub response\n");
        free(buf.data);
        return -1;
    }
    p += 12; /* skip past "tag_name":" */
    char *end = strchr(p, '"');
    if (!end) {
        free(buf.data);
        return -1;
    }

    /* Strip leading 'v' if present */
    if (*p == 'v' || *p == 'V') p++;

    size_t len = (size_t)(end - p);
    if (len == 0 || len >= out_size) {
        free(buf.data);
        return -1;
    }
    memcpy(out_version, p, len);
    out_version[len] = '\0';

    fprintf(stderr, "[update] Latest release: %s\n", out_version);
    free(buf.data);
    return 0;
}

/* ---------------------------------------------------------------------------
 * seccomp network lockdown (Linux only)
 * --------------------------------------------------------------------------- */
int lockdown_network(void)
{
#ifdef __linux__
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (ctx == NULL) {
        perror("[seccomp] seccomp_init");
        return -1;
    }

    /* Block creation of new IP sockets only. Filtering on address-family
     * args lets the X11 / Wayland Unix-domain socket keep working — those
     * are already open and use AF_UNIX, while we only want to deny new
     * outbound IPv4/IPv6/raw network endpoints.
     *
     * We can't filter connect/bind/sendto by sockaddr family because
     * seccomp cannot dereference user-space pointers; gating socket()
     * itself is sufficient since no new INET fd can be obtained. */
    int rc = 0;
    rc |= seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 1,
                           SCMP_A0(SCMP_CMP_EQ, AF_INET));
    rc |= seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 1,
                           SCMP_A0(SCMP_CMP_EQ, AF_INET6));
    rc |= seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 1,
                           SCMP_A0(SCMP_CMP_EQ, AF_PACKET));

    if (rc != 0) {
        fprintf(stderr, "[seccomp] Failed to add kill rules\n");
        seccomp_release(ctx);
        return -1;
    }

    rc = seccomp_load(ctx);
    seccomp_release(ctx);

    if (rc != 0) {
        perror("[seccomp] seccomp_load");
        return -1;
    }

    fprintf(stderr, "[seccomp] Network lockdown active. "
           "New AF_INET/AF_INET6/AF_PACKET sockets will SIGKILL.\n");
    return 0;
#else
    fprintf(stderr, "[seccomp] Network lockdown not available on this platform.\n");
    return 0; /* no-op on macOS / Windows */
#endif
}
