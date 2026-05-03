#ifndef WASTELAND_AGENT_H
#define WASTELAND_AGENT_H

/* ---------------------------------------------------------------------------
 * Wasteland Agent Module
 *
 * Parses the model's streamed output for tool-call markdown fences,
 * sandboxes file paths against a user-chosen workspace directory, and
 * executes the four MVP tools: read_file, list_dir, write_file, apply_edit.
 *
 * Threading: pure functions over caller-owned buffers. The worker thread
 * (or any caller) drives the lifecycle. There is no global state inside
 * this module.
 * --------------------------------------------------------------------------- */

#include <stddef.h>

#define AGENT_MAX_CALLS_PER_TURN 16
#define AGENT_MAX_PATH_LEN       1024
#define AGENT_MAX_TOOL_RESULT    16384  /* truncates very large reads */

typedef enum {
    AGENT_TOOL_NONE = 0,
    AGENT_TOOL_READ_FILE,    /* read-only, auto-approve */
    AGENT_TOOL_LIST_DIR,     /* read-only, auto-approve */
    AGENT_TOOL_WRITE_FILE,   /* gated: needs user APPLY */
    AGENT_TOOL_APPLY_EDIT    /* gated: needs user APPLY */
} agent_tool_t;

/**
 * One tool invocation parsed from the model's output. `content`, `search`,
 * and `replace` are heap-allocated when present; agent_free_calls() cleans
 * them up.
 */
typedef struct {
    agent_tool_t kind;
    char  path[AGENT_MAX_PATH_LEN];
    char *content;   /* write_file: full new contents */
    char *search;    /* apply_edit: the SEARCH block (find target) */
    char *replace;   /* apply_edit: the REPLACE block (new text) */
} agent_call_t;

/**
 * Scan `model_output` for tool-call fences and fill `calls[0..max-1]`.
 * Returns the number of calls found (0..max). Calls are listed in the
 * order they appear in the output.
 */
int  agent_parse_calls(const char *model_output,
                       agent_call_t *calls, int max);

/**
 * Free heap-allocated fields inside `calls[0..count-1]`. Always safe to
 * call even if some entries had no allocations.
 */
void agent_free_calls(agent_call_t *calls, int count);

/**
 * Validate that `user_path` (relative or absolute) resolves to a location
 * strictly inside `workspace`. Writes the canonical absolute path to `out`.
 *
 * Handles the "file doesn't exist yet" case (write_file to a new file) by
 * resolving the parent directory and appending the basename.
 *
 * @return 0 on success; -1 if the path escapes the workspace, the workspace
 *         itself is invalid, or the parent dir doesn't exist.
 */
int  agent_resolve_path(const char *workspace, const char *user_path,
                        char *out, size_t out_size);

/* Read-only tool executors. On success, return 0 and write a textual
 * representation of the result to `out`. On failure return -1 and write
 * a short error message to `out` (so the model sees what went wrong). */
int  agent_exec_read_file(const char *workspace, const char *path,
                          char *out, size_t out_size);
int  agent_exec_list_dir (const char *workspace, const char *path,
                          char *out, size_t out_size);

/* Mutating tool executors — call these only AFTER the user clicks APPLY. */
int  agent_exec_write_file(const char *workspace, const char *path,
                           const char *content,
                           char *err_out, size_t err_size);
int  agent_exec_apply_edit(const char *workspace, const char *path,
                           const char *search, const char *replace,
                           char *err_out, size_t err_size);

/**
 * The system-prompt addendum that teaches the model how to emit tool calls.
 * Returned pointer is to a static string; do not free.
 */
const char* agent_system_prompt(void);

#endif /* WASTELAND_AGENT_H */
