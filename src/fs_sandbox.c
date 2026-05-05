/* ============================================================================
 * fs_sandbox.c — Bulletproof filesystem access for the Agent Mode.
 * ============================================================================ */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "fs_sandbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>

#ifndef PATH_MAX
#  define PATH_MAX 4096
#endif

/* ---------------------------------------------------------------------------
 * Platform-specific includes
 * --------------------------------------------------------------------------- */
#ifdef __linux__
#  include <fcntl.h>
#  include <unistd.h>
#  include <dirent.h>
#else
#  ifdef _WIN32
#    include <windows.h>
#    include <direct.h>
#    define PATH_SEP '\\'
#    ifndef S_ISDIR
#      define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#    endif
#  else
#    include <unistd.h>
#    include <dirent.h>
#    define PATH_SEP '/'
#  endif
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * --------------------------------------------------------------------------- */
static char g_workspace[PATH_MAX];
static int  g_workspace_fd = -1;

/* ---------------------------------------------------------------------------
 * Path helpers (common)
 * --------------------------------------------------------------------------- */
static void expand_home(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) {
        if (out && out_size) out[0] = '\0';
        return;
    }
    const char *home = getenv("HOME");
#  ifdef _WIN32
    if (!home || !*home) home = getenv("USERPROFILE");
#  endif
    if (home && *home) {
        if (in[0] == '~' && (in[1] == '/' || in[1] == '\\' || in[1] == '\0')) {
            snprintf(out, out_size, "%s%s", home, in + 1);
            return;
        }
        if (strncmp(in, "$HOME", 5) == 0 &&
            (in[5] == '/' || in[5] == '\\' || in[5] == '\0')) {
            snprintf(out, out_size, "%s%s", home, in + 5);
            return;
        }
    }
    snprintf(out, out_size, "%s", in);
}

static char *xrealpath(const char *p)
{
#  ifdef _WIN32
    char *buf = (char *)malloc(PATH_MAX);
    if (!buf) return NULL;
    DWORD n = GetFullPathNameA(p, PATH_MAX, buf, NULL);
    if (n == 0 || n >= PATH_MAX) { free(buf); return NULL; }
    DWORD attrs = GetFileAttributesA(buf);
    if (attrs == INVALID_FILE_ATTRIBUTES) { free(buf); return NULL; }
    return buf;
#  else
    return realpath(p, NULL);
#  endif
}

static int path_is_inside(const char *child, const char *parent)
{
    size_t plen = strlen(parent);
    if (strncmp(child, parent, plen) != 0) return 0;
    return child[plen] == '\0' || child[plen] == '/' || child[plen] == '\\';
}

/* ---------------------------------------------------------------------------
 * Linux openat() backend
 * --------------------------------------------------------------------------- */
#  ifdef __linux__

static int openat_beneath(int dirfd, const char *name, int flags, mode_t mode)
{
    int fd = openat(dirfd, name, flags | O_NOFOLLOW | O_CLOEXEC, mode);
    return fd;
}

/* Reject absolute paths and any component that is "..".  We also reject
 * embedded ".." because openat-beneath already forbids them, but catching
 * them early lets us emit a clear "outside workspace" error message. */
static int path_is_suspicious(const char *rel_path)
{
    if (!rel_path || !rel_path[0]) return 1;
    if (rel_path[0] == '/' || rel_path[0] == '\\') return 1;
#ifdef _WIN32
    if (strlen(rel_path) >= 2 && rel_path[1] == ':') return 1;
#endif
    const char *p = rel_path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\\' || p[2] == '\0'))
            return 1;
        while (*p && *p != '/' && *p != '\\') p++;
        if (*p) p++;
    }
    return 0;
}

static int resolve_and_open(const char *rel_path, int flags, mode_t mode,
                            char *out_real, size_t real_size)
{
    if (g_workspace_fd < 0 || !rel_path || !rel_path[0]) {
        errno = ENOENT;
        return -1;
    }

    /* Copy and tokenise. */
    char copy[PATH_MAX];
    if (strlen(rel_path) >= sizeof(copy)) { errno = ENAMETOOLONG; return -1; }
    snprintf(copy, sizeof(copy), "%s", rel_path);

    int cur_fd = g_workspace_fd;
    int prev_fd = -1;

    char *saveptr = NULL;
    char *token = strtok_r(copy, "/", &saveptr);
    char *next;

    while (token) {
        next = strtok_r(NULL, "/", &saveptr);

        if (strcmp(token, ".") == 0) {
            token = next;
            continue;
        }
        if (strcmp(token, "..") == 0) {
            /* Explicit ".." is forbidden even if it stays inside the
             * workspace — we never backtrack via path components. */
            errno = EACCES;
            goto fail;
        }

        if (next) {
            /* Intermediate directory. */
            int fd = openat_beneath(cur_fd, token, O_PATH | O_DIRECTORY, 0);
            if (fd < 0) goto fail;
            if (prev_fd >= 0) close(prev_fd);
            prev_fd = cur_fd == g_workspace_fd ? -1 : cur_fd;
            cur_fd = fd;
        } else {
            /* Final component. */
            int fd = openat_beneath(cur_fd, token, flags, mode);
            if (fd < 0) goto fail;
            if (cur_fd != g_workspace_fd) close(cur_fd);

            /* Verify via /proc/self/fd that we are still under workspace. */
            char proc_path[64];
            snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
            char resolved[PATH_MAX];
            ssize_t r = readlink(proc_path, resolved, sizeof(resolved) - 1);
            if (r < 0) { close(fd); errno = EACCES; return -1; }
            resolved[r] = '\0';
            if (!path_is_inside(resolved, g_workspace)) {
                close(fd);
                errno = EACCES;
                return -1;
            }
            if (out_real && real_size > 0)
                snprintf(out_real, real_size, "%s", resolved);
            return fd;
        }
        token = next;
    }

    /* Empty relative path — open the workspace itself. */
    if (out_real && real_size > 0)
        snprintf(out_real, real_size, "%s", g_workspace);
    return dup(g_workspace_fd);

fail:
    if (cur_fd != g_workspace_fd && cur_fd >= 0) close(cur_fd);
    return -1;
}

#  endif /* __linux__ */

/* ---------------------------------------------------------------------------
 * Fallback backend (macOS / Windows) — realpath() + path_is_inside()
 * --------------------------------------------------------------------------- */
#  ifndef __linux__

static int fallback_resolve(const char *workspace, const char *rel_path,
                            char *out, size_t out_size)
{
    if (!workspace || !*workspace || !rel_path || !*rel_path ||
        !out || out_size < 2)
        return -1;

    char ws_expanded[PATH_MAX];
    expand_home(workspace, ws_expanded, sizeof(ws_expanded));
    char *ws = xrealpath(ws_expanded);
    if (!ws) return -1;

    char joined[PATH_MAX];
    int is_abs = (rel_path[0] == '/' || rel_path[0] == '\\');
#    ifdef _WIN32
    if (!is_abs && strlen(rel_path) >= 2 && rel_path[1] == ':') is_abs = 1;
#    endif
    if (is_abs) {
        snprintf(joined, sizeof(joined), "%s", rel_path);
    } else {
        snprintf(joined, sizeof(joined), "%s%c%s", ws, PATH_SEP, rel_path);
    }

    char target[PATH_MAX];
    char *resolved = xrealpath(joined);
    if (resolved) {
        snprintf(target, sizeof(target), "%s", resolved);
        free(resolved);
    } else {
        char dirbuf[PATH_MAX];
        snprintf(dirbuf, sizeof(dirbuf), "%s", joined);
        char *last_sep = strrchr(dirbuf, '/');
#    ifdef _WIN32
        char *bs = strrchr(dirbuf, '\\');
        if (bs && (!last_sep || bs > last_sep)) last_sep = bs;
#    endif
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

#  endif /* !__linux__ */

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */
int fs_sandbox_set_workspace(const char *workspace)
{
    if (!workspace || !workspace[0]) return -1;

    char expanded[PATH_MAX];
    expand_home(workspace, expanded, sizeof(expanded));
    char *rp = xrealpath(expanded);
    if (!rp) return -1;

    if (strlen(rp) >= sizeof(g_workspace)) {
        free(rp);
        return -1;
    }
    snprintf(g_workspace, sizeof(g_workspace), "%s", rp);
    free(rp);

#  ifdef __linux__
    if (g_workspace_fd >= 0) close(g_workspace_fd);
    g_workspace_fd = open(g_workspace, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (g_workspace_fd < 0) {
        fprintf(stderr, "[fs_sandbox] Cannot open workspace: %s\n", g_workspace);
        return -1;
    }
#  endif

    return 0;
}

int fs_sandbox_read_file(const char *rel_path,
                         char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    if (path_is_suspicious(rel_path)) {
        snprintf(out, out_size, "ERROR: path '%s' is outside workspace", rel_path);
        return -1;
    }

#  ifdef __linux__
    int fd = resolve_and_open(rel_path, O_RDONLY, 0, NULL, 0);
    if (fd < 0) {
        snprintf(out, out_size, "ERROR: cannot open '%s' (%s)",
                 rel_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        snprintf(out, out_size, "ERROR: '%s' is not a regular file", rel_path);
        return -1;
    }
    size_t n = 0;
    ssize_t r;
    while (n < out_size - 1 && (r = read(fd, out + n, out_size - 1 - n)) > 0)
        n += (size_t)r;
    close(fd);
    out[n] = '\0';
    if ((size_t)n >= out_size - 1) {
        const char *suffix = "\n... [truncated, file too large] ...";
        size_t slen = strlen(suffix);
        if (n + slen < out_size) {
            memcpy(out + n, suffix, slen);
            out[n + slen] = '\0';
        }
    }
    return 0;
#  else
    char real[PATH_MAX];
    if (fallback_resolve(g_workspace, rel_path, real, sizeof(real)) != 0) {
        snprintf(out, out_size, "ERROR: path '%s' is outside workspace", rel_path);
        return -1;
    }
    FILE *f = fopen(real, "rb");
    if (!f) {
        snprintf(out, out_size, "ERROR: cannot open '%s' (%s)",
                 rel_path, strerror(errno));
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
#  endif
}

int fs_sandbox_list_dir(const char *rel_path,
                        char *out, size_t out_size)
{
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    if (path_is_suspicious(rel_path)) {
        snprintf(out, out_size, "ERROR: path '%s' is outside workspace", rel_path);
        return -1;
    }

#  ifdef __linux__
    char real_path[PATH_MAX];
    int fd = resolve_and_open(rel_path, O_RDONLY | O_DIRECTORY, 0,
                              real_path, sizeof(real_path));
    if (fd < 0) {
        snprintf(out, out_size, "ERROR: cannot list '%s' (%s)",
                 rel_path, strerror(errno));
        return -1;
    }
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        snprintf(out, out_size, "ERROR: cannot list '%s' (%s)",
                 rel_path, strerror(errno));
        return -1;
    }
    size_t off = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        struct stat st;
        int sub = openat(fd, ent->d_name, O_PATH | O_NOFOLLOW | O_CLOEXEC);
        const char *suffix = "";
        if (sub >= 0) {
            if (fstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) suffix = "/";
            close(sub);
        }
        int wrote = snprintf(out + off, out_size - off, "%s%s\n",
                             ent->d_name, suffix);
        if (wrote < 0 || (size_t)wrote >= out_size - off) break;
        off += (size_t)wrote;
    }
    closedir(d); /* also closes underlying fd */
    if (off == 0) snprintf(out, out_size, "(empty directory)\n");
    return 0;
#  else
    char real[PATH_MAX];
    if (fallback_resolve(g_workspace, rel_path, real, sizeof(real)) != 0) {
        snprintf(out, out_size, "ERROR: path '%s' is outside workspace", rel_path);
        return -1;
    }
    /* ... existing Windows / macOS opendir logic, copied from agent.c ... */
    size_t off = 0;
#    ifdef _WIN32
    char pat[PATH_MAX];
    snprintf(pat, sizeof(pat), "%s\\*", real);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(out, out_size, "ERROR: cannot list '%s'", rel_path);
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
#    else
    DIR *d = opendir(real);
    if (!d) {
        snprintf(out, out_size, "ERROR: cannot list '%s' (%s)",
                 rel_path, strerror(errno));
        return -1;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && (ent->d_name[1] == '\0' ||
            (ent->d_name[1] == '.' && ent->d_name[2] == '\0'))) continue;
        struct stat st;
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
#    endif
    if (off == 0) snprintf(out, out_size, "(empty directory)\n");
    return 0;
#  endif
}

int fs_sandbox_write_file(const char *rel_path, const char *content,
                          char *err_out, size_t err_size)
{
    if (path_is_suspicious(rel_path)) {
        if (err_out) snprintf(err_out, err_size,
                              "path '%s' is outside workspace", rel_path);
        return -1;
    }
#  ifdef __linux__
    int fd = resolve_and_open(rel_path, O_WRONLY | O_CREAT | O_TRUNC, 0644,
                              NULL, 0);
    if (fd < 0) {
        if (err_out) snprintf(err_out, err_size,
                              "cannot open '%s' for writing (%s)",
                              rel_path, strerror(errno));
        return -1;
    }
    size_t clen = content ? strlen(content) : 0;
    ssize_t w = 0;
    size_t written = 0;
    while (written < clen) {
        w = write(fd, content + written, clen - written);
        if (w < 0) {
            close(fd);
            if (err_out) snprintf(err_out, err_size,
                                  "short write to '%s'", rel_path);
            return -1;
        }
        written += (size_t)w;
    }
    close(fd);
    return 0;
#  else
    char real[PATH_MAX];
    if (fallback_resolve(g_workspace, rel_path, real, sizeof(real)) != 0) {
        if (err_out) snprintf(err_out, err_size,
                              "path '%s' is outside workspace", rel_path);
        return -1;
    }
    FILE *f = fopen(real, "wb");
    if (!f) {
        if (err_out) snprintf(err_out, err_size,
                              "cannot open '%s' for writing (%s)",
                              rel_path, strerror(errno));
        return -1;
    }
    size_t clen = content ? strlen(content) : 0;
    size_t w = fwrite(content, 1, clen, f);
    fclose(f);
    if (w != clen) {
        if (err_out) snprintf(err_out, err_size,
                              "short write to '%s' (%zu/%zu)", rel_path, w, clen);
        return -1;
    }
    return 0;
#  endif
}

int fs_sandbox_apply_edit(const char *rel_path,
                          const char *search, const char *replace,
                          char *err_out, size_t err_size)
{
    if (!search || !*search) {
        if (err_out) snprintf(err_out, err_size, "empty SEARCH block");
        return -1;
    }
    if (path_is_suspicious(rel_path)) {
        if (err_out) snprintf(err_out, err_size,
                              "path '%s' is outside workspace", rel_path);
        return -1;
    }

#  ifdef __linux__
    int fd = resolve_and_open(rel_path, O_RDONLY, 0, NULL, 0);
    if (fd < 0) {
        if (err_out) snprintf(err_out, err_size,
                              "cannot open '%s' (%s)", rel_path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        if (err_out) snprintf(err_out, err_size,
                              "'%s' is not a regular file", rel_path);
        return -1;
    }
    off_t sz = st.st_size;
    if (sz < 0) { close(fd); return -1; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { close(fd); return -1; }
    size_t rd = 0;
    ssize_t r;
    while (rd < (size_t)sz && (r = read(fd, buf + rd, (size_t)sz - rd)) > 0)
        rd += (size_t)r;
    close(fd);
    buf[rd] = '\0';
#  else
    char real[PATH_MAX];
    if (fallback_resolve(g_workspace, rel_path, real, sizeof(real)) != 0) {
        if (err_out) snprintf(err_out, err_size,
                              "path '%s' is outside workspace", rel_path);
        return -1;
    }
    FILE *f = fopen(real, "rb");
    if (!f) {
        if (err_out) snprintf(err_out, err_size,
                              "cannot open '%s' (%s)", rel_path, strerror(errno));
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
#  endif

    /* Find the search target; refuse if ambiguous (0 or 2+ matches). */
    char *first = strstr(buf, search);
    if (!first) {
        free(buf);
        if (err_out) snprintf(err_out, err_size,
                              "SEARCH block not found in '%s'", rel_path);
        return -1;
    }
    char *second = strstr(first + 1, search);
    if (second) {
        free(buf);
        if (err_out) snprintf(err_out, err_size,
                              "SEARCH block matches multiple times in '%s'; "
                              "narrow it down with more context", rel_path);
        return -1;
    }

    size_t slen = strlen(search);
    size_t rlen = replace ? strlen(replace) : 0;
    size_t prefix_len = (size_t)(first - buf);
    size_t suffix_len = rd - prefix_len - slen;
    size_t new_len = prefix_len + rlen + suffix_len;

    char *out_buf = (char *)malloc(new_len + 1);
    if (!out_buf) { free(buf); return -1; }
    memcpy(out_buf,                           buf,                     prefix_len);
    if (replace) memcpy(out_buf + prefix_len, replace,                 rlen);
    memcpy(out_buf + prefix_len + rlen,       buf + prefix_len + slen, suffix_len);
    out_buf[new_len] = '\0';
    free(buf);

#  ifdef __linux__
    int fdw = resolve_and_open(rel_path, O_WRONLY | O_TRUNC, 0, NULL, 0);
    if (fdw < 0) {
        free(out_buf);
        if (err_out) snprintf(err_out, err_size,
                              "cannot reopen '%s' for write (%s)",
                              rel_path, strerror(errno));
        return -1;
    }
    size_t written = 0;
    ssize_t w;
    while (written < new_len) {
        w = write(fdw, out_buf + written, new_len - written);
        if (w < 0) break;
        written += (size_t)w;
    }
    close(fdw);
    free(out_buf);
    if (written != new_len) {
        if (err_out) snprintf(err_out, err_size,
                              "short write applying edit to '%s'", rel_path);
        return -1;
    }
#  else
    FILE *fw = fopen(real, "wb");
    if (!fw) {
        free(out_buf);
        if (err_out) snprintf(err_out, err_size,
                              "cannot reopen '%s' for write (%s)",
                              rel_path, strerror(errno));
        return -1;
    }
    size_t w = fwrite(out_buf, 1, new_len, fw);
    fclose(fw);
    free(out_buf);
    if (w != new_len) {
        if (err_out) snprintf(err_out, err_size,
                              "short write applying edit to '%s'", rel_path);
        return -1;
    }
#  endif
    return 0;
}
