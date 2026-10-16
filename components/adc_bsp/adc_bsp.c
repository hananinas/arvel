#include <stdio.h>
#include "adc_bsp.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BAT_ADC_CHANNEL ADC_CHANNEL_3   /* GPIO4 */
#define BAT_DIVIDER 3
#define BAT_SAMPLES 16
#define BAT_PRESENT_MV 2500             /* below this no cell is connected */

static const char *TAG = "battery";
static adc_cali_handle_t cali_handle;
static bool calibrated;
static adc_oneshot_unit_handle_t adc1_handle;
static SemaphoreHandle_t adc_lock;

esp_err_t adc_bsp_init(void)
{
  if (adc1_handle) return ESP_OK;
  adc_lock = xSemaphoreCreateMutex();
  if (!adc_lock) return ESP_ERR_NO_MEM;
  adc_cali_curve_fitting_config_t cali_config = {
    .unit_id = ADC_UNIT_1,
    .chan = BAT_ADC_CHANNEL,
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  /* Without eFuse calibration, fall back to the nominal transfer curve. */
  calibrated = adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK;
  if (!calibrated) ESP_LOGW(TAG, "ADC calibration unavailable; battery voltage is approximate");
  adc_oneshot_unit_init_cfg_t init_config = {
    .unit_id = ADC_UNIT_1,
  };
  esp_err_t err = adc_oneshot_new_unit(&init_config, &adc1_handle);
  if (err != ESP_OK) {
    adc1_handle = NULL;
    return err;
  }
  adc_oneshot_chan_cfg_t config = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  return adc_oneshot_config_channel(adc1_handle, BAT_ADC_CHANNEL, &config);
}

/* Pin millivolts for one raw sample. */
static int to_millivolts(int raw)
{
  int mv = 0;
  if (calibrated && adc_cali_raw_to_voltage(cali_handle, raw, &mv) == ESP_OK) return mv;
  return raw * 3100 / 4095;
}

/* Typical single-cell Li-ion resting voltage at each 10% step. */
static int estimate_percent(int mv)
{
  static const int curve[][2] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3920, 70}, {3870, 60}, {3820, 50},
    {3790, 40}, {3770, 30}, {3740, 20}, {3680, 10}, {3450, 5}, {3000, 0},
  };
  const int n = sizeof(curve) / sizeof(curve[0]);
  if (mv >= curve[0][0]) return 100;
  for (int i = 1; i < n; ++i) {
    if (mv >= curve[i][0]) {
      const int span = curve[i - 1][0] - curve[i][0];
      return curve[i][1] + (curve[i - 1][1] - curve[i][1]) * (mv - curve[i][0]) / span;
    }
  }
  return 0;
}

esp_err_t battery_read(battery_reading_t *reading)
{
  if (!reading) return ESP_ERR_INVALID_ARG;
  if (!adc1_handle) return ESP_ERR_INVALID_STATE;
  int sum = 0, count = 0;
  xSemaphoreTake(adc_lock, portMAX_DELAY);
  for (int i = 0; i < BAT_SAMPLES; ++i) {
    int raw = 0;
    if (adc_oneshot_read(adc1_handle, BAT_ADC_CHANNEL, &raw) == ESP_OK) {
      sum += to_millivolts(raw);
      ++count;
    }
  }
  xSemaphoreGive(adc_lock);
  if (!count) return ESP_FAIL;
  reading->millivolts = sum / count * BAT_DIVIDER;
  reading->present = reading->millivolts >= BAT_PRESENT_MV;
  reading->percent = reading->present ? estimate_percent(reading->millivolts) : 0;
  reading->calibrated = calibrated;
  return ESP_OK;
}
