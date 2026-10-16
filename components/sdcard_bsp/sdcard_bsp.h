#ifndef SDCARD_BSP_H
#define SDCARD_BSP_H
#include "driver/sdmmc_types.h"
#include "esp_err.h"
#include "freertos/event_groups.h"


typedef struct
{
  float sdcard_size;
}sdcard_bsp_t;

extern sdcard_bsp_t user_sdcard_bsp;
extern EventGroupHandle_t sdcard_even_;

#ifdef __cplusplus
extern "C" {
#endif


void _sdcard_init(void);
// Checked, non-formatting mount. Caller must run on a worker, not while holding LVGL.
// ESP_ERR_INVALID_STATE means another task is mounting; existing mount is status-checked.
esp_err_t sdcard_mount_checked(void);
esp_err_t sdcard_file_write(const char *path, const char *data);
esp_err_t sdcard_file_read(const char *path, char *buffer, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif