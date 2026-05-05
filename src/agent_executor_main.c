/* ============================================================================
 * agent_executor_main.c — Stand-alone Agent Executor subprocess
 * ============================================================================
 *
 * This binary is spawned by the main Wasteland process (via posix_spawn or
 * equivalent).  It receives framed IPC requests on stdin and writes responses
 * on stdout.  The process is intended to be locked down with seccomp and/or
 * Landlock before entering the service loop.
 *
 * Usage: wasteland-agent-executor <workspace>
 * ============================================================================ */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "agent_protocol.h"
#include "fs_sandbox.h"
#include "platform_sandbox.h"

static void handle_request(int out_fd, const agent_ipc_req_hdr_t *hdr,
                           const char *path, const uint8_t *data)
{
    uint8_t resp_data[AGENT_IPC_MAX_DATA + 1];
    uint32_t resp_len = 0;
    int32_t status = 0;

    switch (hdr->tool) {
    case AGENT_IPC_TOOL_READ_FILE: {
        int rc = fs_sandbox_read_file(path, (char *)resp_data, sizeof(resp_data));
        if (rc != 0) status = -1;
        resp_len = (uint32_t)strlen((char *)resp_data);
        break;
    }
    case AGENT_IPC_TOOL_LIST_DIR: {
        int rc = fs_sandbox_list_dir(path, (char *)resp_data, sizeof(resp_data));
        if (rc != 0) status = -1;
        resp_len = (uint32_t)strlen((char *)resp_data);
        break;
    }
    case AGENT_IPC_TOOL_WRITE_FILE: {
        char err[512] = "";
        int rc = fs_sandbox_write_file(path, (const char *)data, err, sizeof(err));
        if (rc != 0) {
            status = -1;
            snprintf((char *)resp_data, sizeof(resp_data), "%s", err);
            resp_len = (uint32_t)strlen((char *)resp_data);
        }
        break;
    }
    case AGENT_IPC_TOOL_APPLY_EDIT: {
        /* data format: search\0replace — locate the in-band separator with
         * memchr (bounded by hdr->data_len) so a missing NUL can't walk off
         * the buffer. */
        if (!data || hdr->data_len == 0) {
            status = -1;
            snprintf((char *)resp_data, sizeof(resp_data),
                     "missing apply_edit payload");
            resp_len = (uint32_t)strlen((char *)resp_data);
            break;
        }
        const uint8_t *sep = (const uint8_t *)memchr(data, 0, hdr->data_len);
        if (!sep) {
            status = -1;
            snprintf((char *)resp_data, sizeof(resp_data),
                     "malformed apply_edit request");
            resp_len = (uint32_t)strlen((char *)resp_data);
            break;
        }
        const char *search  = (const char *)data;
        const char *replace = (const char *)(sep + 1);
        char err[512] = "";
        int rc = fs_sandbox_apply_edit(path, search, replace, err, sizeof(err));
        if (rc != 0) {
            status = -1;
            snprintf((char *)resp_data, sizeof(resp_data), "%s", err);
            resp_len = (uint32_t)strlen((char *)resp_data);
        }
        break;
    }
    case AGENT_IPC_TOOL_SHUTDOWN:
        _exit(0);
    default:
        status = -1;
        snprintf((char *)resp_data, sizeof(resp_data),
                 "unknown tool kind %u", hdr->tool);
        resp_len = (uint32_t)strlen((char *)resp_data);
    }

    agent_ipc_resp_hdr_t resp = {
        .status   = status,
        .data_len = resp_len,
    };
    agent_ipc_write_all(out_fd, (const uint8_t *)&resp, sizeof(resp));
    if (resp_len > 0)
        agent_ipc_write_all(out_fd, resp_data, resp_len);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <workspace>\n", argv[0]);
        return 1;
    }

    const char *workspace = argv[1];
    if (fs_sandbox_set_workspace(workspace) != 0) {
        fprintf(stderr, "[executor] Invalid workspace: %s\n", workspace);
        return 1;
    }

    /* Lock down before accepting any requests. */
    platform_sandbox_apply(SANDBOX_CAP_NETWORK_LOCKDOWN);
    platform_sandbox_restrict_fs_to(workspace);

    /* Service loop — read from stdin, write to stdout. */
    while (1) {
        agent_ipc_req_hdr_t hdr;
        if (agent_ipc_read_all(STDIN_FILENO, (uint8_t *)&hdr, sizeof(hdr)) != 0)
            break;

        if (hdr.version != AGENT_IPC_VERSION) {
            fprintf(stderr, "[executor] bad version %u\n", hdr.version);
            break;
        }
        if (hdr.path_len > AGENT_IPC_MAX_PATH) {
            fprintf(stderr, "[executor] path_len too large %u\n", hdr.path_len);
            break;
        }
        if (hdr.data_len > AGENT_IPC_MAX_DATA) {
            fprintf(stderr, "[executor] data_len too large %u\n", hdr.data_len);
            break;
        }

        char *path = (char *)malloc(hdr.path_len + 1);
        /* +1 for trailing NUL — fs_sandbox_write_file and the apply_edit
         * search\0replace decoding both rely on a C-string view of the data
         * payload, but the wire framing carries exactly data_len bytes. */
        uint8_t *data = hdr.data_len > 0 ? (uint8_t *)malloc(hdr.data_len + 1) : NULL;
        if (!path || (hdr.data_len > 0 && !data)) {
            free(path); free(data); break;
        }

        if (agent_ipc_read_all(STDIN_FILENO, (uint8_t *)path, hdr.path_len) != 0) {
            free(path); free(data); break;
        }
        path[hdr.path_len] = '\0';

        if (hdr.data_len > 0 && agent_ipc_read_all(STDIN_FILENO, data, hdr.data_len) != 0) {
            free(path); free(data); break;
        }
        if (data) data[hdr.data_len] = '\0';

        handle_request(STDOUT_FILENO, &hdr, path, data);

        free(path);
        free(data);
    }

    return 0;
}
