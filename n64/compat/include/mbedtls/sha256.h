#ifndef STK_N64_MBEDTLS_SHA256_H
#define STK_N64_MBEDTLS_SHA256_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int mbedtls_sha256_ret(const unsigned char *input, size_t ilen,
                       unsigned char output[32], int is224);
#ifdef __cplusplus
}
#endif
#endif
