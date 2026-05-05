/* ============================================================================
 * platform_sandbox.c — Cross-platform OS isolation abstraction
 * ============================================================================
 *
 * Linux: seccomp-bpf network lockdown + Landlock FS restriction (best-effort).
 * macOS / Windows: honest stubs — process isolation only.
 * ============================================================================ */

#include "platform_sandbox.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Linux backend
 * --------------------------------------------------------------------------- */
#ifdef __linux__

#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#  ifdef WASTELAND_HAS_SECCOMP
#    include <seccomp.h>
#    include <sys/socket.h>
#  endif

/* Landlock headers may be missing on old build hosts; guard with the raw
 * syscall numbers so the code compiles everywhere but degrades gracefully. */
#  ifdef __NR_landlock_create_ruleset
#    define WASTELAND_HAS_LANDLOCK 1
#    include <linux/landlock.h>
#  endif

#  ifdef WASTELAND_HAS_SECCOMP
static int install_seccomp_network_lockdown(void)
{
    scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_ALLOW);
    if (ctx == NULL) {
        perror("[seccomp] seccomp_init");
        return -1;
    }
    int rc = 0;
    rc |= seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 1,
                           SCMP_A0(SCMP_CMP_EQ, AF_INET));
    rc |= seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 1,
                           SCMP_A0(SCMP_CMP_EQ, AF_INET6));
    rc |= seccomp_rule_add(ctx, SCMP_ACT_KILL, SCMP_SYS(socket), 1,
                           SCMP_A0(SCMP_CMP_EQ, AF_PACKET));
    if (rc != 0) {
        fprintf(stderr, "[seccomp] Failed to add kill rules\n");
        seccomp_release(ctx);
        return -1;
    }
    rc = seccomp_load(ctx);
    seccomp_release(ctx);
    if (rc != 0) {
        perror("[seccomp] seccomp_load");
        return -1;
    }
    fprintf(stderr, "[seccomp] Network lockdown active.\n");
    return 0;
}
#  else
static int install_seccomp_network_lockdown(void)
{
    fprintf(stderr, "[seccomp] Not compiled in.\n");
    return -1;
}
#  endif /* WASTELAND_HAS_SECCOMP */

#  ifdef WASTELAND_HAS_LANDLOCK
static int install_landlock(const char *workspace)
{
    if (!workspace || !workspace[0]) return -1;

    /* Hand-pick the rights the agent executor legitimately needs. */
    __u64 access_fs =
        LANDLOCK_ACCESS_FS_READ_FILE  |
        LANDLOCK_ACCESS_FS_READ_DIR   |
        LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_TRUNCATE   |
        LANDLOCK_ACCESS_FS_MAKE_REG   |
        LANDLOCK_ACCESS_FS_MAKE_DIR   |
        LANDLOCK_ACCESS_FS_REMOVE_FILE|
        LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REFER;

    struct landlock_ruleset_attr ruleset_attr = {
        .handled_access_fs = access_fs,
    };
    int ruleset = (int)syscall(__NR_landlock_create_ruleset,
                               &ruleset_attr, sizeof(ruleset_attr), 0);
    if (ruleset < 0) {
        if (errno == ENOSYS)
            fprintf(stderr, "[landlock] Kernel does not support Landlock.\n");
        else
            perror("[landlock] landlock_create_ruleset");
        return -1;
    }

    int fd = open(workspace, O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        perror("[landlock] open workspace");
        close(ruleset);
        return -1;
    }

    struct landlock_path_beneath_attr beneath = {
        .allowed_access = access_fs,
        .parent_fd = fd,
    };
    int rc = (int)syscall(__NR_landlock_add_rule, ruleset,
                          LANDLOCK_RULE_PATH_BENEATH, &beneath, 0);
    close(fd);
    if (rc != 0) {
        perror("[landlock] landlock_add_rule");
        close(ruleset);
        return -1;
    }

    rc = (int)syscall(__NR_landlock_restrict_self, ruleset, 0);
    close(ruleset);
    if (rc != 0) {
        perror("[landlock] landlock_restrict_self");
        return -1;
    }

    fprintf(stderr, "[landlock] FS restricted to: %s\n", workspace);
    return 0;
}
#  else
static int install_landlock(const char *workspace)
{
    (void)workspace;
    fprintf(stderr, "[landlock] Not available on this build.\n");
    return -1;
}
#  endif /* WASTELAND_HAS_LANDLOCK */

int platform_sandbox_query_caps(void)
{
    int caps = SANDBOX_CAP_PROCESS_ISOLATE;
#  ifdef WASTELAND_HAS_SECCOMP
    caps |= SANDBOX_CAP_NETWORK_LOCKDOWN;
#  endif
#  ifdef WASTELAND_HAS_LANDLOCK
    /* Runtime probe: try to create a dummy ruleset. */
    struct landlock_ruleset_attr attr = { .handled_access_fs = 0 };
    int fd = (int)syscall(__NR_landlock_create_ruleset,
                          &attr, sizeof(attr), 0);
    if (fd >= 0) {
        close(fd);
        caps |= SANDBOX_CAP_FS_RESTRICT;
    }
#  endif
    return caps;
}

int platform_sandbox_apply(int caps)
{
    int rc = 0;
    if (caps & SANDBOX_CAP_NETWORK_LOCKDOWN) {
        if (install_seccomp_network_lockdown() != 0) rc = -1;
    }
    return rc;
}

int platform_sandbox_restrict_fs_to(const char *workspace)
{
    return install_landlock(workspace);
}

/* ---------------------------------------------------------------------------
 * macOS / Windows stubs
 * --------------------------------------------------------------------------- */
#else

int platform_sandbox_query_caps(void)
{
    return SANDBOX_CAP_PROCESS_ISOLATE;
}

int platform_sandbox_apply(int caps)
{
    (void)caps;
    fprintf(stderr, "[sandbox] No native sandbox on this platform.\n");
    return 0; /* degrade gracefully */
}

int platform_sandbox_restrict_fs_to(const char *workspace)
{
    (void)workspace;
    return -1;
}

#endif /* __linux__ */

/* ---------------------------------------------------------------------------
 * Common helpers
 * --------------------------------------------------------------------------- */
void platform_sandbox_describe(int caps, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    if (caps & SANDBOX_CAP_FS_RESTRICT) {
        snprintf(out, out_size, "Full Sandbox — Network & FS restricted");
    } else if (caps & SANDBOX_CAP_NETWORK_LOCKDOWN) {
        snprintf(out, out_size, "Partial Sandbox — Network locked, FS open");
    } else if (caps & SANDBOX_CAP_PROCESS_ISOLATE) {
        snprintf(out, out_size, "Partial Sandbox — Process isolated, FS unconfined");
    } else {
        snprintf(out, out_size, "No Sandbox — No restrictions active");
    }
}

const char *platform_sandbox_status_colour(int caps)
{
    if (caps & SANDBOX_CAP_FS_RESTRICT) return "green";
    if (caps & SANDBOX_CAP_NETWORK_LOCKDOWN) return "amber";
    if (caps & SANDBOX_CAP_PROCESS_ISOLATE) return "amber";
    return "red";
}
