#ifndef WASTELAND_SHA256_H
#define WASTELAND_SHA256_H

/* ---------------------------------------------------------------------------
 * Pure C11 SHA-256 implementation.
 * Zero dependencies, zero dynamic allocation. Single-shot and incremental
 * interfaces for model-checksum verification.
 * --------------------------------------------------------------------------- */

#include <stddef.h>
#include <stdint.h>

#define WASTELAND_SHA256_BLOCK_SIZE 64
#define WASTELAND_SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t  buffer[WASTELAND_SHA256_BLOCK_SIZE];
    size_t   buffer_used;
} wasteland_sha256_ctx_t;

void wasteland_sha256_init(wasteland_sha256_ctx_t *ctx);
void wasteland_sha256_update(wasteland_sha256_ctx_t *ctx,
                             const uint8_t *data, size_t len);
void wasteland_sha256_final(wasteland_sha256_ctx_t *ctx,
                            uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE]);

/* Convenience: hash a complete buffer in one call. */
void wasteland_sha256(const uint8_t *data, size_t len,
                      uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE]);

/* Hex-encode digest into out (needs at least 65 bytes). */
void wasteland_sha256_hex(const uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE],
                          char *out, size_t out_size);

#endif /* WASTELAND_SHA256_H */
