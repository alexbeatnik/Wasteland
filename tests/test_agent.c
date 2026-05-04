#include "test_framework.h"
#include "../src/agent.h"
#include <string.h>
#ifdef _WIN32
#  include <direct.h>
#else
#  include <sys/stat.h>
#endif
#include <stdlib.h>

/* ------------------------------------------------------------------------- */
/* agent_parse_calls tests                                                   */
/* ------------------------------------------------------------------------- */

static void test_parse_read_file(void) {
    const char *out = "```read_file\nsrc/main.c\n```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(AGENT_TOOL_READ_FILE, calls[0].kind);
    ASSERT_EQ_STR("src/main.c", calls[0].path);
    agent_free_calls(calls, n);
}

static void test_parse_list_dir(void) {
    const char *out = "```list_dir\nmodels/\n```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(AGENT_TOOL_LIST_DIR, calls[0].kind);
    ASSERT_EQ_STR("models/", calls[0].path);
    agent_free_calls(calls, n);
}

static void test_parse_write_file(void) {
    const char *out = "```write_file\nconfig.txt\nhello world\n```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(AGENT_TOOL_WRITE_FILE, calls[0].kind);
    ASSERT_EQ_STR("config.txt", calls[0].path);
    ASSERT_NOT_NULL(calls[0].content);
    ASSERT_EQ_STR("hello world", calls[0].content);
    agent_free_calls(calls, n);
}

static void test_parse_apply_edit(void) {
    const char *out =
        "```apply_edit\n"
        "src/main.c\n"
        "<<<<<<< SEARCH\n"
        "old text\n"
        "=======\n"
        "new text\n"
        ">>>>>>> REPLACE\n"
        "```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(AGENT_TOOL_APPLY_EDIT, calls[0].kind);
    ASSERT_EQ_STR("src/main.c", calls[0].path);
    ASSERT_NOT_NULL(calls[0].search);
    ASSERT_NOT_NULL(calls[0].replace);
    ASSERT_EQ_STR("old text", calls[0].search);
    ASSERT_EQ_STR("new text", calls[0].replace);
    agent_free_calls(calls, n);
}

static void test_parse_multiple_calls(void) {
    const char *out =
        "```read_file\nfoo.c\n```\n"
        "```list_dir\n.\n```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(2, n);
    ASSERT_EQ_INT(AGENT_TOOL_READ_FILE, calls[0].kind);
    ASSERT_EQ_INT(AGENT_TOOL_LIST_DIR, calls[1].kind);
    agent_free_calls(calls, n);
}

static void test_parse_empty_input(void) {
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls("", calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(0, n);
}

static void test_parse_no_fence(void) {
    const char *out = "Just some regular text without any tool calls.";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(0, n);
}

static void test_parse_max_limit(void) {
    const char *out =
        "```read_file\na\n```\n"
        "```read_file\nb\n```\n"
        "```read_file\nc\n```";
    agent_call_t calls[2];
    int n = agent_parse_calls(out, calls, 2);
    ASSERT_EQ_INT(2, n);
    agent_free_calls(calls, n);
}

/* ------------------------------------------------------------------------- */
/* agent_resolve_path sandbox tests                                          */
/* ------------------------------------------------------------------------- */

static void test_resolve_inside_workspace(void) {
    char out[AGENT_MAX_PATH_LEN];
    int rc = agent_resolve_path("wst_test_ws", "src/main.c",
                                out, sizeof(out));
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(strstr(out, "wst_test_ws") != NULL);
    ASSERT_TRUE(strstr(out, "src") != NULL);
    ASSERT_TRUE(strstr(out, "main.c") != NULL);
}

static void test_resolve_rejects_escape(void) {
    char out[AGENT_MAX_PATH_LEN];
    int rc = agent_resolve_path("wst_test_ws", "../etc/passwd",
                                out, sizeof(out));
    ASSERT_EQ_INT(-1, rc);
}

static void test_resolve_rejects_absolute_outside(void) {
    char out[AGENT_MAX_PATH_LEN];
    int rc = agent_resolve_path("wst_test_ws", "/etc/passwd",
                                out, sizeof(out));
    ASSERT_EQ_INT(-1, rc);
}

static void test_resolve_new_file_in_workspace(void) {
    char out[AGENT_MAX_PATH_LEN];
    int rc = agent_resolve_path("wst_test_ws", "new_dir/new_file.txt",
                                out, sizeof(out));
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(strstr(out, "wst_test_ws") != NULL);
    ASSERT_TRUE(strstr(out, "new_dir") != NULL);
    ASSERT_TRUE(strstr(out, "new_file.txt") != NULL);
}

static void test_resolve_dot_slash(void) {
    char out[AGENT_MAX_PATH_LEN];
    int rc = agent_resolve_path("wst_test_ws", "./foo.c",
                                out, sizeof(out));
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(strstr(out, "wst_test_ws") != NULL);
    ASSERT_TRUE(strstr(out, "foo.c") != NULL);
}

/* ------------------------------------------------------------------------- */
/* Suite runner                                                              */
/* ------------------------------------------------------------------------- */

void run_agent(void) {
    printf("agent_parse_calls\n");
    RUN_TEST(test_parse_read_file);
    RUN_TEST(test_parse_list_dir);
    RUN_TEST(test_parse_write_file);
    RUN_TEST(test_parse_apply_edit);
    RUN_TEST(test_parse_multiple_calls);
    RUN_TEST(test_parse_empty_input);
    RUN_TEST(test_parse_no_fence);
    RUN_TEST(test_parse_max_limit);

    printf("\nagent_resolve_path\n");
    /* Ensure sandbox workspace exists for path resolution tests */
#ifdef _WIN32
    _mkdir("wst_test_ws");
    _mkdir("wst_test_ws\\src");
    _mkdir("wst_test_ws\\new_dir");
#else
    mkdir("wst_test_ws", 0755);
    mkdir("wst_test_ws/src", 0755);
    mkdir("wst_test_ws/new_dir", 0755);
#endif
    RUN_TEST(test_resolve_inside_workspace);
    RUN_TEST(test_resolve_rejects_escape);
    RUN_TEST(test_resolve_rejects_absolute_outside);
    RUN_TEST(test_resolve_new_file_in_workspace);
    RUN_TEST(test_resolve_dot_slash);
}

TEST_MAIN(agent);
