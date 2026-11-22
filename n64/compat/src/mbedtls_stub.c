/* mbedtls subset for the N64: see include/mbedtls/version.h.
 * SHA-256 (FIPS 180-4) and base64 (RFC 4648) are real implementations. */
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>
#include <stdint.h>
#include <string.h>

#define CRYPTO_UNAVAILABLE -0x0070    /* MBEDTLS_ERR_..._FEATURE_UNAVAILABLE */

void mbedtls_entropy_init(mbedtls_entropy_context *c) { c->unused = 0; }
void mbedtls_entropy_free(mbedtls_entropy_context *c) { (void)c; }
int mbedtls_entropy_func(void *d, unsigned char *o, size_t l)
{ (void)d; (void)o; (void)l; return CRYPTO_UNAVAILABLE; }
void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *c) { c->seeded = 0; }
void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *c) { (void)c; }
int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *c,
                          int (*f)(void *, unsigned char *, size_t), void *p,
                          const unsigned char *cu, size_t l)
{ (void)c; (void)f; (void)p; (void)cu; (void)l; return CRYPTO_UNAVAILABLE; }
int mbedtls_ctr_drbg_random(void *p, unsigned char *o, size_t l)
{ (void)p; (void)o; (void)l; return CRYPTO_UNAVAILABLE; }
void mbedtls_gcm_init(mbedtls_gcm_context *c) { c->keyed = 0; }
void mbedtls_gcm_free(mbedtls_gcm_context *c) { (void)c; }
int mbedtls_gcm_setkey(mbedtls_gcm_context *c, mbedtls_cipher_id_t ci,
                       const unsigned char *k, unsigned int b)
{ (void)c; (void)ci; (void)k; (void)b; return CRYPTO_UNAVAILABLE; }
int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context *c, int m, size_t l,
    const unsigned char *iv, size_t ivl, const unsigned char *a, size_t al,
    const unsigned char *in, unsigned char *out, size_t tl, unsigned char *t)
{ (void)c; (void)m; (void)l; (void)iv; (void)ivl; (void)a; (void)al; (void)in;
  (void)out; (void)tl; (void)t; return CRYPTO_UNAVAILABLE; }
int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *c, size_t l,
    const unsigned char *iv, size_t ivl, const unsigned char *a, size_t al,
    const unsigned char *t, size_t tl, const unsigned char *in,
    unsigned char *out)
{ (void)c; (void)l; (void)iv; (void)ivl; (void)a; (void)al; (void)t; (void)tl;
  (void)in; (void)out; return MBEDTLS_ERR_GCM_AUTH_FAILED; }

/* ---- base64 ---- */
static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    size_t need = 4 * ((slen + 2) / 3) + 1;
    if (dlen < need || dst == NULL) { *olen = need; return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL; }
    unsigned char *p = dst;
    for (size_t i = 0; i < slen; i += 3) {
        uint32_t v = src[i] << 16;
        if (i + 1 < slen) v |= src[i + 1] << 8;
        if (i + 2 < slen) v |= src[i + 2];
        *p++ = b64[v >> 18 & 63];
        *p++ = b64[v >> 12 & 63];
        *p++ = i + 1 < slen ? b64[v >> 6 & 63] : '=';
        *p++ = i + 2 < slen ? b64[v & 63] : '=';
    }
    *p = 0;
    *olen = (size_t)(p - dst);
    return 0;
}

static int b64val(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    size_t n = 0, pad = 0;
    for (size_t i = 0; i < slen; i++) {
        if (src[i] == '=') pad++;
        else if (src[i] == '\r' || src[i] == '\n' || src[i] == ' ') continue;
        else if (b64val(src[i]) < 0 || pad) return MBEDTLS_ERR_BASE64_INVALID_CHARACTER;
        else n++;
    }
    size_t need = (n * 6) / 8;
    if (dst == NULL || dlen < need) { *olen = need; return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL; }
    uint32_t acc = 0; int bits = 0; size_t o = 0;
    for (size_t i = 0; i < slen; i++) {
        int v = b64val(src[i]);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; dst[o++] = (unsigned char)(acc >> bits); }
    }
    *olen = o;
    return 0;
}

/* ---- SHA-256 ---- */
static const uint32_t K[64] = {
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
 0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
 0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
 0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
 0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
 0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const unsigned char *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (ROR(e,6) ^ ROR(e,11) ^ ROR(e,25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        uint32_t t2 = (ROR(a,2) ^ ROR(a,13) ^ ROR(a,22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

int mbedtls_sha256_ret(const unsigned char *in, size_t len,
                       unsigned char out[32], int is224)
{
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    if (is224) return CRYPTO_UNAVAILABLE;
    size_t i = 0;
    for (; i + 64 <= len; i += 64) sha256_block(h, in + i);
    unsigned char tail[128] = {0};
    size_t rem = len - i;
    memcpy(tail, in + i, rem);
    tail[rem] = 0x80;
    size_t tl = rem + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int k = 0; k < 8; k++) tail[tl - 1 - k] = (unsigned char)(bits >> (8 * k));
    sha256_block(h, tail);
    if (tl == 128) sha256_block(h, tail + 64);
    for (int k = 0; k < 8; k++) {
        out[4*k] = h[k] >> 24; out[4*k+1] = h[k] >> 16;
        out[4*k+2] = h[k] >> 8; out[4*k+3] = h[k];
    }
    return 0;
}
