#ifndef WASTELAND_VERIFY_H
#define WASTELAND_VERIFY_H

/* ---------------------------------------------------------------------------
 * verify.h — Async SHA-256 model checksum validation.
 *
 * The compute function is blocking and intended to be run from a detached
 * pthread.  Progress is reported via a volatile int (0-100).
 * --------------------------------------------------------------------------- */

#include <stddef.h>

/**
 * @brief Incrementally compute SHA-256 of `file_path` and hex-encode it.
 *
 * @param file_path  Path to the file to hash.
 * @param hex_out    Output buffer (must be at least 65 bytes).
 * @param hex_size   Size of hex_out.
 * @param progress   Optional; if non-NULL, written with 0..100 as file is read.
 * @return 0 on success, -1 on error.
 */
int verify_compute_sha256(const char *file_path,
                          char *hex_out, size_t hex_size,
                          volatile int *progress);

/**
 * @brief Convenience: compute hash and compare against an expected hex string.
 *
 * @param file_path    Path to the file.
 * @param expected_hex Expected SHA-256 in lowercase hex (64 chars).
 * @param progress     Optional progress pointer.
 * @return 0 if hash matches, -1 on mismatch or I/O error.
 */
int verify_check_file(const char *file_path,
                      const char *expected_hex,
                      volatile int *progress);

#endif /* WASTELAND_VERIFY_H */
