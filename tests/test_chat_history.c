#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

/* Minimal llama_chat_message definition for testing */
typedef struct llama_chat_message {
    const char * role;
    const char * content;
} llama_chat_message;

/* Copied from inference.c — pure string processing, no llama.cpp deps */
static int parse_chat_history(const char *history,
                              llama_chat_message *msgs,
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
        while (ulen > 0 && (user_start[ulen - 1] == '\r' ||
                            user_start[ulen - 1] == '\n')) ulen--;

        char *uc = (char *)malloc(ulen + 1);
        if (!uc) break;
        memcpy(uc, user_start, ulen);
        uc[ulen] = '\0';
        size_t uc_len = ulen;
        size_t uc_cap = ulen + 1;

        p = line_end + 1;

        while (p + 2 <= end && p[0] == '>' && p[1] == ' ') {
            const char *cont_start = p + 2;
            const char *cont_nl = memchr(p, '\n', (size_t)(end - p));
            const char *cont_end = cont_nl ? cont_nl : end;
            size_t clen = (size_t)(cont_end - cont_start);
            while (clen > 0 && (cont_start[clen - 1] == '\r' ||
                                cont_start[clen - 1] == '\n')) clen--;
            size_t need = uc_len + 1 + clen + 1;
            if (need > uc_cap) {
                size_t new_cap = uc_cap * 2;
                if (new_cap < need) new_cap = need;
                char *new_uc = (char *)realloc(uc, new_cap);
                if (!new_uc) { free(uc); uc = NULL; break; }
                uc = new_uc;
                uc_cap = new_cap;
            }
            uc[uc_len++] = '\n';
            memcpy(uc + uc_len, cont_start, clen);
            uc_len += clen;
            uc[uc_len] = '\0';
            p = cont_nl ? cont_nl + 1 : end;
        }

        if (!uc) break;

        msgs[n].role = "user";
        msgs[n].content = uc;
        owned[n] = uc;
        n++;
        if (p >= end) {
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
                n--;
                free(owned[n]);
                owned[n] = NULL;
            }
            break;
        }
    }

    return n;
}

/* Copied from inference.c */
static const char BASE_SYSTEM_PROMPT[] =
    "You are a helpful offline AI assistant running inside a terminal.\n"
    "- Output plain text only. No markdown formatting.\n"
    "- Be concise.\n"
    "- Match the user's language.\n"
    "- You run entirely offline.";

static void build_system_prompt(const char *user_sys, char *out, size_t out_size)
{
    if (user_sys && user_sys[0]) {
        snprintf(out, out_size, "%s\n\n%s", BASE_SYSTEM_PROMPT, user_sys);
    } else {
        snprintf(out, out_size, "%s", BASE_SYSTEM_PROMPT);
    }
}

/* Copied from inference.c — extract_summary_prefix.
 * Strips a leading "[SUMMARY]\n...\n[/SUMMARY]\n" block from `history`,
 * copies the body into summary_out, returns the byte offset past the
 * closing marker (so caller can hand the rest to parse_chat_history).
 * Returns 0 if no valid block is present. */
#define WST_SUMMARY_OPEN  "[SUMMARY]"
#define WST_SUMMARY_CLOSE "[/SUMMARY]"
static size_t extract_summary_prefix(const char *history,
                                     char *summary_out, size_t out_size)
{
    if (summary_out && out_size > 0) summary_out[0] = '\0';
    if (!history) return 0;
    const size_t open_len  = sizeof(WST_SUMMARY_OPEN)  - 1;
    const size_t close_len = sizeof(WST_SUMMARY_CLOSE) - 1;
    if (strncmp(history, WST_SUMMARY_OPEN, open_len) != 0) return 0;
    const char *p = history + open_len;
    if (*p == '\r') p++;
    if (*p != '\n') return 0;
    p++;
    const char *body = p;
    const char *end = strstr(p, WST_SUMMARY_CLOSE);
    while (end) {
        if (end == body || end[-1] == '\n') break;
        end = strstr(end + 1, WST_SUMMARY_CLOSE);
    }
    if (!end) return 0;
    size_t body_len = (size_t)(end - body);
    if (body_len > 0 && body[body_len - 1] == '\n') body_len--;
    if (body_len > 0 && body[body_len - 1] == '\r') body_len--;
    if (summary_out && out_size > 0) {
        size_t n = body_len < out_size - 1 ? body_len : out_size - 1;
        memcpy(summary_out, body, n);
        summary_out[n] = '\0';
    }
    const char *after = end + close_len;
    if (*after == '\r') after++;
    if (*after == '\n') after++;
    return (size_t)(after - history);
}

/* ------------------------------------------------------------------------- */
/* parse_chat_history tests                                                  */
/* ------------------------------------------------------------------------- */

static void test_parse_empty_history(void) {
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history("", msgs, owned, 16);
    ASSERT_EQ_INT(0, n);
}

static void test_parse_single_turn(void) {
    const char *hist = "> Hello\nHi there!\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(2, n);
    ASSERT_EQ_STR("user", msgs[0].role);
    ASSERT_EQ_STR("Hello", msgs[0].content);
    ASSERT_EQ_STR("assistant", msgs[1].role);
    ASSERT_EQ_STR("Hi there!", msgs[1].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

static void test_parse_multi_turn(void) {
    const char *hist = "> First\nReply one\n> Second\nReply two\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(4, n);
    ASSERT_EQ_STR("user", msgs[0].role);
    ASSERT_EQ_STR("First", msgs[0].content);
    ASSERT_EQ_STR("assistant", msgs[1].role);
    ASSERT_EQ_STR("Reply one", msgs[1].content);
    ASSERT_EQ_STR("user", msgs[2].role);
    ASSERT_EQ_STR("Second", msgs[2].content);
    ASSERT_EQ_STR("assistant", msgs[3].role);
    ASSERT_EQ_STR("Reply two", msgs[3].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

static void test_parse_trailing_user_no_reply(void) {
    const char *hist = "> Hello\nHi!\n> Current prompt\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(2, n);
    ASSERT_EQ_STR("user", msgs[0].role);
    ASSERT_EQ_STR("Hello", msgs[0].content);
    ASSERT_EQ_STR("assistant", msgs[1].role);
    ASSERT_EQ_STR("Hi!", msgs[1].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

static void test_parse_multiline_assistant(void) {
    const char *hist = "> Question\nLine 1\nLine 2\nLine 3\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(2, n);
    ASSERT_EQ_STR("assistant", msgs[1].role);
    ASSERT_EQ_STR("Line 1\nLine 2\nLine 3", msgs[1].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

static void test_parse_max_msgs(void) {
    const char *hist = "> A\n1\n> B\n2\n> C\n3\n";
    llama_chat_message msgs[2];
    char *owned[2] = {0};
    int n = parse_chat_history(hist, msgs, owned, 2);
    ASSERT_EQ_INT(2, n);
    ASSERT_EQ_STR("user", msgs[0].role);
    ASSERT_EQ_STR("A", msgs[0].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

/* Leading newlines / whitespace before the first prompt must be skipped, not
 * mis-parsed as an empty assistant turn. */
static void test_parse_leading_newlines(void) {
    const char *hist = "\n\n\n> Hello\nHi!\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(2, n);
    ASSERT_EQ_STR("Hello", msgs[0].content);
    ASSERT_EQ_STR("Hi!", msgs[1].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

/* CRLF line endings (Windows-style chat files) should parse the same as LF. */
static void test_parse_crlf_endings(void) {
    const char *hist = "> Hello\r\nHi there!\r\n> Second\r\nReply two\r\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(4, n);
    ASSERT_EQ_STR("Hello", msgs[0].content);
    /* Trailing \r should be stripped from the assistant content. */
    ASSERT_EQ_STR("Hi there!", msgs[1].content);
    ASSERT_EQ_STR("Second", msgs[2].content);
    ASSERT_EQ_STR("Reply two", msgs[3].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

/* UTF-8 (Cyrillic) content must round-trip without truncation. */
static void test_parse_utf8_content(void) {
    const char *hist = "> Привіт\nЯк справи?\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(2, n);
    ASSERT_EQ_STR("Привіт", msgs[0].content);
    ASSERT_EQ_STR("Як справи?", msgs[1].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

/* History containing only an in-flight user prompt (no assistant reply yet)
 * must yield zero messages — the caller supplies the prompt separately. */
static void test_parse_only_pending_prompt(void) {
    const char *hist = "> just typed\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(0, n);
}

/* `> ` appearing INSIDE an assistant reply (e.g. quoted email) must not
 * split the reply. Only `\n> ` at line-start does. */
static void test_parse_gt_inside_reply_no_split(void) {
    const char *hist =
        "> Quote my last email\n"
        "Sure, here it is: 1 > 0 means greater than.\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(2, n);
    /* The "1 > 0" snippet should remain inside the assistant text — not get
     * mis-detected as a new user turn (no '\n' before its '>'). */
    ASSERT_EQ_STR("Sure, here it is: 1 > 0 means greater than.",
                  msgs[1].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

/* Multi-line user prompt: consecutive `> ` lines must be glued back into
 * a single user message with embedded `\n`s. Lets the user paste a
 * paragraph and the chat template still sees the original line breaks. */
static void test_parse_multiline_user_prompt(void) {
    const char *hist =
        "> first paragraph line\n"
        "> second paragraph line\n"
        "> third line\n"
        "Got it, processing your three lines.\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(2, n);
    ASSERT_EQ_STR("user", msgs[0].role);
    ASSERT_EQ_STR("first paragraph line\nsecond paragraph line\nthird line",
                  msgs[0].content);
    ASSERT_EQ_STR("assistant", msgs[1].role);
    ASSERT_EQ_STR("Got it, processing your three lines.", msgs[1].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

/* Multi-line prompt followed by another multi-line prompt — both must
 * be reconstructed correctly as separate turns. */
static void test_parse_two_multiline_turns(void) {
    const char *hist =
        "> alpha 1\n"
        "> alpha 2\n"
        "Reply A\n"
        "> beta 1\n"
        "> beta 2\n"
        "Reply B\n";
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    ASSERT_EQ_INT(4, n);
    ASSERT_EQ_STR("alpha 1\nalpha 2", msgs[0].content);
    ASSERT_EQ_STR("Reply A", msgs[1].content);
    ASSERT_EQ_STR("beta 1\nbeta 2", msgs[2].content);
    ASSERT_EQ_STR("Reply B", msgs[3].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

/* Three consecutive user lines with an assistant reply in the middle. The
 * parser must split correctly across multiple newline-prefixed `> ` markers
 * and not crash on the trailing pending prompt. */
static void test_parse_three_turns_pending_last(void) {
    const char *hist =
        "> First\nReply A\n"
        "> Second\nReply B\n"
        "> Third pending\n";  /* no reply yet */
    llama_chat_message msgs[16];
    char *owned[16] = {0};
    int n = parse_chat_history(hist, msgs, owned, 16);
    /* First two pairs kept; third user is trailing pending → discarded. */
    ASSERT_EQ_INT(4, n);
    ASSERT_EQ_STR("First", msgs[0].content);
    ASSERT_EQ_STR("Reply A", msgs[1].content);
    ASSERT_EQ_STR("Second", msgs[2].content);
    ASSERT_EQ_STR("Reply B", msgs[3].content);
    for (int i = 0; i < n; i++) free(owned[i]);
}

/* ------------------------------------------------------------------------- */
/* build_system_prompt tests                                                 */
/* ------------------------------------------------------------------------- */

static void test_build_system_prompt_without_user(void) {
    char out[2048];
    build_system_prompt("", out, sizeof(out));
    ASSERT_TRUE(out[0] != '\0');
    ASSERT_TRUE(strstr(out, "plain text") != NULL);
}

static void test_build_system_prompt_with_user(void) {
    char out[2048];
    build_system_prompt("Be extra polite.", out, sizeof(out));
    ASSERT_TRUE(strstr(out, "plain text") != NULL);
    ASSERT_TRUE(strstr(out, "Be extra polite.") != NULL);
}

/* NULL user-prompt argument must produce the base prompt only, not crash. */
static void test_build_system_prompt_null_user(void) {
    char out[2048];
    build_system_prompt(NULL, out, sizeof(out));
    ASSERT_TRUE(out[0] != '\0');
    ASSERT_TRUE(strstr(out, "plain text") != NULL);
}

/* User prompt is appended AFTER the base — language-matching rule must
 * appear before any user instructions. */
static void test_build_system_prompt_order(void) {
    char out[2048];
    build_system_prompt("EXTRA_USER_RULE", out, sizeof(out));
    const char *base_marker = strstr(out, "plain text");
    const char *user_marker = strstr(out, "EXTRA_USER_RULE");
    ASSERT_NOT_NULL(base_marker);
    ASSERT_NOT_NULL(user_marker);
    ASSERT_TRUE(base_marker < user_marker);
}

/* ------------------------------------------------------------------------- */
/* extract_summary_prefix tests                                              */
/* ------------------------------------------------------------------------- */

static void test_summary_no_prefix(void) {
    char out[128] = "X";
    size_t off = extract_summary_prefix("> hi\nhello\n", out, sizeof(out));
    ASSERT_EQ_INT(0, (int)off);
    ASSERT_EQ_STR("", out);
}

static void test_summary_basic(void) {
    char out[128];
    const char *h =
        "[SUMMARY]\n"
        "User asked about X. Decided to do Y.\n"
        "[/SUMMARY]\n"
        "> next\nreply\n";
    size_t off = extract_summary_prefix(h, out, sizeof(out));
    ASSERT_TRUE(off > 0);
    ASSERT_EQ_STR("User asked about X. Decided to do Y.", out);
    ASSERT_EQ_STR("> next\nreply\n", h + off);
}

static void test_summary_multiline_body(void) {
    char out[256];
    const char *h =
        "[SUMMARY]\n"
        "Line one.\n"
        "Line two with numbers 42 and path /tmp/foo.\n"
        "Open question: Z.\n"
        "[/SUMMARY]\n"
        "> q\nr\n";
    size_t off = extract_summary_prefix(h, out, sizeof(out));
    ASSERT_TRUE(off > 0);
    ASSERT_TRUE(strstr(out, "Line one.") != NULL);
    ASSERT_TRUE(strstr(out, "Line two") != NULL);
    ASSERT_TRUE(strstr(out, "Open question") != NULL);
    ASSERT_EQ_STR("> q\nr\n", h + off);
}

static void test_summary_malformed_no_close(void) {
    char out[128] = "Z";
    const char *h = "[SUMMARY]\nbody\n> next\nreply\n";
    size_t off = extract_summary_prefix(h, out, sizeof(out));
    ASSERT_EQ_INT(0, (int)off);
    ASSERT_EQ_STR("", out);
}

static void test_summary_close_must_be_at_line_start(void) {
    char out[128] = "Z";
    /* "[/SUMMARY]" appears in the body but is not at line start */
    const char *h =
        "[SUMMARY]\n"
        "x[/SUMMARY] not a real terminator\n"
        "still body\n"
        "[/SUMMARY]\n"
        "> q\nr\n";
    size_t off = extract_summary_prefix(h, out, sizeof(out));
    ASSERT_TRUE(off > 0);
    ASSERT_TRUE(strstr(out, "still body") != NULL);
    ASSERT_EQ_STR("> q\nr\n", h + off);
}

static void test_summary_truncates_to_fit_buffer(void) {
    char out[16];
    const char *h =
        "[SUMMARY]\n"
        "1234567890ABCDEFGHIJKLMNOP\n"
        "[/SUMMARY]\n"
        "> q\nr\n";
    size_t off = extract_summary_prefix(h, out, sizeof(out));
    ASSERT_TRUE(off > 0);
    /* out buffer is 16 bytes — body got truncated to fit. */
    ASSERT_TRUE(strlen(out) <= 15);
    /* Caller still gets the correct offset to skip the full block. */
    ASSERT_EQ_STR("> q\nr\n", h + off);
}

/* ------------------------------------------------------------------------- */
/* Suite runner                                                              */
/* ------------------------------------------------------------------------- */

void run_chat_history(void) {
    printf("parse_chat_history\n");
    RUN_TEST(test_parse_empty_history);
    RUN_TEST(test_parse_single_turn);
    RUN_TEST(test_parse_multi_turn);
    RUN_TEST(test_parse_trailing_user_no_reply);
    RUN_TEST(test_parse_multiline_assistant);
    RUN_TEST(test_parse_max_msgs);

    RUN_TEST(test_parse_leading_newlines);
    RUN_TEST(test_parse_crlf_endings);
    RUN_TEST(test_parse_utf8_content);
    RUN_TEST(test_parse_only_pending_prompt);
    RUN_TEST(test_parse_gt_inside_reply_no_split);
    RUN_TEST(test_parse_three_turns_pending_last);
    RUN_TEST(test_parse_multiline_user_prompt);
    RUN_TEST(test_parse_two_multiline_turns);

    printf("\nbuild_system_prompt\n");
    RUN_TEST(test_build_system_prompt_without_user);
    RUN_TEST(test_build_system_prompt_with_user);
    RUN_TEST(test_build_system_prompt_null_user);
    RUN_TEST(test_build_system_prompt_order);

    printf("\nextract_summary_prefix\n");
    RUN_TEST(test_summary_no_prefix);
    RUN_TEST(test_summary_basic);
    RUN_TEST(test_summary_multiline_body);
    RUN_TEST(test_summary_malformed_no_close);
    RUN_TEST(test_summary_close_must_be_at_line_start);
    RUN_TEST(test_summary_truncates_to_fit_buffer);
}

TEST_MAIN(chat_history);
