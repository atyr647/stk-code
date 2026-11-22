/* mbedtls API subset STK uses (src/network/crypto_mbedtls.*), for the N64.
 * Pure computations (SHA-256, base64) are real; the network crypto (AES-GCM,
 * entropy/DRBG) only matters for online play and reports failure. */
#ifndef STK_N64_MBEDTLS_VERSION_H
#define STK_N64_MBEDTLS_VERSION_H
#define MBEDTLS_VERSION_MAJOR 2
#define MBEDTLS_VERSION_MINOR 28
#define MBEDTLS_VERSION_PATCH 0
#define MBEDTLS_VERSION_NUMBER 0x021C0000
#define MBEDTLS_VERSION_STRING "2.28.0-n64stub"
#endif
