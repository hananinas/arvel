#ifndef HOST_PLATFORM_UTIL_H
#define HOST_PLATFORM_UTIL_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void mbedtls_platform_zeroize(void *buffer, size_t size);
#ifdef __cplusplus
}
#endif
#endif
