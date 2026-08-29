#include <stdio.h>
#include "esp_wifi_bsp.h"
#include "esp_wifi.h"  // WIFI
#include "esp_event.h" // Events
#include "nvs_flash.h" // NVS storage
#include "nvs.h"       // NVS API
#include "esp_log.h"
#include "esp_err.h"
#include "string.h"

EventGroupHandle_t wifi_even_ = NULL;

esp_bsp_t user_esp_bsp;

static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

static const char *TAG = "wifi_bsp";

static esp_event_handler_instance_t wifi_handler_instance = NULL;
static esp_event_handler_instance_t ip_handler_instance = NULL;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void example_scan_wifi_task(void *arg);
void espwifi_init(void)
{
  memset(&user_esp_bsp,0,sizeof(esp_bsp_t));
  if (wifi_even_ == NULL) {
    wifi_even_ = xEventGroupCreate();
  } else {
    xEventGroupClearBits(wifi_even_, 0xFFFFFFFF);
  }
  esp_err_t ret = nvs_flash_init();                    // Initialize default NVS storage
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(esp_netif_init());                    // Initialize TCP/IP stack
  ESP_ERROR_CHECK(esp_event_loop_create_default());     // Create default event loop

  // Create both AP and STA network interfaces
  ap_netif = esp_netif_create_default_wifi_ap();        // Create AP interface
  sta_netif = esp_netif_create_default_wifi_sta();      // Create STA interface

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();  // Default configuration
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));                 // Initialize WiFi
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &wifi_handler_instance));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &ip_handler_instance));

  // Configure AP mode
  wifi_config_t ap_config = {
      .ap = {
        .ssid = "Unravel-Agent",
        .ssid_len = strlen("Unravel-Agent"),
        .channel = 1,
        .password = "unravel123",
        .max_connection = 4,
        .authmode = WIFI_AUTH_WPA2_PSK,
        .pmf_cfg = {
          .required = false,
        },
      },
  };

  // Configure STA mode - try to load from NVS first
  wifi_config_t sta_config = {0};

  // Try to load WiFi credentials from NVS
  nvs_handle_t nvs_handle;
  esp_err_t nvs_err = nvs_open("wifi", NVS_READONLY, &nvs_handle);
  if (nvs_err == ESP_OK) {
    size_t ssid_len = sizeof(sta_config.sta.ssid);
    size_t pass_len = sizeof(sta_config.sta.password);

    esp_err_t ssid_err = nvs_get_str(nvs_handle, "ssid", (char*)sta_config.sta.ssid, &ssid_len);
    esp_err_t pass_err = nvs_get_str(nvs_handle, "password", (char*)sta_config.sta.password, &pass_len);

    nvs_close(nvs_handle);

    if (ssid_err == ESP_OK && pass_err == ESP_OK) {
      ESP_LOGI(TAG, "Loaded WiFi credentials from NVS - SSID: %s", sta_config.sta.ssid);
    } else {
      // Use default credentials if NVS read fails
      strcpy((char*)sta_config.sta.ssid, "NasHome");
      strcpy((char*)sta_config.sta.password, "");
      ESP_LOGI(TAG, "Using default WiFi credentials");
    }
  } else {
    // Use default credentials if NVS is not available
    strcpy((char*)sta_config.sta.ssid, "NasHome");
    strcpy((char*)sta_config.sta.password, "");
    ESP_LOGI(TAG, "Using default WiFi credentials (NVS not available)");
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));              // Set mode to AP+STA
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));    // Configure AP
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));  // Configure STA
  ESP_ERROR_CHECK(esp_wifi_start());                                // Start WiFi

  // Only try to connect if SSID is not empty
  if (strlen((char*)sta_config.sta.ssid) > 0) {
    ESP_ERROR_CHECK(esp_wifi_connect());                            // Connect to AP as STA
  }

  ESP_LOGI(TAG, "WiFi AP started - SSID: Unravel-Agent, Password: unravel123");

  xTaskCreatePinnedToCore(example_scan_wifi_task, "example_scan_wifi_task", 2000, NULL, 2, NULL,0);   
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
  {
    xEventGroupSetBits(wifi_even_, WIFI_STARTED_BIT);
    ESP_LOGI(TAG, "Wi-Fi STA start");
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START)
  {
    ESP_LOGI(TAG, "Wi-Fi AP started");
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED)
  {
    wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
    ESP_LOGI(TAG, "Station joined, AID=%d", event->aid);
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED)
  {
    wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
    ESP_LOGI(TAG, "Station left, AID=%d", event->aid);
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
  {
    ESP_LOGI(TAG, "Wi-Fi STA connected");
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
    ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d", disc ? disc->reason : -1);
    xEventGroupClearBits(wifi_even_, WIFI_CONNECTED_BIT);
    /* Retry connection so the chat feature stays online. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    char ip[25];
    uint32_t pxip = event->ip_info.ip.addr;
    sprintf(ip, "%d.%d.%d.%d", (uint8_t)(pxip), (uint8_t)(pxip >> 8), (uint8_t)(pxip >> 16), (uint8_t)(pxip >> 24));
    ESP_LOGI(TAG,"Got STA IP %s",ip);
    xEventGroupSetBits(wifi_even_, WIFI_CONNECTED_BIT);
  }
}

void espwifi_deinit(void)
{
  if (wifi_handler_instance) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_instance));
    wifi_handler_instance = NULL;
  }
  if (ip_handler_instance) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_instance));
    ip_handler_instance = NULL;
  }
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_deinit());
  if (ap_netif) {
    esp_netif_destroy_default_wifi(ap_netif);
    ap_netif = NULL;
  }
  if (sta_netif) {
    esp_netif_destroy_default_wifi(sta_netif);
    sta_netif = NULL;
  }
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_loop_delete_default());
  if (wifi_even_) {
    xEventGroupClearBits(wifi_even_, WIFI_STARTED_BIT | WIFI_CONNECTED_BIT | WIFI_SCAN_DONE_BIT);
  }
  //esp_netif_deinit();
  //nvs_flash_deinit();
}



static void example_scan_wifi_task(void *arg)
{
  uint16_t rec = 0;
  EventBits_t even = xEventGroupWaitBits(wifi_even_, WIFI_STARTED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));
  if( even & WIFI_STARTED_BIT )
  {
    esp_err_t err = esp_wifi_scan_start(NULL,true);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
    } else {
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&rec));
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_clear_ap_list());
    }
  }
  user_esp_bsp.apNum = rec;
  xEventGroupSetBits(wifi_even_, WIFI_SCAN_DONE_BIT);
  vTaskDelete(NULL);
}

