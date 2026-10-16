#pragma once
#include "esp_err.h"
#include <stddef.h>
typedef unsigned nvs_handle_t;
typedef struct { size_t used_entries, free_entries, total_entries, namespace_count; } nvs_stats_t;
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
esp_err_t nvs_get_stats(const char *partition, nvs_stats_t *stats);
#ifdef __cplusplus
}
#endif
