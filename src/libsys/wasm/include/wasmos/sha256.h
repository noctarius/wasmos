/* sha256.h - Minimal SHA-256 implementation for WASM drivers and services.
 * Used for integrity checking of loaded modules and signing metadata in
 * .wap package headers.  Freestanding; no external dependencies. */
#ifndef WASMOS_LIBSYS_SHA256_H
#define WASMOS_LIBSYS_SHA256_H

#include <stdint.h>
#include "string.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} wasmos_sha256_ctx_t;

static inline uint32_t
wasmos_sha256_rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

static inline void
wasmos_sha256_transform(wasmos_sha256_ctx_t *ctx, const uint8_t data[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t m[64];
    for (uint32_t i = 0; i < 16; ++i) {
        m[i] = ((uint32_t)data[i * 4] << 24) | ((uint32_t)data[i * 4 + 1] << 16) |
               ((uint32_t)data[i * 4 + 2] << 8) | (uint32_t)data[i * 4 + 3];
    }
    for (uint32_t i = 16; i < 64; ++i) {
        uint32_t s0 = wasmos_sha256_rotr(m[i - 15], 7) ^ wasmos_sha256_rotr(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = wasmos_sha256_rotr(m[i - 2], 17) ^ wasmos_sha256_rotr(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t S1 = wasmos_sha256_rotr(e, 6) ^ wasmos_sha256_rotr(e, 11) ^ wasmos_sha256_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k[i] + m[i];
        uint32_t S0 = wasmos_sha256_rotr(a, 2) ^ wasmos_sha256_rotr(a, 13) ^ wasmos_sha256_rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

/* Initialise ctx with the standard SHA-256 IV (H0..H7). */
static inline void
wasmos_sha256_init(wasmos_sha256_ctx_t *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u; ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu; ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
}

static inline void
wasmos_sha256_update(wasmos_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            wasmos_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static inline void
wasmos_sha256_final(wasmos_sha256_ctx_t *ctx, uint8_t hash[32])
{
    uint32_t i = ctx->datalen;
    if (ctx->datalen < 56) {
        ctx->data[i++] = 0x80u;
        while (i < 56) ctx->data[i++] = 0;
    } else {
        ctx->data[i++] = 0x80u;
        while (i < 64) ctx->data[i++] = 0;
        wasmos_sha256_transform(ctx, ctx->data);
        for (i = 0; i < 56; ++i) ctx->data[i] = 0;
    }
    ctx->bitlen += (uint64_t)ctx->datalen * 8u;
    ctx->data[63] = (uint8_t)(ctx->bitlen);
    ctx->data[62] = (uint8_t)(ctx->bitlen >> 8);
    ctx->data[61] = (uint8_t)(ctx->bitlen >> 16);
    ctx->data[60] = (uint8_t)(ctx->bitlen >> 24);
    ctx->data[59] = (uint8_t)(ctx->bitlen >> 32);
    ctx->data[58] = (uint8_t)(ctx->bitlen >> 40);
    ctx->data[57] = (uint8_t)(ctx->bitlen >> 48);
    ctx->data[56] = (uint8_t)(ctx->bitlen >> 56);
    wasmos_sha256_transform(ctx, ctx->data);
    for (i = 0; i < 4; ++i) {
        for (uint32_t j = 0; j < 8; ++j) {
            hash[i + (j * 4)] = (uint8_t)((ctx->state[j] >> (24 - i * 8)) & 0xFFu);
        }
    }
}

/* Compute SHA-256(in) and write the first 16 hex characters into out[0..15],
 * NUL-terminated at out[16].  Used as a stable compact identifier for modules. */
static inline void
wasmos_sha256_hex16_prefix(const char *in, char out[17])
{
    static const char *hex = "0123456789abcdef";
    wasmos_sha256_ctx_t ctx;
    uint8_t digest[32];
    wasmos_sha256_init(&ctx);
    wasmos_sha256_update(&ctx, (const uint8_t *)in, (uint32_t)strlen(in));
    wasmos_sha256_final(&ctx, digest);
    for (uint32_t i = 0; i < 8; ++i) {
        out[i * 2] = hex[(digest[i] >> 4) & 0xFu];
        out[i * 2 + 1] = hex[digest[i] & 0xFu];
    }
    out[16] = '\0';
}

#ifdef __cplusplus
}
#endif

#endif
