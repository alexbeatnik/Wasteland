/* ============================================================================
 * verify.c — Async SHA-256 model checksum validation
 * ============================================================================ */

#include "verify.h"
#include "vendor/sha256/sha256.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

int verify_compute_sha256(const char *file_path,
                          char *hex_out, size_t hex_size,
                          volatile int *progress)
{
    if (!file_path || !hex_out || hex_size < 65) return -1;
    if (progress) *progress = 0;

    FILE *f = fopen(file_path, "rb");
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long total = ftell(f);
    if (total < 0) { fclose(f); return -1; }
    rewind(f);

    wasteland_sha256_ctx_t ctx;
    wasteland_sha256_init(&ctx);

    uint8_t buf[65536];
    size_t read_total = 0;
    size_t n;
    int last_pct = -1;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        wasteland_sha256_update(&ctx, buf, n);
        read_total += n;
        if (progress && total > 0) {
            int pct = (int)((read_total * 100) / (size_t)total);
            if (pct != last_pct) {
                *progress = pct;
                last_pct = pct;
            }
        }
    }
    fclose(f);

    uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE];
    wasteland_sha256_final(&ctx, digest);
    wasteland_sha256_hex(digest, hex_out, hex_size);
    if (progress) *progress = 100;
    return 0;
}

int verify_check_file(const char *file_path,
                      const char *expected_hex,
                      volatile int *progress)
{
    char actual[65];
    if (verify_compute_sha256(file_path, actual, sizeof(actual), progress) != 0)
        return -1;
    return (strcmp(actual, expected_hex) == 0) ? 0 : -1;
}
