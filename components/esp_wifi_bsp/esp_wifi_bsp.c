#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "esp_wifi_bsp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

EventGroupHandle_t wifi_even_ = NULL;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static const char *TAG = "wifi_bsp";
static esp_event_handler_instance_t wifi_handler_instance = NULL;
static esp_event_handler_instance_t ip_handler_instance = NULL;
/* Retained across deinit so concurrent callers never wait on a deleted lock. */
static SemaphoreHandle_t wifi_lock = NULL;
static SemaphoreHandle_t provisioning_done = NULL;
static TaskHandle_t provisioning_task = NULL;
static bool initialized = false;
static bool shutting_down = false;
static bool station_configured = false;
static bool provisioning_station_requested = false;
static int64_t provisioning_deadline_us = 0;
static int64_t provisioning_success_deadline_us = 0;
/* Protected by wifi_lock; retained across stops, failures and reinitialization. */
static uint32_t provisioning_previous_code = UINT32_MAX;
#define PROVISIONING_SUCCESS_BIT (1U << 0)
#define PROVISIONING_STOP_BIT (1U << 1)

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

static bool provisioning_active_locked(void)
{
  return initialized && !shutting_down &&
      provisioning_success_deadline_us != provisioning_deadline_us &&
      provisioning_deadline_us > esp_timer_get_time();
}

/* Called by ordinary tasks only, never a FreeRTOS timer/ISR or the lwIP thread. */
static void provisioning_stop_locked(bool reconnect)
{
  provisioning_deadline_us = 0; /* Deny HTTP before changing the interface. */
  provisioning_success_deadline_us = 0;
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
  provisioning_station_requested = false;
  if (reconnect && station_configured && !(xEventGroupGetBits(wifi_even_) & WIFI_CONNECTED_BIT))
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
}

static void provisioning_worker(void *arg)
{
  (void)arg;
  for (;;) {
    uint32_t events = 0;
    xTaskNotifyWait(0, UINT32_MAX, &events, pdMS_TO_TICKS(100));
    xSemaphoreTake(wifi_lock, portMAX_DELAY);
    const bool exit = shutting_down || (events & PROVISIONING_STOP_BIT);
    if (!exit && provisioning_deadline_us &&
        ((provisioning_success_deadline_us == provisioning_deadline_us) || !provisioning_active_locked())) {
      provisioning_stop_locked(true);
    }
    xSemaphoreGive(wifi_lock);
    if (exit) break;
  }
  xSemaphoreGive(provisioning_done);
  vTaskDelete(NULL);
}
void espwifi_init(void)
{
  if (!wifi_lock) wifi_lock = xSemaphoreCreateMutex();
  if (!provisioning_done) provisioning_done = xSemaphoreCreateBinary();
  assert(wifi_lock && provisioning_done);
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  if (initialized || shutting_down) {
    xSemaphoreGive(wifi_lock);
    return;
  }
  if (!wifi_even_) wifi_even_ = xEventGroupCreate();
  assert(wifi_even_);
  xEventGroupClearBits(wifi_even_, WIFI_STARTED_BIT | WIFI_CONNECTED_BIT);
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ap_netif = esp_netif_create_default_wifi_ap();
  sta_netif = esp_netif_create_default_wifi_sta();
  assert(ap_netif && sta_netif);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  /* Only our explicit station record persists; AP keys never enter NVS. */
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &wifi_handler_instance));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &ip_handler_instance));

  wifi_config_t sta_config = {0};
  nvs_handle_t handle;
  if (nvs_open("wifi", NVS_READONLY, &handle) == ESP_OK) {
    char ssid[33] = {0};
    char password[65] = {0};
    size_t ssid_len = sizeof(ssid), password_len = sizeof(password);
    esp_err_t ssid_err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    esp_err_t password_err = nvs_get_str(handle, "password", password, &password_len);
    nvs_close(handle);
    if (ssid_err == ESP_OK && password_err == ESP_OK &&
        ssid_len > 1 && ssid_len <= sizeof(ssid) &&
        password_len >= 1 && password_len <= 64 &&
        (password_len == 1 || password_len >= 9)) {
      memcpy(sta_config.sta.ssid, ssid, ssid_len - 1);
      memcpy(sta_config.sta.password, password, password_len - 1);
    }
    memset(password, 0, sizeof(password));
  }
  station_configured = sta_config.sta.ssid[0] != '\0';
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  memset(&sta_config, 0, sizeof(sta_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  initialized = true;
  provisioning_deadline_us = 0;
  const BaseType_t task_created = xTaskCreate(provisioning_worker, "wifi_setup", 3072, NULL, 4, &provisioning_task);
  ESP_ERROR_CHECK(task_created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
  if (station_configured) ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
  else ESP_LOGI(TAG, "Wi-Fi unconfigured; open Settings > Device > Wi-Fi on the display");
  xSemaphoreGive(wifi_lock);
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  (void)arg;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    xEventGroupSetBits(wifi_even_, WIFI_STARTED_BIT);
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t *disc = event_data;
    ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%d", disc ? disc->reason : -1);
    xEventGroupClearBits(wifi_even_, WIFI_CONNECTED_BIT);
    xSemaphoreTake(wifi_lock, portMAX_DELAY);
    if (initialized && !shutting_down && station_configured)
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    xSemaphoreGive(wifi_lock);
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(wifi_even_, WIFI_CONNECTED_BIT);
    xSemaphoreTake(wifi_lock, portMAX_DELAY);
    if (initialized && !shutting_down && provisioning_task && provisioning_active_locked() &&
        provisioning_station_requested) {
      provisioning_success_deadline_us = provisioning_deadline_us;
      xTaskNotify(provisioning_task, PROVISIONING_SUCCESS_BIT, eSetBits);
    }
    xSemaphoreGive(wifi_lock);
  }
}

esp_err_t espwifi_provisioning_start(char *password, size_t capacity)
{
  if (!password || capacity < ESPWIFI_PROVISIONING_PASSWORD_CAPACITY) return ESP_ERR_INVALID_ARG;
  password[0] = '\0';
  if (!wifi_lock) return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  if (!initialized || shutting_down) {
    xSemaphoreGive(wifi_lock);
    return ESP_ERR_INVALID_STATE;
  }
  /* Refresh disconnects old AP peers and rotates the key, not just the clock. */
  provisioning_stop_locked(false);
  wifi_config_t config = {0};
  memcpy(config.ap.ssid, ESPWIFI_PROVISIONING_SSID, sizeof(ESPWIFI_PROVISIONING_SSID) - 1);
  config.ap.ssid_len = sizeof(ESPWIFI_PROVISIONING_SSID) - 1;
  config.ap.channel = 1;
  config.ap.max_connection = 4;
  config.ap.authmode = WIFI_AUTH_WPA2_PSK;
  uint32_t code;
  for (;;) {
    const uint32_t random = esp_random(); /* Running Wi-Fi supplies RF entropy. */
    /* 42 complete decimal ranges fit in uint32_t; reject the biased tail. */
    if (random >= UINT32_C(4200000000)) continue;
    code = random % UINT32_C(100000000);
    if (code != provisioning_previous_code) break;
  }
  snprintf((char *)config.ap.password, ESPWIFI_PROVISIONING_PASSWORD_CAPACITY,
      "%08lu", (unsigned long)code);
  /* Configure while stopped: no stale/open AP is advertised during setup. */
  esp_err_t err = esp_wifi_stop();
  xEventGroupClearBits(wifi_even_, WIFI_CONNECTED_BIT);
  if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &config);
  if (err == ESP_OK) err = esp_wifi_start();
  if (err == ESP_OK) {
    provisioning_deadline_us = esp_timer_get_time() + ESPWIFI_PROVISIONING_SECONDS * INT64_C(1000000);
    provisioning_previous_code = code;
    memcpy(password, config.ap.password, ESPWIFI_PROVISIONING_PASSWORD_CAPACITY);
    // Preserve provider connectivity for the separate local-window text session.
    // Rejoining a saved network is not onboarding success and must not close AP.
    if (station_configured) ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
  } else {
    provisioning_stop_locked(false);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_start());
    if (station_configured) ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
  }
  memset(&config, 0, sizeof(config));
  xSemaphoreGive(wifi_lock);
  return err;
}

uint32_t espwifi_provisioning_remaining_seconds(void)
{
  if (!wifi_lock) return 0;
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  const int64_t remaining = provisioning_active_locked() ?
      provisioning_deadline_us - esp_timer_get_time() : 0;
  xSemaphoreGive(wifi_lock);
  return remaining > 0 ? (uint32_t)((remaining + 999999) / 1000000) : 0;
}

bool espwifi_provisioning_active(void)
{
  return espwifi_provisioning_remaining_seconds() != 0;
}

void espwifi_provisioning_stop(void)
{
  if (!wifi_lock) return;
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  if (initialized && !shutting_down) provisioning_stop_locked(true);
  xSemaphoreGive(wifi_lock);
}

bool espwifi_provisioning_request_allowed(int socket_fd)
{
  if (!wifi_lock || socket_fd < 0) return false;
  union {
    struct sockaddr sa;
    struct sockaddr_in v4;
#if LWIP_IPV6
    struct sockaddr_in6 v6;
#endif
  } local = {0};
  socklen_t length = sizeof(local);
  if (getsockname(socket_fd, &local.sa, &length) != 0) return false;
  uint32_t local_ipv4;
  if (local.sa.sa_family == AF_INET && length == sizeof(local.v4)) {
    local_ipv4 = local.v4.sin_addr.s_addr;
#if LWIP_IPV6
  } else if (local.sa.sa_family == AF_INET6 && length == sizeof(local.v6) &&
      IN6_IS_ADDR_V4MAPPED(&local.v6.sin6_addr)) {
    /* HTTPD's dual-stack listener reports IPv4 AP connections as ::ffff:a.b.c.d. */
    memcpy(&local_ipv4, &local.v6.sin6_addr.s6_addr[12], sizeof(local_ipv4));
#endif
  } else {
    return false; /* Native IPv6 does not establish access through the IPv4 AP. */
  }
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  esp_netif_ip_info_t ap_ip = {0}, sta_ip = {0};
  bool allowed = provisioning_active_locked() && ap_netif &&
      esp_netif_get_ip_info(ap_netif, &ap_ip) == ESP_OK &&
      ap_ip.ip.addr != 0 && local_ipv4 == ap_ip.ip.addr;
  /* Ambiguous equal interface addresses must not admit STA/LAN clients. */
  if (allowed && sta_netif && esp_netif_get_ip_info(sta_netif, &sta_ip) == ESP_OK &&
      sta_ip.ip.addr == ap_ip.ip.addr) allowed = false;
  allowed = allowed && provisioning_active_locked();
  xSemaphoreGive(wifi_lock);
  return allowed;
}

esp_err_t espwifi_store_station(const char *ssid, const char *password)
{
  if (!ssid || !password) return ESP_ERR_INVALID_ARG;
  const size_t ssid_len = strlen(ssid);
  const size_t password_len = strlen(password);
  if (ssid_len == 0 || ssid_len > 32 || password_len > 63 || (password_len > 0 && password_len < 8)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (!wifi_lock) return ESP_ERR_INVALID_STATE;
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  if (!initialized || shutting_down) {
    xSemaphoreGive(wifi_lock);
    return ESP_ERR_INVALID_STATE;
  }
  wifi_config_t previous = {0};
  esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &previous);
  if (err != ESP_OK) {
    memset(&previous, 0, sizeof(previous));
    xSemaphoreGive(wifi_lock);
    return err;
  }
  wifi_config_t config = {0};
  memcpy(config.sta.ssid, ssid, ssid_len);
  if (password_len > 0) {
    memcpy(config.sta.password, password, password_len);
  } else if (ssid_len == strnlen((const char *)previous.sta.ssid, sizeof(previous.sta.ssid)) &&
             memcmp(previous.sta.ssid, config.sta.ssid, ssid_len) == 0) {
    /* A blank password for the unchanged network keeps the saved one. */
    memcpy(config.sta.password, previous.sta.password, sizeof(config.sta.password));
  }
  /* NVS strings need a terminator even for a full-length 32-byte SSID. */
  char ssid_string[33] = {0};
  memcpy(ssid_string, config.sta.ssid, ssid_len);
  char password_string[65] = {0};
  memcpy(password_string, config.sta.password, sizeof(config.sta.password));
  nvs_handle_t handle;
  err = nvs_open("wifi", NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_str(handle, "ssid", ssid_string);
    if (err == ESP_OK) err = nvs_set_str(handle, "password", password_string);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }
  memset(password_string, 0, sizeof(password_string));
  if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &config);
  if (err == ESP_OK) station_configured = true;
  memset(&config, 0, sizeof(config));
  memset(&previous, 0, sizeof(previous));
  xSemaphoreGive(wifi_lock);
  return err;
}

void espwifi_reconnect(void)
{
  if (!wifi_lock) return;
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  if (initialized && !shutting_down && station_configured) {
    provisioning_station_requested = true;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
  }
  xSemaphoreGive(wifi_lock);
}

void espwifi_deinit(void)
{
  if (!wifi_lock) return;
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  if (!initialized || shutting_down) {
    xSemaphoreGive(wifi_lock);
    return;
  }
  provisioning_stop_locked(false);
  shutting_down = true;
  xTaskNotify(provisioning_task, PROVISIONING_STOP_BIT, eSetBits);
  xSemaphoreGive(wifi_lock);
  xSemaphoreTake(provisioning_done, portMAX_DELAY);
  /* Handler unregister waits for callbacks; never hold their mutex here. */
  if (wifi_handler_instance) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_instance));
    wifi_handler_instance = NULL;
  }
  if (ip_handler_instance) {
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_handler_instance));
    ip_handler_instance = NULL;
  }
  xSemaphoreTake(wifi_lock, portMAX_DELAY);
  provisioning_task = NULL;
  initialized = false;
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
  xEventGroupClearBits(wifi_even_, WIFI_STARTED_BIT | WIFI_CONNECTED_BIT);
  station_configured = false;
  shutting_down = false;
  xSemaphoreGive(wifi_lock);
}




