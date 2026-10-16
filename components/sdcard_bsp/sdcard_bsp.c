#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_default_configs.h"
#include "sdcard_bsp.h"
#include "esp_log.h"
#include "esp_err.h"

#define SDMMC_D0_PIN    40  
#define SDMMC_CLK_PIN   41
#define SDMMC_CMD_PIN   39

static const char *TAG = "_sdcard";

EventGroupHandle_t sdcard_even_ = NULL;

sdcard_bsp_t user_sdcard_bsp;

#define SDlist "/sdcard" //目录,类似于一个标准

sdmmc_card_t *card_host = NULL;

esp_err_t sdcard_mount_checked(void)
{
  static volatile int mounting = 0;
  if (__sync_lock_test_and_set(&mounting, 1)) return ESP_ERR_INVALID_STATE;
  if (card_host != NULL) {
    esp_err_t result = sdmmc_get_status(card_host);
    __sync_lock_release(&mounting);
    return result;
  }
  esp_vfs_fat_sdmmc_mount_config_t mount_config = {
    .format_if_mount_failed = false,
    .max_files = 8,
    .allocation_unit_size = 16 * 1024,
  };
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
  sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
  slot_config.width = 1;
  slot_config.clk = SDMMC_CLK_PIN;
  slot_config.cmd = SDMMC_CMD_PIN;
  slot_config.d0 = SDMMC_D0_PIN;
  esp_err_t result = esp_vfs_fat_sdmmc_mount(SDlist, &host, &slot_config, &mount_config, &card_host);
  if (result == ESP_OK && card_host != NULL) {
    user_sdcard_bsp.sdcard_size = (float)(card_host->csd.capacity)/2048/1024;
  } else if (result == ESP_OK) {
    result = ESP_FAIL;
  }
  __sync_lock_release(&mounting);
  return result;
}

void _sdcard_init(void)
{
  if (sdcard_even_ == NULL) sdcard_even_ = xEventGroupCreate();
  esp_err_t result = sdcard_mount_checked();
  if (result == ESP_OK && sdcard_even_ != NULL) xEventGroupSetBits(sdcard_even_, 0x01);
  else ESP_LOGW(TAG, "SD mount unavailable: %s", esp_err_to_name(result));
}



/* Write data
path: Path
data: Data */ 
esp_err_t sdcard_file_write(const char *path, const char *data)
{
  esp_err_t err;
  if(card_host == NULL)
  {
    ESP_LOGE(TAG, "SD card not initialized (card == NULL)");
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card_host); //First, check if there is an SD card.
  if(err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card status check failed (card not present or unresponsive)");
    return err;
  }
  FILE *f = fopen(path, "w"); //Obtain the path address
  if(f == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return ESP_ERR_NOT_FOUND;
  }
  fprintf(f, data); 
  fclose(f);
  return ESP_OK;
}
/*
Read data
path: path */
esp_err_t sdcard_file_read(const char *path, char *buffer, size_t *out_len)
{
  esp_err_t err;
  if(card_host == NULL)
  {
    ESP_LOGE(TAG, "SD card not initialized (card == NULL)");
    return ESP_ERR_NOT_FOUND;
  }
  err = sdmmc_get_status(card_host); //First, check if there is an SD card.
  if(err != ESP_OK)
  {
    ESP_LOGE(TAG, "SD card status check failed (card not present or unresponsive)");
    return err;
  }
  FILE *f = fopen(path, "rb");
  if (f == NULL)
  {
    ESP_LOGE(TAG, "Failed to open file: %s", path);
    return ESP_ERR_NOT_FOUND;
  }
  fseek(f, 0, SEEK_END);     //Move the pointer to the very end.
  uint32_t unlen = ftell(f);
  //fgets(pxbuf, unlen, f); //read characters from file
  fseek(f, 0, SEEK_SET); //Move the pointer to the very beginning.
  uint32_t poutLen = fread((void *)buffer,1,unlen,f);
  if(out_len != NULL)
  *out_len = poutLen;
  fclose(f);
  return ESP_OK;
}