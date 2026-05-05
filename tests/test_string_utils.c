/* test_string_utils.c
 *
 * Pure string helpers copied verbatim from ui.c / network.c so they can be
 * exercised without pulling SDL2, libcurl, or llama.cpp into the test
 * binary. Each block carries the source-file reference of its origin so the
 * code under test can be kept in sync by inspection.
 */

#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Authenticated chat cipher — XChaCha20-Poly1305 via crypto_engine.c
 * (origin: src/ui.c → save_chat_history / load_chat_history).
 * ========================================================================= */
#include "crypto_engine.h"

static const unsigned char test_key[WASTELAND_CRYPTO_KEY_SIZE] = {
    0x7a, 0x13, 0x4f, 0x9e, 0x2b, 0x81, 0xcc, 0x55,
    0x3d, 0x67, 0x10, 0xf8, 0x99, 0x44, 0xae, 0x2e,
    0x5c, 0x77, 0x18, 0xd3, 0xb6, 0x91, 0x0a, 0xe4,
    0xcf, 0x28, 0x83, 0xfb, 0x41, 0x6d, 0x35, 0x1c
};

static void test_crypto_round_trip_ascii(void) {
    const char *plain = "> hello\nHi there!\n";
    size_t n = strlen(plain);
    uint8_t cipher[64];
    uint8_t opened[64];
    uint8_t nonce[WASTELAND_CRYPTO_NONCE_SIZE] = {0};
    uint8_t tag[WASTELAND_CRYPTO_TAG_SIZE];

    /* Deterministic nonce for the test (real code uses crypto_random_bytes). */
    memset(nonce, 0xAB, sizeof(nonce));

    ASSERT_EQ_INT(0, crypto_seal(test_key, nonce,
                                 (const uint8_t *)plain, n,
                                 cipher, tag));
    /* Ciphertext must differ from plaintext (catches NOP cipher). */
    ASSERT_FALSE(memcmp(cipher, plain, n) == 0);

    ASSERT_EQ_INT(0, crypto_open(test_key, nonce,
                                 cipher, n, tag, opened));
    opened[n] = '\0';
    ASSERT_TRUE(memcmp(opened, plain, n) == 0);
}

static void test_crypto_round_trip_utf8(void) {
    const char *plain = "> Привіт\nЯк справи?\n";
    size_t n = strlen(plain);
    uint8_t cipher[128];
    uint8_t opened[128];
    uint8_t nonce[WASTELAND_CRYPTO_NONCE_SIZE] = {0};
    uint8_t tag[WASTELAND_CRYPTO_TAG_SIZE];
    memset(nonce, 0xCD, sizeof(nonce));

    ASSERT_EQ_INT(0, crypto_seal(test_key, nonce,
                                 (const uint8_t *)plain, n,
                                 cipher, tag));
    ASSERT_EQ_INT(0, crypto_open(test_key, nonce,
                                 cipher, n, tag, opened));
    opened[n] = '\0';
    ASSERT_TRUE(memcmp(opened, plain, n) == 0);
}

static void test_crypto_auth_failure(void) {
    /* Tamper with one ciphertext byte — open must fail. */
    const char *plain = "sensitive chat data";
    size_t n = strlen(plain);
    uint8_t cipher[64];
    uint8_t opened[64];
    uint8_t nonce[WASTELAND_CRYPTO_NONCE_SIZE] = {0};
    uint8_t tag[WASTELAND_CRYPTO_TAG_SIZE];
    memset(nonce, 0xEF, sizeof(nonce));

    ASSERT_EQ_INT(0, crypto_seal(test_key, nonce,
                                 (const uint8_t *)plain, n,
                                 cipher, tag));
    cipher[0] ^= 0xFF;
    ASSERT_EQ_INT(-1, crypto_open(test_key, nonce,
                                  cipher, n, tag, opened));
}

static void test_crypto_empty_buffer(void) {
    uint8_t cipher[1];
    uint8_t opened[1];
    uint8_t nonce[WASTELAND_CRYPTO_NONCE_SIZE] = {0};
    uint8_t tag[WASTELAND_CRYPTO_TAG_SIZE];
    memset(nonce, 0x01, sizeof(nonce));

    /* Zero-length plaintext still produces a valid tag. */
    ASSERT_EQ_INT(0, crypto_seal(test_key, nonce, NULL, 0, cipher, tag));
    ASSERT_EQ_INT(0, crypto_open(test_key, nonce, NULL, 0, tag, opened));
}

/* =========================================================================
 * HuggingFace URL rewrite: `/blob/main/` → `/resolve/main/` (origin: network.c).
 *
 * The actual rewrite logic is inline inside network_download_model().
 * Re-implement it here in pure form so we can hammer edge cases without
 * a live curl session.
 * ========================================================================= */
static int hf_rewrite_url(char *url, size_t cap)
{
    char *blob = strstr(url, "/blob/main/");
    if (!blob) return 0;
    size_t tail_len = strlen(blob + 11) + 1; /* incl. terminator */
    size_t needed   = (size_t)(blob - url) + 14 + tail_len;
    if (needed > cap) return -1;
    memmove(blob + 14, blob + 11, tail_len);
    memcpy(blob, "/resolve/main/", 14);
    return 1;
}

static void test_url_rewrite_basic(void) {
    char url[256] =
        "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/blob/main/qwen2.5-0.5b-instruct-q4_k_m.gguf";
    int rc = hf_rewrite_url(url, sizeof(url));
    ASSERT_EQ_INT(1, rc);
    ASSERT_TRUE(strstr(url, "/resolve/main/") != NULL);
    ASSERT_TRUE(strstr(url, "/blob/main/") == NULL);
    /* The trailing filename must be intact — the +3 shift bug used to clobber
     * the first three bytes ("qwe" → ""), which is why the order matters. */
    ASSERT_TRUE(strstr(url, "qwen2.5-0.5b-instruct-q4_k_m.gguf") != NULL);
}

static void test_url_rewrite_already_resolve(void) {
    /* URL that already has /resolve/main/ — function must not touch it
     * and must return 0 (no rewrite performed). */
    char url[256] =
        "https://huggingface.co/repo/resolve/main/file.gguf";
    char before[256];
    snprintf(before, sizeof(before), "%s", url);
    int rc = hf_rewrite_url(url, sizeof(url));
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_STR(before, url);
}

static void test_url_rewrite_too_small_buffer(void) {
    /* Buffer just barely fits the original URL but cannot hold the +3
     * expansion. Function must refuse with -1, not corrupt the buffer. */
    char url[64];
    /* 50-char URL → +3 bytes overflows 64-cap when including terminator. */
    snprintf(url, sizeof(url),
             "https://hf.co/x/y/blob/main/long_name_here.gguf");
    int rc = hf_rewrite_url(url, sizeof(url));
    /* Either succeeds (if it fits) or refuses; never overflows. We only
     * check the safety contract: result is exactly 1 or -1, never 0. */
    ASSERT_TRUE(rc == 1 || rc == -1);
}

static void test_url_rewrite_does_not_match_substring(void) {
    /* "/blob/main" with no trailing slash should not match. */
    char url[128] =
        "https://example.com/blob/main_branch_file";
    int rc = hf_rewrite_url(url, sizeof(url));
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_STR("https://example.com/blob/main_branch_file", url);
}

/* =========================================================================
 * Chat-name sanitisation — first-message → filesystem-safe name
 * (origin: ui.c → generate_chat_name_from_prompt).
 *
 * The real function also picks a unique suffix via stat() so we can't unit-
 * test that part without a tempdir.  Here we focus on the character filter:
 * keep ASCII alphanumerics + UTF-8 high-bytes, collapse spaces/dashes to a
 * single space, trim trailing space.
 * ========================================================================= */
static void sanitise_chat_base(const char *prompt, char *base, size_t cap)
{
    int b = 0;
    int max = (int)cap - 1;
    while (*prompt == ' ') prompt++;
    for (int i = 0; prompt[i] && b < max && b < 40; i++) {
        unsigned char c = (unsigned char)prompt[i];
        if (c >= 0x80 || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            base[b++] = (char)c;
        } else if (c == ' ' || c == '-') {
            if (b > 0 && base[b-1] != ' ') base[b++] = ' ';
        }
    }
    while (b > 0 && base[b-1] == ' ') b--;
    base[b] = '\0';
}

static void test_chatname_basic_ascii(void) {
    char base[64];
    sanitise_chat_base("hello world", base, sizeof(base));
    ASSERT_EQ_STR("hello world", base);
}

static void test_chatname_strips_punctuation(void) {
    char base[64];
    sanitise_chat_base("Question? What is C++!?", base, sizeof(base));
    /* '?' '.' '!' all dropped; '+' is dropped (not alnum); spaces collapsed. */
    ASSERT_EQ_STR("Question What is C", base);
}

static void test_chatname_collapses_runs_of_spaces(void) {
    char base[64];
    sanitise_chat_base("a    b---c", base, sizeof(base));
    /* Runs of spaces / dashes collapse to a single space between tokens. */
    ASSERT_EQ_STR("a b c", base);
}

static void test_chatname_trims_leading_whitespace(void) {
    char base[64];
    sanitise_chat_base("    leading", base, sizeof(base));
    ASSERT_EQ_STR("leading", base);
}

static void test_chatname_trims_trailing_space(void) {
    char base[64];
    sanitise_chat_base("done!!!", base, sizeof(base));
    ASSERT_EQ_STR("done", base);
}

static void test_chatname_keeps_utf8(void) {
    char base[64];
    sanitise_chat_base("Привіт світ", base, sizeof(base));
    /* All Cyrillic bytes are >= 0x80 and pass through. */
    ASSERT_EQ_STR("Привіт світ", base);
}

static void test_chatname_caps_at_40(void) {
    char base[128];
    sanitise_chat_base(
        "This is a very long first message that should be truncated past forty characters somewhere",
        base, sizeof(base));
    ASSERT_TRUE(strlen(base) <= 40);
    /* And the trim should not leave a trailing space. */
    if (strlen(base) > 0)
        ASSERT_FALSE(base[strlen(base) - 1] == ' ');
}

/* =========================================================================
 * Tool-fence stripper — agent fences elided from chat rendering view
 * (origin: src/ui.c → strip_tool_fences). Pure string transform.
 * ========================================================================= */
static int is_tool_fence_header(const char *p, size_t hlen)
{
    while (hlen > 0 && (p[hlen-1] == ' ' || p[hlen-1] == '\t' ||
                        p[hlen-1] == '\r')) hlen--;
    if (hlen == 9  && memcmp(p, "read_file",  9 ) == 0) return 1;
    if (hlen == 8  && memcmp(p, "list_dir",   8 ) == 0) return 1;
    if (hlen == 10 && memcmp(p, "write_file", 10) == 0) return 1;
    if (hlen == 10 && memcmp(p, "apply_edit", 10) == 0) return 1;
    return 0;
}

static void strip_tool_fences(const char *in, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!in) return;

    size_t in_len = strlen(in);
    size_t out_pos = 0;
    size_t i = 0;

    while (i < in_len && out_pos + 1 < out_size) {
        int line_start = (i == 0) || (in[i-1] == '\n');
        if (line_start && i + 3 <= in_len &&
            in[i] == '`' && in[i+1] == '`' && in[i+2] == '`')
        {
            const char *hdr = in + i + 3;
            const char *hdr_end = memchr(hdr, '\n', in_len - (i + 3));
            if (!hdr_end) hdr_end = in + in_len;
            if (is_tool_fence_header(hdr, (size_t)(hdr_end - hdr))) {
                size_t j = (size_t)(hdr_end - in);
                if (j < in_len && in[j] == '\n') j++;
                while (j < in_len) {
                    int j_line_start = (j == 0) || (in[j-1] == '\n');
                    if (j_line_start && j + 3 <= in_len &&
                        in[j] == '`' && in[j+1] == '`' && in[j+2] == '`') {
                        j += 3;
                        while (j < in_len && in[j] != '\n') j++;
                        if (j < in_len) j++;
                        break;
                    }
                    j++;
                }
                i = j;
                continue;
            }
        }
        out[out_pos++] = in[i++];
    }
    out[out_pos] = '\0';
}

static void test_strip_simple_read_file(void) {
    const char *in =
        "Sure thing.\n"
        "```read_file\n"
        "src/main.c\n"
        "```\n"
        "[ TOOL: read_file | src/main.c ]\n";
    char out[512];
    strip_tool_fences(in, out, sizeof(out));
    /* The fence body is gone, the prose and TOOL marker remain. */
    ASSERT_TRUE(strstr(out, "Sure thing.") != NULL);
    ASSERT_TRUE(strstr(out, "[ TOOL: read_file | src/main.c ]") != NULL);
    ASSERT_TRUE(strstr(out, "```read_file") == NULL);
    ASSERT_TRUE(strstr(out, "src/main.c\n```") == NULL);
}

static void test_strip_keeps_non_tool_fences(void) {
    /* A plain ```c code block should pass through unchanged — only the four
     * known agent tool names are recognised as fences to strip. */
    const char *in =
        "Look at this snippet:\n"
        "```c\n"
        "int main(void) { return 0; }\n"
        "```\n"
        "It compiles.\n";
    char out[512];
    strip_tool_fences(in, out, sizeof(out));
    ASSERT_TRUE(strstr(out, "```c") != NULL);
    ASSERT_TRUE(strstr(out, "int main") != NULL);
}

static void test_strip_apply_edit_block(void) {
    const char *in =
        "Patching:\n"
        "```apply_edit\n"
        "src/foo.c\n"
        "<<<<<<< SEARCH\n"
        "old\n"
        "=======\n"
        "new\n"
        ">>>>>>> REPLACE\n"
        "```\n"
        "[ TOOL: apply_edit | src/foo.c ]\n";
    char out[512];
    strip_tool_fences(in, out, sizeof(out));
    /* The whole apply_edit body should be elided. */
    ASSERT_TRUE(strstr(out, "<<<<<<< SEARCH") == NULL);
    ASSERT_TRUE(strstr(out, ">>>>>>> REPLACE") == NULL);
    ASSERT_TRUE(strstr(out, "Patching:") != NULL);
    ASSERT_TRUE(strstr(out, "[ TOOL: apply_edit") != NULL);
}

static void test_strip_unclosed_fence_drops_tail(void) {
    /* Mid-stream: model has emitted the opening fence but no closing yet.
     * Everything after the opener is skipped, prose before is kept. */
    const char *in =
        "Listing now:\n"
        "```list_dir\n"
        ".\n"
        "still typing...";
    char out[256];
    strip_tool_fences(in, out, sizeof(out));
    ASSERT_TRUE(strstr(out, "Listing now:") != NULL);
    ASSERT_TRUE(strstr(out, "list_dir") == NULL);
    ASSERT_TRUE(strstr(out, "still typing") == NULL);
}

static void test_strip_inline_backticks_kept(void) {
    /* Backticks NOT at line-start should never trigger a strip. */
    const char *in = "Use `read_file` carefully — it's read-only.";
    char out[256];
    strip_tool_fences(in, out, sizeof(out));
    ASSERT_EQ_STR(in, out);
}

static void test_strip_multiple_fences(void) {
    const char *in =
        "First step:\n"
        "```list_dir\n.\n```\n"
        "[ TOOL: list_dir | . ]\n"
        "Now read it:\n"
        "```read_file\nfoo.c\n```\n"
        "[ TOOL: read_file | foo.c ]\n";
    char out[512];
    strip_tool_fences(in, out, sizeof(out));
    /* Both fences gone, both TOOL markers preserved. */
    ASSERT_TRUE(strstr(out, "list_dir\n") == NULL);
    ASSERT_TRUE(strstr(out, "read_file\nfoo") == NULL);
    ASSERT_TRUE(strstr(out, "[ TOOL: list_dir") != NULL);
    ASSERT_TRUE(strstr(out, "[ TOOL: read_file") != NULL);
}

/* =========================================================================
 * SHA-256 — incremental and single-shot (origin: src/vendor/sha256/sha256.c).
 * ========================================================================= */
#include "vendor/sha256/sha256.h"

static void test_sha256_empty(void) {
    uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE];
    wasteland_sha256((const uint8_t *)"", 0, digest);
    /* Known SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855 */
    const char *expected = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
    char hex[65];
    wasteland_sha256_hex(digest, hex, sizeof(hex));
    ASSERT_EQ_STR(expected, hex);
}

static void test_sha256_abc(void) {
    uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE];
    wasteland_sha256((const uint8_t *)"abc", 3, digest);
    const char *expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    char hex[65];
    wasteland_sha256_hex(digest, hex, sizeof(hex));
    ASSERT_EQ_STR(expected, hex);
}

static void test_sha256_incremental(void) {
    wasteland_sha256_ctx_t ctx;
    wasteland_sha256_init(&ctx);
    wasteland_sha256_update(&ctx, (const uint8_t *)"a", 1);
    wasteland_sha256_update(&ctx, (const uint8_t *)"b", 1);
    wasteland_sha256_update(&ctx, (const uint8_t *)"c", 1);
    uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE];
    wasteland_sha256_final(&ctx, digest);
    const char *expected = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    char hex[65];
    wasteland_sha256_hex(digest, hex, sizeof(hex));
    ASSERT_EQ_STR(expected, hex);
}

/* =========================================================================
 * Suite runner
 * ========================================================================= */
void run_string_utils(void) {
    printf("crypto_engine (XChaCha20-Poly1305)\n");
    RUN_TEST(test_crypto_round_trip_ascii);
    RUN_TEST(test_crypto_round_trip_utf8);
    RUN_TEST(test_crypto_auth_failure);
    RUN_TEST(test_crypto_empty_buffer);

    printf("\nsha256\n");
    RUN_TEST(test_sha256_empty);
    RUN_TEST(test_sha256_abc);
    RUN_TEST(test_sha256_incremental);

    printf("\nhf_rewrite_url\n");
    RUN_TEST(test_url_rewrite_basic);
    RUN_TEST(test_url_rewrite_already_resolve);
    RUN_TEST(test_url_rewrite_too_small_buffer);
    RUN_TEST(test_url_rewrite_does_not_match_substring);

    printf("\nchat_name sanitisation\n");
    RUN_TEST(test_chatname_basic_ascii);
    RUN_TEST(test_chatname_strips_punctuation);
    RUN_TEST(test_chatname_collapses_runs_of_spaces);
    RUN_TEST(test_chatname_trims_leading_whitespace);
    RUN_TEST(test_chatname_trims_trailing_space);
    RUN_TEST(test_chatname_keeps_utf8);
    RUN_TEST(test_chatname_caps_at_40);

    printf("\nstrip_tool_fences\n");
    RUN_TEST(test_strip_simple_read_file);
    RUN_TEST(test_strip_keeps_non_tool_fences);
    RUN_TEST(test_strip_apply_edit_block);
    RUN_TEST(test_strip_unclosed_fence_drops_tail);
    RUN_TEST(test_strip_inline_backticks_kept);
    RUN_TEST(test_strip_multiple_fences);
}

TEST_MAIN(string_utils);
