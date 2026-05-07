#ifndef WASTELAND_CRYPTO_ENGINE_H
#define WASTELAND_CRYPTO_ENGINE_H

/* ---------------------------------------------------------------------------
 * Application-facing authenticated-encryption API.
 *
 * Backed by Monocypher (XChaCha20-Poly1305).  Replaces the previous RC4
 * obfuscation with real authenticated encryption.
 *
 * The chat-file format is:
 *   "WST2"  (4 bytes magic)
 *   nonce   (24 bytes)
 *   tag     (16 bytes)
 *   ciphertext (same length as plaintext)
 *
 * Key is a compile-time constant pepper; no user passphrase required for the
 * default threat model (casual file-manager snooping).  If a user-supplied
 * password is added later, derive the key with Argon2id via
 * crypto_derive_key_from_password().
 * --------------------------------------------------------------------------- */

#include <stddef.h>
#include <stdint.h>

#define WASTELAND_CRYPTO_KEY_SIZE   32
#define WASTELAND_CRYPTO_NONCE_SIZE 24
#define WASTELAND_CRYPTO_TAG_SIZE   16
#define WASTELAND_CRYPTO_MAGIC      "WST2"

/**
 * @brief Encrypt `plain` into `cipher` + `tag` using XChaCha20-Poly1305.
 *
 * @param key       32-byte secret key.
 * @param nonce     24-byte unique nonce (must not repeat for same key).
 * @param plain     Plaintext buffer.
 * @param plain_len Length of plaintext.
 * @param cipher    Output ciphertext buffer (plain_len bytes).
 * @param tag       Output 16-byte authentication tag.
 * @return 0 on success, -1 on error.
 */
int crypto_seal(const uint8_t key[WASTELAND_CRYPTO_KEY_SIZE],
                const uint8_t nonce[WASTELAND_CRYPTO_NONCE_SIZE],
                const uint8_t *plain, size_t plain_len,
                uint8_t *cipher,
                uint8_t tag[WASTELAND_CRYPTO_TAG_SIZE]);

/**
 * @brief Decrypt and verify `cipher` using XChaCha20-Poly1305.
 *
 * @param key       32-byte secret key.
 * @param nonce     24-byte nonce.
 * @param cipher    Ciphertext buffer.
 * @param cipher_len Length of ciphertext.
 * @param tag       16-byte authentication tag.
 * @param plain     Output plaintext buffer (cipher_len bytes).
 * @return 0 on success (tag verified), -1 on authentication failure.
 */
int crypto_open(const uint8_t key[WASTELAND_CRYPTO_KEY_SIZE],
                const uint8_t nonce[WASTELAND_CRYPTO_NONCE_SIZE],
                const uint8_t *cipher, size_t cipher_len,
                const uint8_t tag[WASTELAND_CRYPTO_TAG_SIZE],
                uint8_t *plain);

/**
 * @brief Fill `buf` with `len` cryptographically secure random bytes.
 *
 * Uses getrandom() on Linux, arc4random() on macOS, BCryptGenRandom on
 * Windows, and /dev/urandom as a fallback.
 */
void crypto_random_bytes(uint8_t *buf, size_t len);

#endif /* WASTELAND_CRYPTO_ENGINE_H */
