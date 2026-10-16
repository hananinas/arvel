#ifndef HOST_SHA256_H
#define HOST_SHA256_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int mbedtls_sha256(const unsigned char *input, size_t length, unsigned char *output, int is224);
#ifdef __cplusplus
}
#endif
#endif
