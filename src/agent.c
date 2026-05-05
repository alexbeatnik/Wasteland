/* ============================================================================
 * agent.c — Tool-call parser, sandbox, and tool executors
 * ============================================================================
 *
 * _GNU_SOURCE must be defined before any standard headers so glibc exposes
 * memmem() (used by the apply_edit parser). Without this, memmem is an
 * implicit function declaration and its return pointer gets truncated on
 * 64-bit Linux, breaking the SEARCH/======/REPLACE marker scan.
 * ============================================================================ */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* ============================================================================
 *
 * Sandbox model: every path is resolved with realpath(); both the resolved
 * target AND its parent (for create-new-file) must lie strictly inside the
 * user-chosen workspace. There is no escape hatch — symlinks, "..", and
 * absolute paths that point outside the workspace all get rejected.
 *
 * Parser format taught to the model via agent_system_prompt():
 *
 *   ```read_file
 *   path/to/foo.c
 *   ```
 *
 *   ```list_dir
 *   src/
 *   ```
 *
 *   ```write_file
 *   path/to/bar.c
 *   <full new contents>
 *   ```
 *
 *   ```apply_edit
 *   path/to/foo.c
 *   <<<<<<< SEARCH
 *   <exact text to find>
 *   =======
 *   <replacement text>
 *   >>>>>>> REPLACE
 *   ```
 * ============================================================================ */

#include "agent.h"
#include "fs_sandbox.h"
#include "agent_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

#ifdef __linux__
#  include <unistd.h>
#  include <sys/socket.h>
#  include <spawn.h>
#  include <signal.h>
#  include <sys/wait.h>
#endif

/* ---------------------------------------------------------------------------
 * Optional Agent Executor subprocess (IPC backend)
 * --------------------------------------------------------------------------- */
static char g_executor_path[512] = "";
static int  g_executor_fd = -1;
#ifdef __linux__
static pid_t g_executor_pid = -1;
#endif

void agent_configure_executor_path(const char *path)
{
    if (!path) {
        g_executor_path[0] = '\0';
        return;
    }
    snprintf(g_executor_path, sizeof(g_executor_path), "%s", path);
}

void agent_executor_shutdown(void)
{
    if (g_executor_fd >= 0) {
        /* Graceful shutdown request. */
        agent_ipc_send_request(g_executor_fd, AGENT_IPC_TOOL_SHUTDOWN, "", NULL, 0);
        close(g_executor_fd);
        g_executor_fd = -1;
    }
#  ifdef __linux__
    if (g_executor_pid > 0) {
        waitpid(g_executor_pid, NULL, WNOHANG);
        g_executor_pid = -1;
    }
#  endif
}

#ifdef __linux__
static int ensure_executor(const char *workspace)
{
    if (g_executor_fd >= 0) return 0;
    if (!g_executor_path[0]) return -1;

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
        return -1;

    char *argv[] = { (char *)g_executor_path, (char *)workspace, NULL };
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, sv[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, sv[0]);
    posix_spawn_file_actions_addclose(&actions, sv[1]);

    extern char **environ;
    pid_t pid;
    if (posix_spawn(&pid, g_executor_path, &actions, NULL, argv, environ) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    posix_spawn_file_actions_destroy(&actions);
    close(sv[1]);
    g_executor_fd = sv[0];
    g_executor_pid = pid;
    return 0;
}

static int ipc_exec(const char *workspace, agent_ipc_tool_t tool,
                    const char *path,
                    const uint8_t *data, uint32_t data_len,
                    char *out, size_t out_size)
{
    if (ensure_executor(workspace) != 0) return -1;
    if (agent_ipc_send_request(g_executor_fd, tool, path, data, data_len) != 0)
        return -1;
    int32_t status;
    uint8_t resp[AGENT_IPC_MAX_DATA + 1];
    uint32_t resp_len;
    if (agent_ipc_recv_response(g_executor_fd, &status, resp, &resp_len) != 0)
        return -1;
    size_t copy = resp_len < out_size - 1 ? resp_len : out_size - 1;
    memcpy(out, resp, copy);
    out[copy] = '\0';
    return status == 0 ? 0 : -1;
}
#endif

/* ---------------------------------------------------------------------------
 * Tool executors
 * --------------------------------------------------------------------------- */

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define PATH_SEP '\\'
/* memmem is POSIX — MSVC doesn't expose it. Provide a static fallback before
 * any call site so there's no conflict with an implicit int () declaration. */
static void *memmem(const void *haystack, size_t hlen,
                    const void *needle,   size_t nlen)
{
    if (nlen == 0) return (void *)haystack;
    if (hlen < nlen) return NULL;
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, nlen) == 0)
            return (void *)(h + i);
    }
    return NULL;
}
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <libgen.h>
#  define PATH_SEP '/'
#endif

#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

/* ---------------------------------------------------------------------------
 * Path helpers
 * --------------------------------------------------------------------------- */

/* Cross-platform realpath: returns a malloc'd canonical path or NULL. */
static char *xrealpath(const char *p)
{
#ifdef _WIN32
    char *buf = (char *)malloc(PATH_MAX);
    if (!buf) return NULL;
    DWORD n = GetFullPathNameA(p, PATH_MAX, buf, NULL);
    if (n == 0 || n >= PATH_MAX) { free(buf); return NULL; }
    /* Verify it actually exists — GetFullPathName doesn't check */
    DWORD attrs = GetFileAttributesA(buf);
    if (attrs == INVALID_FILE_ATTRIBUTES) { free(buf); return NULL; }
    return buf;
#else
    return realpath(p, NULL);
#endif
}

/* True iff `child` is `parent` itself or a descendant. Both must already be
 * canonical absolute paths (no trailing slash). */
static int path_is_inside(const char *child, const char *parent)
{
    size_t plen = strlen(parent);
    if (strncmp(child, parent, plen) != 0) return 0;
    /* exact match OR next char is a separator */
    return child[plen] == '\0' || child[plen] == '/' || child[plen] == '\\';
}

/* Expand a leading "~/" or "$HOME/" to the actual home directory. Writes
 * into `out` (size out_size). Falls back to copying input verbatim if HOME
 * is unset or input doesn't begin with one of those prefixes. Caller can
 * then pass the result to realpath / fopen / mkdir.
 *
 * This makes per-user wasteland.cfg files portable across machines —
 * users can write `~/projects/foo` instead of a hard-coded absolute path. */
static void expand_home(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) {
        if (out && out_size) out[0] = '\0';
        return;
    }
    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (home && *home) {
        if (in[0] == '~' && (in[1] == '/' || in[1] == '\\' || in[1] == '\0')) {
            snprintf(out, out_size, "%s%s", home, in + 1);
            return;
        }
        if (strncmp(in, "$HOME", 5) == 0 &&
            (in[5] == '/' || in[5] == '\\' || in[5] == '\0'))
        {
            snprintf(out, out_size, "%s%s", home, in + 5);
            return;
        }
    }
    snprintf(out, out_size, "%s", in);
}

int agent_resolve_path(const char *workspace, const char *user_path,
                       char *out, size_t out_size)
{
    if (!workspace || !*workspace || !user_path || !*user_path ||
        !out || out_size < 2)
        return -1;

    /* Expand ~/foo or $HOME/foo BEFORE hitting realpath — neither libc
     * nor Windows GetFullPathName understands the shell-style expansion. */
    char ws_expanded[PATH_MAX];
    expand_home(workspace, ws_expanded, sizeof(ws_expanded));

    char *ws = xrealpath(ws_expanded);
    if (!ws) return -1;

    /* Build the joined candidate path. If user gave absolute, use it as-is;
     * otherwise glue under workspace. */
    char joined[PATH_MAX];
    int is_abs = (user_path[0] == '/' || user_path[0] == '\\');
#ifdef _WIN32
    if (!is_abs && strlen(user_path) >= 2 && user_path[1] == ':') is_abs = 1;
#endif
    if (is_abs) {
        snprintf(joined, sizeof(joined), "%s", user_path);
    } else {
        snprintf(joined, sizeof(joined), "%s%c%s", ws, PATH_SEP, user_path);
    }

    /* Try to canonicalise the full path. If the file doesn't exist yet
     * (write_file to a brand-new file), fall back to canonicalising the
     * parent directory and appending the basename. */
    char target[PATH_MAX];
    char *resolved = xrealpath(joined);
    if (resolved) {
        snprintf(target, sizeof(target), "%s", resolved);
        free(resolved);
    } else {
        /* Split into dir + base. Mutate a copy because dirname/basename
         * may modify their arg on some libcs. */
        char dirbuf[PATH_MAX];
        snprintf(dirbuf, sizeof(dirbuf), "%s", joined);
        char *last_sep = strrchr(dirbuf, '/');
#ifdef _WIN32
        char *bs = strrchr(dirbuf, '\\');
        if (bs && (!last_sep || bs > last_sep)) last_sep = bs;
#endif
        if (!last_sep) { free(ws); return -1; }
        *last_sep = '\0';
        const char *base = last_sep + 1;
        if (!*base) { free(ws); return -1; }

        char *parent = xrealpath(dirbuf[0] ? dirbuf : ".");
        if (!parent) { free(ws); return -1; }
        snprintf(target, sizeof(target), "%s%c%s", parent, PATH_SEP, base);
        free(parent);
    }

    int inside = path_is_inside(target, ws);
    free(ws);
    if (!inside) return -1;

    if (strlen(target) >= out_size) return -1;
    snprintf(out, out_size, "%s", target);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Parser
 *
 * Walk the model output looking for ``` opening fences. The first non-empty
 * line of fence content is the tool name; remaining lines are the body. The
 * fence closes at the first standalone ``` line (or end of input).
 * --------------------------------------------------------------------------- */

/* Find the next ```... fence start at or after `*pos`. Returns 1 if found,
 * 0 otherwise. Updates *pos to point AFTER the opening ```. */
static int next_fence_start(const char *text, size_t len, size_t *pos)
{
    while (*pos + 3 <= len) {
        if (text[*pos] == '`' && text[*pos + 1] == '`' && text[*pos + 2] == '`') {
            /* Must be at line start (or start of buffer) so we don't mistake
             * an inline `code` snippet for a fence. */
            if (*pos == 0 || text[*pos - 1] == '\n') {
                *pos += 3;
                return 1;
            }
        }
        (*pos)++;
    }
    return 0;
}

/* Find the closing ``` (start of line). Returns offset of first backtick of
 * the closing fence, or `len` if not found. */
static size_t find_fence_end(const char *text, size_t len, size_t start)
{
    size_t i = start;
    while (i + 3 <= len) {
        if (text[i] == '`' && text[i + 1] == '`' && text[i + 2] == '`') {
            if (i == 0 || text[i - 1] == '\n') return i;
        }
        i++;
    }
    return len;
}

/* Parse the body of one tool call into the call struct. On error,
 * sets call->kind = AGENT_TOOL_NONE. */
static void parse_one_call(agent_tool_t kind, const char *body, size_t blen,
                           agent_call_t *call)
{
    call->kind     = AGENT_TOOL_NONE;
    call->path[0]  = '\0';
    call->content  = NULL;
    call->search   = NULL;
    call->replace  = NULL;

    /* Skip leading whitespace/newlines, then read the first non-empty line as
     * the path. */
    size_t i = 0;
    while (i < blen && (body[i] == '\n' || body[i] == '\r' || body[i] == ' ' ||
                        body[i] == '\t')) i++;
    if (i >= blen) return;

    size_t path_start = i;
    while (i < blen && body[i] != '\n' && body[i] != '\r') i++;
    size_t path_len = i - path_start;
    if (path_len == 0 || path_len >= AGENT_MAX_PATH_LEN) return;
    memcpy(call->path, body + path_start, path_len);
    call->path[path_len] = '\0';
    /* Trim trailing whitespace from path */
    while (path_len > 0 && (call->path[path_len - 1] == ' ' ||
                            call->path[path_len - 1] == '\t')) {
        call->path[--path_len] = '\0';
    }
    if (path_len == 0) return;

    /* Skip the EOL after the path */
    if (i < blen && body[i] == '\r') i++;
    if (i < blen && body[i] == '\n') i++;

    /* For read-only tools, body ends here. */
    if (kind == AGENT_TOOL_READ_FILE || kind == AGENT_TOOL_LIST_DIR) {
        call->kind = kind;
        return;
    }

    /* For write_file, the rest of the body IS the file content. */
    if (kind == AGENT_TOOL_WRITE_FILE) {
        size_t rest = blen - i;
        /* Trim trailing newlines so writes don't accidentally accumulate
         * blank lines on each round-trip. */
        while (rest > 0 && (body[i + rest - 1] == '\n' ||
                            body[i + rest - 1] == '\r')) rest--;
        call->content = (char *)malloc(rest + 1);
        if (!call->content) return;
        memcpy(call->content, body + i, rest);
        call->content[rest] = '\0';
        call->kind = kind;
        return;
    }

    /* For apply_edit: parse SEARCH/======/REPLACE markers. */
    if (kind == AGENT_TOOL_APPLY_EDIT) {
        const char *rest_start = body + i;
        size_t      rest_len   = blen - i;

        const char *S_MARK = "<<<<<<< SEARCH";
        const char *D_MARK = "=======";
        const char *R_MARK = ">>>>>>> REPLACE";

        const char *p_s = (const char *)memmem(rest_start, rest_len,
                                               S_MARK, strlen(S_MARK));
        if (!p_s) return;
        const char *search_start = p_s + strlen(S_MARK);
        if (search_start < rest_start + rest_len && *search_start == '\n') search_start++;

        size_t remaining = rest_len - (size_t)(search_start - rest_start);
        const char *p_d = (const char *)memmem(search_start, remaining,
                                               D_MARK, strlen(D_MARK));
        if (!p_d) return;
        size_t search_len = (size_t)(p_d - search_start);
        /* Trim a single trailing newline before the divider */
        if (search_len > 0 && search_start[search_len - 1] == '\n') search_len--;

        const char *replace_start = p_d + strlen(D_MARK);
        if (replace_start < rest_start + rest_len && *replace_start == '\n') replace_start++;

        remaining = rest_len - (size_t)(replace_start - rest_start);
        const char *p_r = (const char *)memmem(replace_start, remaining,
                                               R_MARK, strlen(R_MARK));
        if (!p_r) return;
        size_t replace_len = (size_t)(p_r - replace_start);
        if (replace_len > 0 && replace_start[replace_len - 1] == '\n') replace_len--;

        call->search  = (char *)malloc(search_len + 1);
        call->replace = (char *)malloc(replace_len + 1);
        if (!call->search || !call->replace) {
            free(call->search);  call->search  = NULL;
            free(call->replace); call->replace = NULL;
            return;
        }
        memcpy(call->search,  search_start,  search_len);  call->search [search_len ] = '\0';
        memcpy(call->replace, replace_start, replace_len); call->replace[replace_len] = '\0';
        call->kind = kind;
        return;
    }
}

/* Map an opening-fence header line ("read_file", "write_file ", etc.) to a
 * tool kind. Returns AGENT_TOOL_NONE for non-tool fences (e.g. "c", "json"). */
static agent_tool_t header_to_kind(const char *hdr, size_t hlen)
{
    /* Trim trailing space */
    while (hlen > 0 && (hdr[hlen - 1] == ' ' || hdr[hlen - 1] == '\t' ||
                        hdr[hlen - 1] == '\r')) hlen--;
    if      (hlen == 9  && memcmp(hdr, "read_file",  9 ) == 0) return AGENT_TOOL_READ_FILE;
    else if (hlen == 8  && memcmp(hdr, "list_dir",   8 ) == 0) return AGENT_TOOL_LIST_DIR;
    else if (hlen == 10 && memcmp(hdr, "write_file", 10) == 0) return AGENT_TOOL_WRITE_FILE;
    else if (hlen == 10 && memcmp(hdr, "apply_edit", 10) == 0) return AGENT_TOOL_APPLY_EDIT;
    return AGENT_TOOL_NONE;
}

int agent_parse_calls(const char *text, agent_call_t *calls, int max)
{
    if (!text || !calls || max <= 0) return 0;
    size_t len = strlen(text);
    size_t pos = 0;
    int    n   = 0;

    while (n < max && next_fence_start(text, len, &pos)) {
        /* Header line: from `pos` until '\n'. */
        size_t h0 = pos;
        while (pos < len && text[pos] != '\n') pos++;
        size_t hlen = pos - h0;
        agent_tool_t kind = header_to_kind(text + h0, hlen);

        if (pos < len) pos++; /* skip '\n' */

        size_t end = find_fence_end(text, len, pos);
        if (kind != AGENT_TOOL_NONE) {
            parse_one_call(kind, text + pos, end - pos, &calls[n]);
            if (calls[n].kind != AGENT_TOOL_NONE) n++;
        }

        /* Advance past the closing fence line. */
        pos = (end < len) ? end + 3 : len;
        while (pos < len && text[pos] != '\n') pos++;
        if (pos < len) pos++;
    }

    return n;
}

void agent_free_calls(agent_call_t *calls, int count)
{
    if (!calls) return;
    for (int i = 0; i < count; i++) {
        free(calls[i].content); calls[i].content = NULL;
        free(calls[i].search);  calls[i].search  = NULL;
        free(calls[i].replace); calls[i].replace = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * Tool executors
 * --------------------------------------------------------------------------- */
int agent_exec_read_file(const char *workspace, const char *path,
                         char *out, size_t out_size)
{
#ifdef __linux__
    if (g_executor_path[0]) {
        return ipc_exec(workspace, AGENT_IPC_TOOL_READ_FILE, path,
                        NULL, 0, out, out_size);
    }
#endif
    if (fs_sandbox_set_workspace(workspace) != 0) {
        snprintf(out, out_size, "ERROR: invalid workspace '%s'", workspace);
        return -1;
    }
    return fs_sandbox_read_file(path, out, out_size);
}

int agent_exec_list_dir(const char *workspace, const char *path,
                        char *out, size_t out_size)
{
#ifdef __linux__
    if (g_executor_path[0]) {
        return ipc_exec(workspace, AGENT_IPC_TOOL_LIST_DIR, path,
                        NULL, 0, out, out_size);
    }
#endif
    if (fs_sandbox_set_workspace(workspace) != 0) {
        snprintf(out, out_size, "ERROR: invalid workspace '%s'", workspace);
        return -1;
    }
    return fs_sandbox_list_dir(path, out, out_size);
}

int agent_exec_write_file(const char *workspace, const char *path,
                          const char *content,
                          char *err_out, size_t err_size)
{
#ifdef __linux__
    if (g_executor_path[0]) {
        uint32_t data_len = content ? (uint32_t)strlen(content) : 0;
        int rc = ipc_exec(workspace, AGENT_IPC_TOOL_WRITE_FILE, path,
                          (const uint8_t *)content, data_len,
                          err_out, err_size);
        return rc;
    }
#endif
    if (fs_sandbox_set_workspace(workspace) != 0) {
        if (err_out) snprintf(err_out, err_size,
                              "invalid workspace '%s'", workspace);
        return -1;
    }
    return fs_sandbox_write_file(path, content, err_out, err_size);
}

int agent_exec_apply_edit(const char *workspace, const char *path,
                          const char *search, const char *replace,
                          char *err_out, size_t err_size)
{
#ifdef __linux__
    if (g_executor_path[0]) {
        /* Pack search\0replace into one data buffer. */
        size_t slen = search ? strlen(search) : 0;
        size_t rlen = replace ? strlen(replace) : 0;
        uint8_t *data = NULL;
        uint32_t data_len = 0;
        if (slen + rlen + 1 <= AGENT_IPC_MAX_DATA) {
            data = (uint8_t *)malloc(slen + rlen + 2);
            if (data) {
                memcpy(data, search, slen);
                data[slen] = '\0';
                memcpy(data + slen + 1, replace, rlen);
                data_len = (uint32_t)(slen + rlen + 1);
            }
        }
        int rc = ipc_exec(workspace, AGENT_IPC_TOOL_APPLY_EDIT, path,
                          data, data_len, err_out, err_size);
        free(data);
        return rc;
    }
#endif
    if (fs_sandbox_set_workspace(workspace) != 0) {
        if (err_out) snprintf(err_out, err_size,
                              "invalid workspace '%s'", workspace);
        return -1;
    }
    return fs_sandbox_apply_edit(path, search, replace, err_out, err_size);
}

/* ---------------------------------------------------------------------------
 * System-prompt addendum
 * --------------------------------------------------------------------------- */
const char* agent_system_prompt(void)
{
    return
"You are operating in AGENT MODE with access to the user's workspace.\n"
"You may invoke tools by emitting markdown fences with these exact headers.\n"
"Emit ONE tool call per turn, then STOP and wait for the result.\n"
"\n"
"Available tools:\n"
"\n"
"1) Read a file:\n"
"```read_file\n"
"path/to/file.ext\n"
"```\n"
"\n"
"2) List a directory:\n"
"```list_dir\n"
"path/to/dir\n"
"```\n"
"\n"
"3) Overwrite a file (full new contents — requires user approval):\n"
"```write_file\n"
"path/to/file.ext\n"
"<full new content here>\n"
"```\n"
"\n"
"4) Edit a file (find/replace one occurrence — requires user approval):\n"
"```apply_edit\n"
"path/to/file.ext\n"
"<<<<<<< SEARCH\n"
"<exact text to find — include enough context to be unique>\n"
"=======\n"
"<replacement text>\n"
">>>>>>> REPLACE\n"
"```\n"
"\n"
"Rules:\n"
"- All paths are relative to the workspace root. You cannot escape it.\n"
"- The SEARCH block must match exactly ONCE in the file or the edit fails.\n"
"- After a tool result is provided to you, decide the next action.\n"
"- When the task is done, reply in plain prose without any tool fence.\n";
}

