/* ============================================================================
 * agent_protocol.c — Framed IPC serialization helpers
 * ============================================================================ */

#include "agent_protocol.h"
#include <string.h>
#include <stdio.h>

#ifdef __linux__
#  include <unistd.h>
#  include <errno.h>

int agent_ipc_read_all(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

int agent_ipc_write_all(int fd, const uint8_t *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, buf + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

int agent_ipc_send_request(int fd, agent_ipc_tool_t tool,
                           const char *path,
                           const uint8_t *data, uint32_t data_len)
{
    if (!path) return -1;
    size_t path_len = strlen(path);
    if (path_len > AGENT_IPC_MAX_PATH) {
        fprintf(stderr, "[agent_ipc] path too long (%zu)\n", path_len);
        return -1;
    }
    if (data_len > AGENT_IPC_MAX_DATA) {
        fprintf(stderr, "[agent_ipc] data too large (%u)\n", data_len);
        return -1;
    }

    agent_ipc_req_hdr_t hdr = {
        .version  = AGENT_IPC_VERSION,
        .tool     = (uint8_t)tool,
        .path_len = (uint16_t)path_len,
        .data_len = data_len,
    };

    if (agent_ipc_write_all(fd, (const uint8_t *)&hdr, sizeof(hdr)) != 0)
        return -1;
    if (agent_ipc_write_all(fd, (const uint8_t *)path, path_len) != 0)
        return -1;
    if (data_len > 0 && agent_ipc_write_all(fd, data, data_len) != 0)
        return -1;
    return 0;
}

int agent_ipc_recv_response(int fd,
                            int32_t *out_status,
                            uint8_t *out_data, uint32_t *out_len)
{
    agent_ipc_resp_hdr_t hdr;
    if (agent_ipc_read_all(fd, (uint8_t *)&hdr, sizeof(hdr)) != 0)
        return -1;

    if (hdr.data_len > AGENT_IPC_MAX_DATA) {
        fprintf(stderr, "[agent_ipc] response data too large (%u)\n", hdr.data_len);
        return -1;
    }

    if (hdr.data_len > 0) {
        if (agent_ipc_read_all(fd, out_data, hdr.data_len) != 0)
            return -1;
    }
    out_data[hdr.data_len] = '\0';
    *out_status = hdr.status;
    *out_len    = hdr.data_len;
    return 0;
}
#else
/* IPC executor is Linux-only; provide no-op stubs for other platforms. */
int agent_ipc_read_all(int fd, uint8_t *buf, size_t n) { (void)fd; (void)buf; (void)n; return -1; }
int agent_ipc_write_all(int fd, const uint8_t *buf, size_t n) { (void)fd; (void)buf; (void)n; return -1; }
int agent_ipc_send_request(int fd, agent_ipc_tool_t tool,
                           const char *path,
                           const uint8_t *data, uint32_t data_len)
{ (void)fd; (void)tool; (void)path; (void)data; (void)data_len; return -1; }
int agent_ipc_recv_response(int fd,
                            int32_t *out_status,
                            uint8_t *out_data, uint32_t *out_len)
{ (void)fd; (void)out_status; (void)out_data; (void)out_len; return -1; }
#endif
