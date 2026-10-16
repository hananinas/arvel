#ifndef HOST_ESP_ERR_H
#define HOST_ESP_ERR_H
#include <stdint.h>
typedef int32_t esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_NOT_INITIALIZED 0x1101
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
#endif
