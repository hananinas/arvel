#ifndef ESP_WIFI_BSP_H
#define ESP_WIFI_BSP_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_STARTED_BIT    (1U << 0)
#define WIFI_SCAN_DONE_BIT  (1U << 1)
#define WIFI_CONNECTED_BIT  (1U << 2)

extern EventGroupHandle_t wifi_even_;


typedef struct
{
  char _ip[25];
  int8_t rssi;
  int8_t apNum;
}esp_bsp_t;
extern esp_bsp_t user_esp_bsp;

#ifdef __cplusplus
extern "C" {
#endif

void espwifi_init(void);
void espwifi_deinit(void);


#ifdef __cplusplus
}
#endif

#endif