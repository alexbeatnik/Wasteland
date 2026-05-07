/* ============================================================================
 * sha256.c — Single-file SHA-256 (NIST FIPS 180-4), pure C11, no dependencies.
 * ============================================================================ */

#include "sha256.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Initial hash values (first 32 bits of fractional parts of square roots of
 * the first 8 primes).
 * --------------------------------------------------------------------------- */
static const uint32_t K[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

/* Byte-swap on little-endian hosts. SHA-256 operates in big-endian. */
static inline uint32_t bswap32(uint32_t x)
{
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(x);
#elif defined(_MSC_VER)
    return _byteswap_ulong(x);
#else
    return ((x & 0xFF000000U) >> 24) |
           ((x & 0x00FF0000U) >>  8) |
           ((x & 0x0000FF00U) <<  8) |
           ((x & 0x000000FFU) << 24);
#endif
}

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR((x), 2) ^ ROTR((x), 13) ^ ROTR((x), 22))
#define EP1(x) (ROTR((x), 6) ^ ROTR((x), 11) ^ ROTR((x), 25))
#define SIG0(x) (ROTR((x), 7) ^ ROTR((x), 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR((x), 17) ^ ROTR((x), 19) ^ ((x) >> 10))

static void sha256_transform(uint32_t state[8], const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t m[64];
    int i;

    for (i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4 + 0] << 24) |
               ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] <<  8) |
               ((uint32_t)data[i * 4 + 3] <<  0);
    }
    for (i = 16; i < 64; i++) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }

    a = state[0]; b = state[1]; c = state[2]; d = state[3];
    e = state[4]; f = state[5]; g = state[6]; h = state[7];

    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        uint32_t t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

void wasteland_sha256_init(wasteland_sha256_ctx_t *ctx)
{
    static const uint32_t H0[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    memset(ctx, 0, sizeof(*ctx));
    memcpy(ctx->state, H0, sizeof(H0));
}

void wasteland_sha256_update(wasteland_sha256_ctx_t *ctx,
                             const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t room = WASTELAND_SHA256_BLOCK_SIZE - ctx->buffer_used;
        size_t chunk = (len < room) ? len : room;
        memcpy(ctx->buffer + ctx->buffer_used, data, chunk);
        ctx->buffer_used += chunk;
        data += chunk;
        len  -= chunk;

        if (ctx->buffer_used == WASTELAND_SHA256_BLOCK_SIZE) {
            sha256_transform(ctx->state, ctx->buffer);
            ctx->bit_count += (uint64_t)WASTELAND_SHA256_BLOCK_SIZE * 8;
            ctx->buffer_used = 0;
        }
    }
}

void wasteland_sha256_final(wasteland_sha256_ctx_t *ctx,
                            uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE])
{
    uint64_t total_bits = ctx->bit_count + (uint64_t)ctx->buffer_used * 8;

    /* Pad with 0x80 then zeros, leaving room for the 8-byte length. */
    ctx->buffer[ctx->buffer_used++] = 0x80;
    if (ctx->buffer_used > 56) {
        memset(ctx->buffer + ctx->buffer_used, 0,
               WASTELAND_SHA256_BLOCK_SIZE - ctx->buffer_used);
        sha256_transform(ctx->state, ctx->buffer);
        ctx->buffer_used = 0;
    }
    memset(ctx->buffer + ctx->buffer_used, 0,
           WASTELAND_SHA256_BLOCK_SIZE - 8 - ctx->buffer_used);

    /* Append total bit length as big-endian 64-bit integer. */
    ctx->buffer[56] = (uint8_t)(total_bits >> 56);
    ctx->buffer[57] = (uint8_t)(total_bits >> 48);
    ctx->buffer[58] = (uint8_t)(total_bits >> 40);
    ctx->buffer[59] = (uint8_t)(total_bits >> 32);
    ctx->buffer[60] = (uint8_t)(total_bits >> 24);
    ctx->buffer[61] = (uint8_t)(total_bits >> 16);
    ctx->buffer[62] = (uint8_t)(total_bits >>  8);
    ctx->buffer[63] = (uint8_t)(total_bits >>  0);
    sha256_transform(ctx->state, ctx->buffer);

    /* Produce big-endian digest. */
    for (int i = 0; i < 8; i++) {
        digest[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >>  8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] >>  0);
    }
}

void wasteland_sha256(const uint8_t *data, size_t len,
                      uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE])
{
    wasteland_sha256_ctx_t ctx;
    wasteland_sha256_init(&ctx);
    wasteland_sha256_update(&ctx, data, len);
    wasteland_sha256_final(&ctx, digest);
}

static const char HEX[] = "0123456789abcdef";

void wasteland_sha256_hex(const uint8_t digest[WASTELAND_SHA256_DIGEST_SIZE],
                          char *out, size_t out_size)
{
    if (out_size < 65) return;
    for (int i = 0; i < 32; i++) {
        out[i * 2 + 0] = HEX[digest[i] >> 4];
        out[i * 2 + 1] = HEX[digest[i] & 0x0F];
    }
    out[64] = '\0';
}
