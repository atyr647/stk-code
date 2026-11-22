#ifndef STK_N64_MBEDTLS_GCM_H
#define STK_N64_MBEDTLS_GCM_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum { MBEDTLS_CIPHER_ID_NONE = 0, MBEDTLS_CIPHER_ID_NULL,
               MBEDTLS_CIPHER_ID_AES } mbedtls_cipher_id_t;
#define MBEDTLS_GCM_ENCRYPT 1
#define MBEDTLS_GCM_DECRYPT 0
#define MBEDTLS_ERR_GCM_AUTH_FAILED -0x0012
typedef struct { int keyed; } mbedtls_gcm_context;
void mbedtls_gcm_init(mbedtls_gcm_context *ctx);
int mbedtls_gcm_setkey(mbedtls_gcm_context *ctx, mbedtls_cipher_id_t cipher,
                       const unsigned char *key, unsigned int keybits);
int mbedtls_gcm_crypt_and_tag(mbedtls_gcm_context *ctx, int mode,
                              size_t length, const unsigned char *iv,
                              size_t iv_len, const unsigned char *add,
                              size_t add_len, const unsigned char *input,
                              unsigned char *output, size_t tag_len,
                              unsigned char *tag);
int mbedtls_gcm_auth_decrypt(mbedtls_gcm_context *ctx, size_t length,
                             const unsigned char *iv, size_t iv_len,
                             const unsigned char *add, size_t add_len,
                             const unsigned char *tag, size_t tag_len,
                             const unsigned char *input,
                             unsigned char *output);
void mbedtls_gcm_free(mbedtls_gcm_context *ctx);
#ifdef __cplusplus
}
#endif
#endif
