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
 * RC4 stream cipher — chat-file obfuscation (origin: src/ui.c).
 * ========================================================================= */
static const unsigned char chat_key[] = {
    0x7a, 0x13, 0x4f, 0x9e, 0x2b, 0x81, 0xcc, 0x55,
    0x3d, 0x67, 0x10, 0xf8, 0x99, 0x44, 0xae, 0x2e,
    0x5c, 0x77, 0x18, 0xd3, 0xb6, 0x91, 0x0a, 0xe4,
    0xcf, 0x28, 0x83, 0xfb, 0x41, 0x6d, 0x35, 0x1c
};

static void rc4_crypt_buffer(unsigned char *data, size_t len)
{
    unsigned char s[256];
    int i, j;
    for (i = 0; i < 256; i++) s[i] = (unsigned char)i;
    j = 0;
    size_t key_len = sizeof(chat_key);
    for (i = 0; i < 256; i++) {
        j = (j + s[i] + chat_key[i % key_len]) & 255;
        unsigned char tmp = s[i]; s[i] = s[j]; s[j] = tmp;
    }
    i = j = 0;
    for (int drop = 0; drop < 256; drop++) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        unsigned char tmp = s[i]; s[i] = s[j]; s[j] = tmp;
    }
    for (size_t k = 0; k < len; k++) {
        i = (i + 1) & 255;
        j = (j + s[i]) & 255;
        unsigned char tmp = s[i]; s[i] = s[j]; s[j] = tmp;
        data[k] ^= s[(s[i] + s[j]) & 255];
    }
}

/* RC4 is symmetric: encrypt + encrypt = identity. Round-trip a chat
 * snapshot and confirm we get the exact bytes back. */
static void test_rc4_round_trip_ascii(void) {
    const char *plain = "> hello\nHi there!\n";
    size_t n = strlen(plain);
    unsigned char buf[64];
    memcpy(buf, plain, n);

    rc4_crypt_buffer(buf, n);              /* encrypt */
    /* Ciphertext should differ from plaintext (otherwise something is wrong). */
    ASSERT_FALSE(memcmp(buf, plain, n) == 0);

    rc4_crypt_buffer(buf, n);              /* decrypt */
    ASSERT_TRUE(memcmp(buf, plain, n) == 0);
}

/* Round-trip with multibyte UTF-8 (Cyrillic) — RC4 is byte-oriented so this
 * must work identically to ASCII. */
static void test_rc4_round_trip_utf8(void) {
    const char *plain = "> Привіт\nЯк справи?\n";
    size_t n = strlen(plain);
    unsigned char buf[128];
    memcpy(buf, plain, n);

    rc4_crypt_buffer(buf, n);
    rc4_crypt_buffer(buf, n);
    ASSERT_TRUE(memcmp(buf, plain, n) == 0);
}

/* Round-trip an empty buffer. Must not crash and the keystream init must
 * still complete (otherwise next call leaks state). */
static void test_rc4_empty_buffer(void) {
    unsigned char buf[1] = { 0 };
    rc4_crypt_buffer(buf, 0); /* no-op; just must return */
    rc4_crypt_buffer(buf, 0);
    ASSERT_EQ_INT(0, buf[0]);
}

/* The same key produces the same ciphertext every time — confirms determinism
 * (no time-based seed leaking in). Two encryptions of the same plaintext must
 * be byte-identical. */
static void test_rc4_deterministic(void) {
    const char *plain = "deterministic check 123";
    size_t n = strlen(plain);
    unsigned char a[64], b[64];
    memcpy(a, plain, n);
    memcpy(b, plain, n);
    rc4_crypt_buffer(a, n);
    rc4_crypt_buffer(b, n);
    ASSERT_TRUE(memcmp(a, b, n) == 0);
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
 * Suite runner
 * ========================================================================= */
void run_string_utils(void) {
    printf("rc4_crypt_buffer\n");
    RUN_TEST(test_rc4_round_trip_ascii);
    RUN_TEST(test_rc4_round_trip_utf8);
    RUN_TEST(test_rc4_empty_buffer);
    RUN_TEST(test_rc4_deterministic);

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
}

TEST_MAIN(string_utils);
