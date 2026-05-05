#ifndef WASTELAND_PLATFORM_SANDBOX_H
#define WASTELAND_PLATFORM_SANDBOX_H

/* ---------------------------------------------------------------------------
 * Cross-platform sandbox abstraction.
 *
 * Provides a unified interface for querying and applying OS-level isolation
 * primitives.  Real restrictions are only available on Linux (seccomp +
 * Landlock).  macOS and Windows receive honest degradation (process isolation
 * stubs) and the UI reflects the actual security posture.
 * --------------------------------------------------------------------------- */

#include <stddef.h>

#define SANDBOX_CAP_NONE             0
#define SANDBOX_CAP_NETWORK_LOCKDOWN (1 << 0)
#define SANDBOX_CAP_FS_RESTRICT      (1 << 1)
#define SANDBOX_CAP_PROCESS_ISOLATE  (1 << 2)

/**
 * @brief Query the set of sandbox capabilities available on this OS.
 *
 * @return Bitmask of SANDBOX_CAP_* constants.
 */
int platform_sandbox_query_caps(void);

/**
 * @brief Apply the requested sandbox capabilities to the *current* process.
 *
 * This is intended for the Agent Executor subprocess, which starts with a
 * clean slate and then locks itself down before servicing tool requests.
 *
 * @param caps Bitmask of SANDBOX_CAP_* to enable.
 * @return 0 on success, -1 if a requested cap could not be applied.
 */
int platform_sandbox_apply(int caps);

/**
 * @brief Restrict filesystem access to a single directory tree (Linux Landlock).
 *
 * On Linux 5.13+ this creates a Landlock ruleset that allows read/write
 * access only beneath `workspace`.  On older kernels or other OSs this is a
 * no-op and returns -1.
 *
 * @param workspace Absolute path to the allowed directory.
 * @return 0 on success, -1 if Landlock is unavailable.
 */
int platform_sandbox_restrict_fs_to(const char *workspace);

/**
 * @brief Human-readable description of the sandbox status.
 *
 * Writes a short sentence like "Full Sandbox — Network & FS restricted"
 * into `out` (max `out_size` bytes).
 */
void platform_sandbox_describe(int caps, char *out, size_t out_size);

/**
 * @brief Severity colour for the UI indicator.
 *
 * Returns a static string: "green", "amber", or "red".  The UI maps these
 * to the appropriate Nuklear colour values.
 */
const char *platform_sandbox_status_colour(int caps);

#endif /* WASTELAND_PLATFORM_SANDBOX_H */
