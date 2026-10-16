#ifndef HOST_NVS_H
#define HOST_NVS_H
#include "esp_err.h"
#include <stddef.h>
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *bytes, size_t *size);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *bytes, size_t size);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);
#ifdef __cplusplus
}
#endif
#endif
