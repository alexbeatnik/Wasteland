/* ============================================================================
 * crypto_engine.c — Application crypto facade over Monocypher.
 * ============================================================================ */

#include "crypto_engine.h"

/* Monocypher declares fixed-size array parameters (e.g. uint8_t key[32]).
 * GCC 12+ -Wstringop-overread warns that the call site might pass a smaller
 * buffer.  Our wrappers guarantee correct sizes via WASTELAND_CRYPTO_*_SIZE
 * constants, so the warning is a false positive for our usage. */
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wstringop-overread"
#endif
#include "vendor/monocypher/monocypher.h"
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __linux__
#  include <sys/random.h>
#elif defined(__APPLE__)
#  include <stdlib.h> /* arc4random */
#elif defined(_WIN32)
#  include <windows.h>
#  include <wincrypt.h>
#  pragma comment(lib, "advapi32.lib")
#endif

int crypto_seal(const uint8_t key[WASTELAND_CRYPTO_KEY_SIZE],
                const uint8_t nonce[WASTELAND_CRYPTO_NONCE_SIZE],
                const uint8_t *plain, size_t plain_len,
                uint8_t *cipher,
                uint8_t tag[WASTELAND_CRYPTO_TAG_SIZE])
{
    if (!key || !nonce || (!plain && plain_len > 0) || !cipher || !tag)
        return -1;

    crypto_aead_lock(cipher, tag, key, nonce, NULL, 0, plain, plain_len);
    return 0;
}

int crypto_open(const uint8_t key[WASTELAND_CRYPTO_KEY_SIZE],
                const uint8_t nonce[WASTELAND_CRYPTO_NONCE_SIZE],
                const uint8_t *cipher, size_t cipher_len,
                const uint8_t tag[WASTELAND_CRYPTO_TAG_SIZE],
                uint8_t *plain)
{
    if (!key || !nonce || (!cipher && cipher_len > 0) || !tag || !plain)
        return -1;

    if (crypto_aead_unlock(plain, tag, key, nonce, NULL, 0,
                           cipher, cipher_len) != 0) {
        /* Authentication failure — plaintext was tampered or key is wrong. */
        return -1;
    }
    return 0;
}

void crypto_random_bytes(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return;

#if defined(__linux__)
    /* getrandom() is available since glibc 2.25 / Linux 3.17. */
    size_t got = 0;
    while (got < len) {
        ssize_t r = getrandom(buf + got, len - got, 0);
        if (r < 0) {
            perror("[crypto] getrandom");
            break;
        }
        got += (size_t)r;
    }
    if (got == len) return;
    /* Fallback to /dev/urandom if getrandom fails or is short. */
#elif defined(__APPLE__)
    arc4random_buf(buf, len);
    return;
#elif defined(_WIN32)
    HCRYPTPROV h = 0;
    if (CryptAcquireContextA(&h, NULL, NULL, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT)) {
        CryptGenRandom(h, (DWORD)len, buf);
        CryptReleaseContext(h, 0);
        return;
    }
#endif

    /* Universal fallback: /dev/urandom */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t n = fread(buf, 1, len, f);
        fclose(f);
        if (n == len) return;
    }

    /* Last resort — not cryptographically secure, but prevents crashes.
     * This path should never be reached on any supported OS. */
    fprintf(stderr, "[crypto] WARNING: using insecure fallback RNG\n");
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(rand() & 0xFF);
    }
}
