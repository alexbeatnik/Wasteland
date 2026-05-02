/* ============================================================================
 * agent.c — Tool-call parser, sandbox, and tool executors
 * ============================================================================
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
#  define PATH_SEP '\\'
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

int agent_resolve_path(const char *workspace, const char *user_path,
                       char *out, size_t out_size)
{
    if (!workspace || !*workspace || !user_path || !*user_path ||
        !out || out_size < 2)
        return -1;

    char *ws = xrealpath(workspace);
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
        snprintf(joined, sizeof(joined), "%s/%s", ws, user_path);
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
        snprintf(target, sizeof(target), "%s/%s", parent, base);
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
    if (!out || out_size == 0) return -1;
    char real[PATH_MAX];
    if (agent_resolve_path(workspace, path, real, sizeof(real)) != 0) {
        snprintf(out, out_size, "ERROR: path '%s' is outside workspace", path);
        return -1;
    }
    FILE *f = fopen(real, "rb");
    if (!f) {
        snprintf(out, out_size, "ERROR: cannot open '%s' (%s)", path, strerror(errno));
        return -1;
    }
    size_t n = fread(out, 1, out_size - 1, f);
    int truncated = !feof(f);
    fclose(f);
    out[n] = '\0';
    if (truncated && out_size > 64) {
        const char *suffix = "\n... [truncated, file too large] ...";
        size_t slen = strlen(suffix);
        if (n + slen < out_size) {
            memcpy(out + n, suffix, slen);
            out[n + slen] = '\0';
        }
    }
    return 0;
}

int agent_exec_list_dir(const char *workspace, const char *path,
                        char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;
    char real[PATH_MAX];
    if (agent_resolve_path(workspace, path, real, sizeof(real)) != 0) {
        snprintf(out, out_size, "ERROR: path '%s' is outside workspace", path);
        return -1;
    }
    out[0] = '\0';
    size_t off = 0;
#ifdef _WIN32
    char pat[PATH_MAX];
    snprintf(pat, sizeof(pat), "%s\\*", real);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(out, out_size, "ERROR: cannot list '%s'", path);
        return -1;
    }
    do {
        if (fd.cFileName[0] == '.' && (fd.cFileName[1] == '\0' ||
            (fd.cFileName[1] == '.' && fd.cFileName[2] == '\0'))) continue;
        const char *suffix = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "/" : "";
        int wrote = snprintf(out + off, out_size - off, "%s%s\n",
                             fd.cFileName, suffix);
        if (wrote < 0 || (size_t)wrote >= out_size - off) break;
        off += (size_t)wrote;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(real);
    if (!d) {
        snprintf(out, out_size, "ERROR: cannot list '%s' (%s)", path, strerror(errno));
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' ||
            (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) continue;
        struct stat st;
        /* +258 = max d_name (255) + '/' + '\0' to placate gcc's
         * format-truncation warning when real itself is PATH_MAX-sized. */
        char full[PATH_MAX + 258];
        snprintf(full, sizeof(full), "%s/%s", real, ent->d_name);
        const char *suffix = "";
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) suffix = "/";
        int wrote = snprintf(out + off, out_size - off, "%s%s\n",
                             ent->d_name, suffix);
        if (wrote < 0 || (size_t)wrote >= out_size - off) break;
        off += (size_t)wrote;
    }
    closedir(d);
#endif
    if (off == 0) snprintf(out, out_size, "(empty directory)\n");
    return 0;
}

int agent_exec_write_file(const char *workspace, const char *path,
                          const char *content,
                          char *err_out, size_t err_size)
{
    char real[PATH_MAX];
    if (agent_resolve_path(workspace, path, real, sizeof(real)) != 0) {
        if (err_out) snprintf(err_out, err_size,
                              "path '%s' is outside workspace", path);
        return -1;
    }
    FILE *f = fopen(real, "wb");
    if (!f) {
        if (err_out) snprintf(err_out, err_size,
                              "cannot open '%s' for writing (%s)",
                              path, strerror(errno));
        return -1;
    }
    size_t clen = content ? strlen(content) : 0;
    size_t w    = fwrite(content, 1, clen, f);
    fclose(f);
    if (w != clen) {
        if (err_out) snprintf(err_out, err_size,
                              "short write to '%s' (%zu/%zu)", path, w, clen);
        return -1;
    }
    return 0;
}

int agent_exec_apply_edit(const char *workspace, const char *path,
                          const char *search, const char *replace,
                          char *err_out, size_t err_size)
{
    if (!search || !*search) {
        if (err_out) snprintf(err_out, err_size, "empty SEARCH block");
        return -1;
    }
    char real[PATH_MAX];
    if (agent_resolve_path(workspace, path, real, sizeof(real)) != 0) {
        if (err_out) snprintf(err_out, err_size,
                              "path '%s' is outside workspace", path);
        return -1;
    }

    FILE *f = fopen(real, "rb");
    if (!f) {
        if (err_out) snprintf(err_out, err_size,
                              "cannot open '%s' (%s)", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    /* Find the search target; refuse if it appears 0 or 2+ times so the
     * edit is unambiguous (Aider convention). */
    char *first  = strstr(buf, search);
    if (!first) {
        free(buf);
        if (err_out) snprintf(err_out, err_size,
                              "SEARCH block not found in '%s'", path);
        return -1;
    }
    char *second = strstr(first + 1, search);
    if (second) {
        free(buf);
        if (err_out) snprintf(err_out, err_size,
                              "SEARCH block matches multiple times in '%s'; "
                              "narrow it down with more context", path);
        return -1;
    }

    size_t slen = strlen(search);
    size_t rlen = replace ? strlen(replace) : 0;
    size_t prefix_len = (size_t)(first - buf);
    size_t suffix_len = rd - prefix_len - slen;
    size_t new_len    = prefix_len + rlen + suffix_len;

    char *out_buf = (char *)malloc(new_len + 1);
    if (!out_buf) { free(buf); return -1; }
    memcpy(out_buf,                              buf,                      prefix_len);
    if (replace) memcpy(out_buf + prefix_len,    replace,                  rlen);
    memcpy(out_buf + prefix_len + rlen,          buf + prefix_len + slen,  suffix_len);
    out_buf[new_len] = '\0';
    free(buf);

    f = fopen(real, "wb");
    if (!f) {
        free(out_buf);
        if (err_out) snprintf(err_out, err_size,
                              "cannot reopen '%s' for write (%s)",
                              path, strerror(errno));
        return -1;
    }
    size_t w = fwrite(out_buf, 1, new_len, f);
    fclose(f);
    free(out_buf);
    if (w != new_len) {
        if (err_out) snprintf(err_out, err_size,
                              "short write applying edit to '%s'", path);
        return -1;
    }
    return 0;
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

/* ---------------------------------------------------------------------------
 * memmem fallback for platforms that don't expose it (MSVC).
 * --------------------------------------------------------------------------- */
#if defined(_WIN32) && !defined(__MINGW32__)
void *memmem(const void *haystack, size_t hlen,
             const void *needle,   size_t nlen);

void *memmem(const void *haystack, size_t hlen,
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
#endif
