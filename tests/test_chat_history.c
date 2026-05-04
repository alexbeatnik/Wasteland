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

    printf("\nbuild_system_prompt\n");
    RUN_TEST(test_build_system_prompt_without_user);
    RUN_TEST(test_build_system_prompt_with_user);
}

TEST_MAIN(chat_history);
