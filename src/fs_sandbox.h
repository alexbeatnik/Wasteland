#ifndef WASTELAND_FS_SANDBOX_H
#define WASTELAND_FS_SANDBOX_H

/* ---------------------------------------------------------------------------
 * fs_sandbox.h — Bulletproof filesystem access for Agent Mode.
 *
 * All agent tool I/O (read_file, list_dir, write_file, apply_edit) must go
 * through this module.  On Linux it uses openat() with O_NOFOLLOW and
 * /proc/self/fd verification; on macOS/Windows it falls back to realpath()
 * + path_is_inside() with honest UI degradation.
 * --------------------------------------------------------------------------- */

#include <stddef.h>

/**
 * @brief Set (or change) the workspace root directory.
 *
 * All subsequent relative paths are resolved beneath this directory.
 * @return 0 on success, -1 on error.
 */
int fs_sandbox_set_workspace(const char *workspace);

/**
 * @brief Read a file into `out`.
 *
 * @param rel_path Path relative to the workspace.
 * @param out      Output buffer.
 * @param out_size Size of output buffer.
 * @return 0 on success, -1 on error (message written to `out`).
 */
int fs_sandbox_read_file(const char *rel_path,
                         char *out, size_t out_size);

/**
 * @brief List directory contents into `out`.
 *
 * Each entry is one line; directories have a trailing '/'.
 * @return 0 on success, -1 on error (message written to `out`).
 */
int fs_sandbox_list_dir(const char *rel_path,
                        char *out, size_t out_size);

/**
 * @brief Overwrite a file with `content`.
 *
 * @return 0 on success, -1 on error (message written to `err_out`).
 */
int fs_sandbox_write_file(const char *rel_path, const char *content,
                          char *err_out, size_t err_size);

/**
 * @brief Apply a single SEARCH/REPLACE edit.
 *
 * The SEARCH block must match exactly once or the edit is rejected.
 * @return 0 on success, -1 on error (message written to `err_out`).
 */
int fs_sandbox_apply_edit(const char *rel_path,
                          const char *search, const char *replace,
                          char *err_out, size_t err_size);

#endif /* WASTELAND_FS_SANDBOX_H */
