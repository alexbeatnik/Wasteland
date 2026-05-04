#include "test_framework.h"
#include "../src/agent.h"
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define MKDIR(p) mkdir((p), 0755)
#endif
#include <stdlib.h>

/* Scratch workspace for executor tests. Created in run_agent() before any
 * test runs; leaves files behind so the user can inspect failures. */
#define EXEC_WS "wst_test_exec_ws"

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

/* Non-tool fences (e.g. ```c, ```json) interleaved with real tool calls
 * must be ignored by the parser. */
static void test_parse_skips_non_tool_fences(void) {
    const char *out =
        "Some prose.\n"
        "```c\n"
        "int main(void) { return 0; }\n"
        "```\n"
        "Then a tool call:\n"
        "```read_file\n"
        "src/foo.c\n"
        "```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(AGENT_TOOL_READ_FILE, calls[0].kind);
    ASSERT_EQ_STR("src/foo.c", calls[0].path);
    agent_free_calls(calls, n);
}

/* Trailing whitespace on the path line must be trimmed. */
static void test_parse_trims_path_whitespace(void) {
    const char *out = "```read_file\nsrc/main.c   \n```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_STR("src/main.c", calls[0].path);
    agent_free_calls(calls, n);
}

/* Trailing whitespace AFTER the tool name on the header line. */
static void test_parse_header_with_trailing_space(void) {
    const char *out = "```list_dir   \nsubdir/\n```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(AGENT_TOOL_LIST_DIR, calls[0].kind);
    ASSERT_EQ_STR("subdir/", calls[0].path);
    agent_free_calls(calls, n);
}

/* Malformed apply_edit (missing the ======= divider) must be rejected, not
 * crash. The whole call should be dropped — kind stays NONE. */
static void test_parse_apply_edit_missing_divider(void) {
    const char *out =
        "```apply_edit\n"
        "src/foo.c\n"
        "<<<<<<< SEARCH\n"
        "old text\n"
        ">>>>>>> REPLACE\n"  /* divider missing */
        "```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(0, n);
}

/* apply_edit with multi-line SEARCH and REPLACE blocks. Trims one trailing
 * newline before the divider / closing marker. */
static void test_parse_apply_edit_multiline_blocks(void) {
    const char *out =
        "```apply_edit\n"
        "src/main.c\n"
        "<<<<<<< SEARCH\n"
        "line one\n"
        "line two\n"
        "=======\n"
        "replaced one\n"
        "replaced two\n"
        "replaced three\n"
        ">>>>>>> REPLACE\n"
        "```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_STR("line one\nline two", calls[0].search);
    ASSERT_EQ_STR("replaced one\nreplaced two\nreplaced three",
                  calls[0].replace);
    agent_free_calls(calls, n);
}

/* write_file with empty body — content should be an empty string, not NULL. */
static void test_parse_write_file_empty_content(void) {
    const char *out = "```write_file\nempty.txt\n```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_INT(AGENT_TOOL_WRITE_FILE, calls[0].kind);
    ASSERT_NOT_NULL(calls[0].content);
    ASSERT_EQ_STR("", calls[0].content);
    agent_free_calls(calls, n);
}

/* write_file with a multi-line body and trailing newlines — trailing newlines
 * are stripped to avoid accumulation across round-trips. */
static void test_parse_write_file_multiline_strip_trail(void) {
    const char *out =
        "```write_file\n"
        "doc.txt\n"
        "alpha\n"
        "beta\n"
        "gamma\n\n"
        "```";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    ASSERT_EQ_INT(1, n);
    ASSERT_EQ_STR("alpha\nbeta\ngamma", calls[0].content);
    agent_free_calls(calls, n);
}

/* Inline ` `<think>` ` style fences are NOT at line-start and must NOT be
 * picked up as tools. */
static void test_parse_ignores_inline_backtick(void) {
    const char *out = "Talking about ```read_file``` inline shouldn't trigger.";
    agent_call_t calls[AGENT_MAX_CALLS_PER_TURN];
    int n = agent_parse_calls(out, calls, AGENT_MAX_CALLS_PER_TURN);
    /* The first ``` is preceded by space, not '\n'/start-of-buffer, so it
     * is rejected. The next ``` after the word is also not preceded by
     * '\n' — also rejected. So no tool call should be parsed. */
    ASSERT_EQ_INT(0, n);
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
/* Tool executor tests (require a real scratch workspace under /tmp)         */
/* ------------------------------------------------------------------------- */

/* Helper: write `content` to <ws>/<rel>. Uses fopen directly, NOT the
 * sandboxed write executor, because we want to seed the filesystem
 * independently of the code under test. */
static int seed_file(const char *ws, const char *rel, const char *content) {
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", ws, rel);
    FILE *f = fopen(full, "wb");
    if (!f) return -1;
    if (content) fwrite(content, 1, strlen(content), f);
    fclose(f);
    return 0;
}

/* Helper: read <ws>/<rel> back into a static buffer for comparison. */
static char *read_back(const char *ws, const char *rel, char *buf, size_t cap) {
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", ws, rel);
    FILE *f = fopen(full, "rb");
    if (!f) { buf[0] = '\0'; return NULL; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static void test_exec_read_file_ok(void) {
    seed_file(EXEC_WS, "hello.txt", "hello, world\n");
    char out[256];
    int rc = agent_exec_read_file(EXEC_WS, "hello.txt", out, sizeof(out));
    ASSERT_EQ_INT(0, rc);
    ASSERT_EQ_STR("hello, world\n", out);
}

static void test_exec_read_file_missing(void) {
    char out[256];
    int rc = agent_exec_read_file(EXEC_WS, "does_not_exist.txt",
                                  out, sizeof(out));
    ASSERT_EQ_INT(-1, rc);
    /* Error string must mention what failed so the model can self-correct. */
    ASSERT_TRUE(strstr(out, "ERROR") != NULL);
}

static void test_exec_read_file_escape_blocked(void) {
    char out[256];
    int rc = agent_exec_read_file(EXEC_WS, "../../../etc/passwd",
                                  out, sizeof(out));
    ASSERT_EQ_INT(-1, rc);
    ASSERT_TRUE(strstr(out, "outside workspace") != NULL);
}

static void test_exec_write_file_creates_new(void) {
    char err[256] = "";
    int rc = agent_exec_write_file(EXEC_WS, "fresh.txt",
                                   "fresh content\n",
                                   err, sizeof(err));
    ASSERT_EQ_INT(0, rc);

    char back[256];
    ASSERT_NOT_NULL(read_back(EXEC_WS, "fresh.txt", back, sizeof(back)));
    ASSERT_EQ_STR("fresh content\n", back);
}

static void test_exec_write_file_overwrites(void) {
    seed_file(EXEC_WS, "overwrite.txt", "old content");
    char err[256] = "";
    int rc = agent_exec_write_file(EXEC_WS, "overwrite.txt",
                                   "new", err, sizeof(err));
    ASSERT_EQ_INT(0, rc);
    char back[64];
    ASSERT_NOT_NULL(read_back(EXEC_WS, "overwrite.txt", back, sizeof(back)));
    ASSERT_EQ_STR("new", back);
}

static void test_exec_write_file_escape_blocked(void) {
    char err[256] = "";
    int rc = agent_exec_write_file(EXEC_WS, "/tmp/wst_should_not_exist.txt",
                                   "evil", err, sizeof(err));
    ASSERT_EQ_INT(-1, rc);
    ASSERT_TRUE(strstr(err, "outside workspace") != NULL);
}

static void test_exec_apply_edit_ok(void) {
    seed_file(EXEC_WS, "edit_me.txt",
              "alpha\nbeta\ngamma\n");
    char err[256] = "";
    int rc = agent_exec_apply_edit(EXEC_WS, "edit_me.txt",
                                   "beta", "BETA",
                                   err, sizeof(err));
    ASSERT_EQ_INT(0, rc);
    char back[256];
    read_back(EXEC_WS, "edit_me.txt", back, sizeof(back));
    ASSERT_EQ_STR("alpha\nBETA\ngamma\n", back);
}

static void test_exec_apply_edit_missing_search(void) {
    seed_file(EXEC_WS, "no_match.txt", "alpha\nbeta\n");
    char err[256] = "";
    int rc = agent_exec_apply_edit(EXEC_WS, "no_match.txt",
                                   "ZZZZ_NOT_PRESENT_ZZZZ", "x",
                                   err, sizeof(err));
    ASSERT_EQ_INT(-1, rc);
    ASSERT_TRUE(strstr(err, "not found") != NULL);
}

static void test_exec_apply_edit_ambiguous_match(void) {
    seed_file(EXEC_WS, "ambiguous.txt",
              "x\nrepeat\ny\nrepeat\nz\n");
    char err[256] = "";
    int rc = agent_exec_apply_edit(EXEC_WS, "ambiguous.txt",
                                   "repeat", "ONCE",
                                   err, sizeof(err));
    ASSERT_EQ_INT(-1, rc);
    ASSERT_TRUE(strstr(err, "multiple") != NULL);
    /* File must be untouched when the edit is rejected. */
    char back[64];
    read_back(EXEC_WS, "ambiguous.txt", back, sizeof(back));
    ASSERT_EQ_STR("x\nrepeat\ny\nrepeat\nz\n", back);
}

static void test_exec_apply_edit_empty_search_rejected(void) {
    seed_file(EXEC_WS, "empty_search.txt", "anything");
    char err[256] = "";
    int rc = agent_exec_apply_edit(EXEC_WS, "empty_search.txt",
                                   "", "x", err, sizeof(err));
    ASSERT_EQ_INT(-1, rc);
    ASSERT_TRUE(strstr(err, "empty") != NULL);
}

static void test_exec_apply_edit_delete_block(void) {
    /* Empty REPLACE = delete the SEARCH block. */
    seed_file(EXEC_WS, "delete_me.txt", "keep\nremove me\nkeep\n");
    char err[256] = "";
    int rc = agent_exec_apply_edit(EXEC_WS, "delete_me.txt",
                                   "remove me\n", "",
                                   err, sizeof(err));
    ASSERT_EQ_INT(0, rc);
    char back[64];
    read_back(EXEC_WS, "delete_me.txt", back, sizeof(back));
    ASSERT_EQ_STR("keep\nkeep\n", back);
}

static void test_exec_list_dir_ok(void) {
    /* Pre-populate a small set of files under EXEC_WS/listing/. */
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/listing", EXEC_WS);
    MKDIR(dir);
    seed_file(EXEC_WS, "listing/aaa.txt", "a");
    seed_file(EXEC_WS, "listing/bbb.txt", "b");

    char out[1024];
    int rc = agent_exec_list_dir(EXEC_WS, "listing", out, sizeof(out));
    ASSERT_EQ_INT(0, rc);
    ASSERT_TRUE(strstr(out, "aaa.txt") != NULL);
    ASSERT_TRUE(strstr(out, "bbb.txt") != NULL);
}

static void test_exec_list_dir_escape_blocked(void) {
    char out[256];
    int rc = agent_exec_list_dir(EXEC_WS, "/etc", out, sizeof(out));
    ASSERT_EQ_INT(-1, rc);
    ASSERT_TRUE(strstr(out, "outside workspace") != NULL);
}

/* ------------------------------------------------------------------------- */
/* agent_system_prompt smoke test                                            */
/* ------------------------------------------------------------------------- */

static void test_system_prompt_describes_tools(void) {
    const char *p = agent_system_prompt();
    ASSERT_NOT_NULL(p);
    /* Every supported tool must be named so the model can emit valid fences. */
    ASSERT_TRUE(strstr(p, "read_file")  != NULL);
    ASSERT_TRUE(strstr(p, "list_dir")   != NULL);
    ASSERT_TRUE(strstr(p, "write_file") != NULL);
    ASSERT_TRUE(strstr(p, "apply_edit") != NULL);
    ASSERT_TRUE(strstr(p, "<<<<<<< SEARCH") != NULL);
    ASSERT_TRUE(strstr(p, ">>>>>>> REPLACE") != NULL);
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
    RUN_TEST(test_parse_skips_non_tool_fences);
    RUN_TEST(test_parse_trims_path_whitespace);
    RUN_TEST(test_parse_header_with_trailing_space);
    RUN_TEST(test_parse_apply_edit_missing_divider);
    RUN_TEST(test_parse_apply_edit_multiline_blocks);
    RUN_TEST(test_parse_write_file_empty_content);
    RUN_TEST(test_parse_write_file_multiline_strip_trail);
    RUN_TEST(test_parse_ignores_inline_backtick);

    printf("\nagent_resolve_path\n");
    /* Ensure sandbox workspace exists for path resolution tests */
    MKDIR("wst_test_ws");
#ifdef _WIN32
    _mkdir("wst_test_ws\\src");
    _mkdir("wst_test_ws\\new_dir");
#else
    mkdir("wst_test_ws/src", 0755);
    mkdir("wst_test_ws/new_dir", 0755);
#endif
    RUN_TEST(test_resolve_inside_workspace);
    RUN_TEST(test_resolve_rejects_escape);
    RUN_TEST(test_resolve_rejects_absolute_outside);
    RUN_TEST(test_resolve_new_file_in_workspace);
    RUN_TEST(test_resolve_dot_slash);

    printf("\nagent_exec_*\n");
    /* Scratch tree for the executor tests — a separate workspace from the
     * resolve_path tests so we can leave files behind without polluting it. */
    MKDIR(EXEC_WS);
    RUN_TEST(test_exec_read_file_ok);
    RUN_TEST(test_exec_read_file_missing);
    RUN_TEST(test_exec_read_file_escape_blocked);
    RUN_TEST(test_exec_write_file_creates_new);
    RUN_TEST(test_exec_write_file_overwrites);
    RUN_TEST(test_exec_write_file_escape_blocked);
    RUN_TEST(test_exec_apply_edit_ok);
    RUN_TEST(test_exec_apply_edit_missing_search);
    RUN_TEST(test_exec_apply_edit_ambiguous_match);
    RUN_TEST(test_exec_apply_edit_empty_search_rejected);
    RUN_TEST(test_exec_apply_edit_delete_block);
    RUN_TEST(test_exec_list_dir_ok);
    RUN_TEST(test_exec_list_dir_escape_blocked);

    printf("\nagent_system_prompt\n");
    RUN_TEST(test_system_prompt_describes_tools);
}

TEST_MAIN(agent);
