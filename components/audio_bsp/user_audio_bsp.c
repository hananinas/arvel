#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "user_audio_bsp.h"
#include "esp_log.h"
#include "esp_err.h"

#include "esp_gmf_init.h"

#include "codec_board.h"
#include "codec_init.h"

#define SAMPLE_RATE     24000           // 采样率：24000Hz
#define BIT_DEPTH       32              // 位深：32位


esp_codec_dev_handle_t playback = NULL;
esp_codec_dev_handle_t record = NULL;
static bool playback_open_ok = false;
static bool record_open_ok = false;


extern const uint8_t music_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t music_pcm_end[]   asm("_binary_canon_pcm_end");


void user_audio_bsp_init(void)
{
  set_codec_board_type("S3_LCD_3_49");
  codec_init_cfg_t codec_cfg = 
  {
    .in_mode = CODEC_I2S_MODE_TDM,
    .out_mode = CODEC_I2S_MODE_TDM,
    .in_use_tdm = false,
    .reuse_dev = false,
  };
  ESP_ERROR_CHECK(init_codec(&codec_cfg));
  playback = get_playback_handle();
  record = get_record_handle();
}


void i2s_music(void *args)
{
  esp_codec_dev_set_out_vol(playback, 60.0);  //设置60声音大小
  for(;;)
  {
    size_t bytes_write = 0;
    size_t bytes_sizt = music_pcm_end - music_pcm_start;
    uint8_t *data_ptr = (uint8_t *)music_pcm_start;
    esp_codec_dev_sample_info_t fs = {
      .sample_rate = 24000,
      .channel = 2,
      .bits_per_sample = 16,
    };
    if(esp_codec_dev_open(playback, &fs) == ESP_CODEC_DEV_OK)
    {
      while (bytes_write < bytes_sizt)
      {
        esp_codec_dev_write(playback, data_ptr, 256);
        data_ptr += 256;
        bytes_write += 256;
      }
      //esp_codec_dev_close(playback); //close 
    }
    else
    {
      break;
    }
  }
  vTaskDelete(NULL);
}

void i2s_echo(void *arg)
{
  esp_codec_dev_set_out_vol(playback, 70.0); //设置90声音大小
  esp_codec_dev_set_in_gain(record, 20.0);   //设置录音时的增益
  uint8_t *data_ptr = (uint8_t *)heap_caps_malloc(1024 * sizeof(uint8_t), MALLOC_CAP_DEFAULT);
  esp_codec_dev_sample_info_t fs = {
    .sample_rate = SAMPLE_RATE,
    .channel = 2,
    .bits_per_sample = BIT_DEPTH,
  };
  esp_codec_dev_open(playback, &fs); //打开播放
  esp_codec_dev_open(record, &fs);   //打开录音
  for(;;)
  {
    if(ESP_CODEC_DEV_OK == esp_codec_dev_read(record, data_ptr, 1024))
    {
      esp_codec_dev_write(playback, data_ptr, 1024);
    }
  }
}

void example_i2s_audio_Test(void *arg)
{
  uint8_t *Modes = (uint8_t *)arg;
  uint8_t *rec_data_ptr = (uint8_t *)heap_caps_malloc(512 * sizeof(uint8_t), MALLOC_CAP_DEFAULT);
  esp_codec_dev_set_out_vol(playback, 100); //设置100声音大小
  esp_codec_dev_set_in_gain(record, 20.0);   //设置录音时的增益
  esp_codec_dev_sample_info_t fs = 
  {
    .sample_rate = 24000,            //24000
    .channel = 2,
    .bits_per_sample = 16,
  };
  esp_codec_dev_open(playback, &fs); //打开播放
  esp_codec_dev_open(record, &fs);   //打开录音
  size_t bytes_sizt = music_pcm_end - music_pcm_start;  //获取PCM文件的大小
  for(;;)
  {
    if(*Modes == 0) //播放音乐模式
    {
      size_t bytes_write = 0;
      uint8_t *data_ptr = (uint8_t *)music_pcm_start;
      while ((bytes_write < bytes_sizt) && (*Modes == 0))
      {
        esp_codec_dev_write(playback, data_ptr, 256);
        data_ptr += 256;
        bytes_write += 256;
      }
    }
    else
    {
      while((*Modes == 1))
      {
        if(ESP_CODEC_DEV_OK == esp_codec_dev_read(record, rec_data_ptr, 512))
        {
          //esp_codec_dev_write(playback, rec_data_ptr, 512);   获取MIC数据 , Obtain MIC data
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void audio_play_init(void)
{
  playback_open_ok = false;
  record_open_ok = false;
  if (!playback || !record) {
    ESP_LOGE("audio", "Initialize codec handles before opening audio");
    return;
  }
  int out_vol = esp_codec_dev_set_out_vol(playback, 100.0);
  int in_gain = esp_codec_dev_set_in_gain(record, 35.0);
  esp_codec_dev_sample_info_t fs = {};
  fs.sample_rate = 16000;
  fs.channel = 2;
  fs.bits_per_sample = 16;
  int out_status = esp_codec_dev_open(playback, &fs);
  int in_status = esp_codec_dev_open(record, &fs);
  playback_open_ok = out_status == ESP_CODEC_DEV_OK && out_vol == ESP_CODEC_DEV_OK;
  record_open_ok = in_status == ESP_CODEC_DEV_OK && in_gain == ESP_CODEC_DEV_OK;
  if (!playback_open_ok || !record_open_ok) {
    ESP_LOGE("audio", "Audio initialization failed: output=%d volume=%d input=%d gain=%d",
             out_status, out_vol, in_status, in_gain);
  }
}

void audio_playback_read(void *data_ptr,uint32_t len)
{
	esp_codec_dev_read(record, data_ptr, len);
}

void audio_playback_read_chunked(void *data_ptr, uint32_t total_len, uint32_t chunk_size)
{
	uint8_t *write_ptr = (uint8_t *)data_ptr;
	uint32_t total_read = 0;
	
	ESP_LOGI("audio", "Starting chunked audio read: total=%d, chunk=%d", total_len, chunk_size);
	
	while (total_read < total_len) {
		uint32_t bytes_to_read = (total_len - total_read) < chunk_size ? (total_len - total_read) : chunk_size;
		
		if (esp_codec_dev_read(record, write_ptr, bytes_to_read) == ESP_CODEC_DEV_OK) {
			write_ptr += bytes_to_read;
			total_read += bytes_to_read;
		} else {
			ESP_LOGW("audio", "Read failed at %d bytes", total_read);
			vTaskDelay(pdMS_TO_TICKS(1));
		}
		
		// Allow other tasks to run periodically
		if (total_read % (chunk_size * 10) == 0) {
			vTaskDelay(pdMS_TO_TICKS(1));
		}
	}
	
	ESP_LOGI("audio", "Chunked audio read completed: %d bytes", total_read);
}

void audio_playback_write(void *data_ptr,uint32_t len)
{
	esp_codec_dev_write(playback, data_ptr, len);
}

bool audio_playback_write_checked(const void *data_ptr, uint32_t len)
{
  if (!playback || !playback_open_ok || !data_ptr || len == 0 ||
      (len % 4) != 0 || len > INT_MAX) {
    return false;
  }
  int status = esp_codec_dev_write(playback, (void *)data_ptr, (int)len);
  if (status != ESP_CODEC_DEV_OK) {
    ESP_LOGE("audio", "Codec playback failed: %d", status);
    return false;
  }
  return true;
}

bool audio_playback_read_chunked_checked(void *data_ptr, uint32_t total_len, uint32_t chunk_size)
{
  if (!record || !record_open_ok || !data_ptr || total_len == 0 ||
      chunk_size == 0 || (total_len % 4) != 0 || (chunk_size % 4) != 0 ||
      chunk_size > INT_MAX) {
    return false;
  }
  uint8_t *ptr = (uint8_t *)data_ptr;
  for (uint32_t offset = 0; offset < total_len;) {
    uint32_t len = total_len - offset;
    if (len > chunk_size) len = chunk_size;
    int status = esp_codec_dev_read(record, ptr + offset, (int)len);
    if (status != ESP_CODEC_DEV_OK) {
      ESP_LOGE("audio", "Codec capture failed at %u bytes: %d", (unsigned)offset, status);
      return false;
    }
    offset += len;
  }
  return true;
}


void audio_playback_set_vol(uint8_t vol)
{
  esp_codec_dev_set_out_vol(playback, vol);   //设置60声音大小
}
















/*
Board: AMOLED_1_43
i2c: {sda: 18, scl: 8}
i2s: {bclk: 21, ws: 22, dout: 23, din: 20, mclk: 19}
out: {codec: ES8311, pa: -1, use_mclk: 1, pa_gain:6}
in: {codec: ES7210}
*/