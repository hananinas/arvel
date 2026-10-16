#ifndef HOST_ESP_RANDOM_H
#define HOST_ESP_RANDOM_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void esp_fill_random(void *output, size_t length);
uint32_t esp_random(void);
#ifdef __cplusplus
}
#endif
#endif
