#ifndef HOST_PKCS5_H
#define HOST_PKCS5_H
#include <stddef.h>
#include <stdint.h>
#define MBEDTLS_MD_SHA256 6
#ifdef __cplusplus
extern "C" {
#endif
int mbedtls_pkcs5_pbkdf2_hmac_ext(int md_type, const unsigned char *password,
    size_t length, const unsigned char *salt, size_t salt_length,
    unsigned iterations, uint32_t key_length, unsigned char *output);
#ifdef __cplusplus
}
#endif
#endif
