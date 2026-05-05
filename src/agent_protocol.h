#ifndef WASTELAND_AGENT_PROTOCOL_H
#define WASTELAND_AGENT_PROTOCOL_H

/* ---------------------------------------------------------------------------
 * Framed IPC protocol between the main Agent Controller (inference thread)
 * and the separate Agent Executor subprocess.
 *
 * Stream transport: reliable byte stream (Unix-domain socketpair or pipe).
 * Each message is prefixed by a fixed-size header, followed by payload bytes.
 * --------------------------------------------------------------------------- */

#include <stdint.h>
#include <stddef.h>

#define AGENT_IPC_VERSION 1

/* Max payload sizes — generous but bounded to prevent unbounded malloc. */
#define AGENT_IPC_MAX_PATH   1024
#define AGENT_IPC_MAX_DATA   65536

typedef enum {
    AGENT_IPC_TOOL_READ_FILE = 1,
    AGENT_IPC_TOOL_LIST_DIR  = 2,
    AGENT_IPC_TOOL_WRITE_FILE= 3,
    AGENT_IPC_TOOL_APPLY_EDIT= 4,
    AGENT_IPC_TOOL_SHUTDOWN  = 99
} agent_ipc_tool_t;

typedef struct {
    uint8_t  version;    /* AGENT_IPC_VERSION */
    uint8_t  tool;       /* agent_ipc_tool_t */
    uint16_t path_len;   /* <= AGENT_IPC_MAX_PATH */
    uint32_t data_len;   /* <= AGENT_IPC_MAX_DATA */
} __attribute__((packed)) agent_ipc_req_hdr_t;

typedef struct {
    int32_t  status;     /* 0 = success, negative = errno or custom error */
    uint32_t data_len;   /* <= AGENT_IPC_MAX_DATA */
} __attribute__((packed)) agent_ipc_resp_hdr_t;

/**
 * @brief Send a complete request (header + path + optional data) over `fd`.
 * @return 0 on success, -1 on error (partial write, timeout, etc.).
 */
int agent_ipc_send_request(int fd, agent_ipc_tool_t tool,
                           const char *path,
                           const uint8_t *data, uint32_t data_len);

/**
 * @brief Receive a complete response (header + data) over `fd`.
 * @param out_data  Must be at least AGENT_IPC_MAX_DATA + 1 bytes.
 * @param out_len   Written with actual data length.
 * @return 0 on success, -1 on error.
 */
int agent_ipc_recv_response(int fd,
                            int32_t *out_status,
                            uint8_t *out_data, uint32_t *out_len);

/**
 * @brief Read exactly `n` bytes from `fd` into `buf`.
 * @return 0 on success, -1 on EOF or error.
 */
int agent_ipc_read_all(int fd, uint8_t *buf, size_t n);

/**
 * @brief Write exactly `n` bytes to `fd` from `buf`.
 * @return 0 on success, -1 on error.
 */
int agent_ipc_write_all(int fd, const uint8_t *buf, size_t n);

#endif /* WASTELAND_AGENT_PROTOCOL_H */
