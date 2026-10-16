#ifndef ESP_WIFI_BSP_H
#define ESP_WIFI_BSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_err.h"

#define WIFI_STARTED_BIT    (1U << 0)
#define WIFI_CONNECTED_BIT  (1U << 2)

extern EventGroupHandle_t wifi_even_;



#ifdef __cplusplus
extern "C" {
#endif

void espwifi_init(void);
void espwifi_deinit(void);

#define ESPWIFI_PROVISIONING_SSID "Unravel-Agent"
#define ESPWIFI_PROVISIONING_PASSWORD_CAPACITY 9
#define ESPWIFI_PROVISIONING_SECONDS 300
/* Physical native UI only. Opens/refreshes AP with a fresh eight-digit
 * temporary WPA2 code, including any leading zeros. password needs at least
 * nine bytes and is returned solely for the local display; never log or serve it. */
esp_err_t espwifi_provisioning_start(char *password, size_t capacity);
bool espwifi_provisioning_active(void);
uint32_t espwifi_provisioning_remaining_seconds(void);
void espwifi_provisioning_stop(void);
/* Accepted socket must terminate on the AP's local IPv4 address during the
 * physical window. STA/LAN, unspecified, IPv6 and ambiguous addresses deny. */
bool espwifi_provisioning_request_allowed(int socket_fd);

/* Saves station credentials to NVS and applies them without reconnecting.
 * ssid: 1-32 bytes; password: blank (open network, or the saved password when
 * the SSID is unchanged) or 8-63 bytes. Returns ESP_ERR_INVALID_ARG on bad
 * input, otherwise the NVS/Wi-Fi error. */
esp_err_t espwifi_store_station(const char *ssid, const char *password);
/* Drops the current station link and joins the configured network. */
void espwifi_reconnect(void);


#ifdef __cplusplus
}
#endif

#endif