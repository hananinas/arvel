#include "web_server.h"
#include "WWWData.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "web_server";

// Forward declarations
static esp_err_t www_handler(httpd_req_t *req);
static esp_err_t api_system_status_handler(httpd_req_t *req);
static esp_err_t api_wifi_status_handler(httpd_req_t *req);
static esp_err_t api_features_handler(httpd_req_t *req);
static esp_err_t api_system_status_simple_handler(httpd_req_t *req);
static esp_err_t api_wifi_status_simple_handler(httpd_req_t *req);
static esp_err_t api_ap_status_handler(httpd_req_t *req);
static esp_err_t api_stub_handler(httpd_req_t *req);
static esp_err_t api_wifi_settings_get_handler(httpd_req_t *req);
static esp_err_t api_wifi_settings_post_handler(httpd_req_t *req);
static esp_err_t api_wifi_scan_handler(httpd_req_t *req);
static esp_err_t api_options_handler(httpd_req_t *req);
static esp_err_t api_restart_handler(httpd_req_t *req);
static esp_err_t api_factory_reset_handler(httpd_req_t *req);

/**
 * @brief Handler for serving embedded web files
 */
static esp_err_t www_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    // Redirect root to index.html
    if (strcmp(uri, "/") == 0) {
        uri = "/index.html";
    }

    ESP_LOGD(TAG, "Serving: %s", uri);

    // Find and serve the file
    for (int i = 0; i < WWW_DATA_COUNT; i++) {
        if (strcmp(uri, www_data[i].uri) == 0) {
            // Set content type
            httpd_resp_set_type(req, www_data[i].content_type);

            // Set gzip encoding header
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

            // Set cache control for static assets
            if (strstr(uri, "/_app/immutable/") != NULL) {
                // Immutable assets can be cached forever
                httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
            } else {
                // Other files get shorter cache
                httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
            }

            // Send the file
            esp_err_t ret = httpd_resp_send(req, (const char*)www_data[i].content, www_data[i].len);

            if (ret == ESP_OK) {
                ESP_LOGD(TAG, "Served %s (%zu bytes)", uri, www_data[i].len);
            } else {
                ESP_LOGE(TAG, "Failed to serve %s: %s", uri, esp_err_to_name(ret));
            }

            return ret;
        }
    }

    // File not found
    ESP_LOGW(TAG, "File not found: %s", uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

/**
 * @brief Handler for /rest/system/status API endpoint
 */
static esp_err_t api_system_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Get system information
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // Add system information to JSON
    cJSON_AddStringToObject(root, "app_name", APP_NAME);
    cJSON_AddStringToObject(root, "app_version", APP_VERSION);
    cJSON_AddNumberToObject(root, "cpu_cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000); // Convert to seconds

    // Get chip model string
    const char *chip_model;
    switch(chip_info.model) {
        case CHIP_ESP32:   chip_model = "ESP32"; break;
        case CHIP_ESP32S2: chip_model = "ESP32-S2"; break;
        case CHIP_ESP32S3: chip_model = "ESP32-S3"; break;
        case CHIP_ESP32C3: chip_model = "ESP32-C3"; break;
        case CHIP_ESP32C2: chip_model = "ESP32-C2"; break;
        case CHIP_ESP32C6: chip_model = "ESP32-C6"; break;
        case CHIP_ESP32H2: chip_model = "ESP32-H2"; break;
        default:           chip_model = "Unknown"; break;
    }
    cJSON_AddStringToObject(root, "chip_model", chip_model);

    // Convert to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);

    free(json_str);
    return ret;
}

/**
 * @brief Handler for /rest/wifi/status API endpoint
 */
static esp_err_t api_wifi_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Get WiFi status
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    cJSON_AddNumberToObject(root, "mode", mode);

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddStringToObject(root, "ssid", (char*)ap_info.ssid);
            cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
            cJSON_AddBoolToObject(root, "connected", true);

            // Get IP address
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    cJSON_AddStringToObject(root, "ip", ip_str);
                }
            }
        } else {
            cJSON_AddBoolToObject(root, "connected", false);
        }
    }

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        wifi_config_t wifi_config;
        if (esp_wifi_get_config(WIFI_IF_AP, &wifi_config) == ESP_OK) {
            cJSON_AddStringToObject(root, "ap_ssid", (char*)wifi_config.ap.ssid);
            cJSON_AddNumberToObject(root, "ap_channel", wifi_config.ap.channel);

            // Get AP IP address
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    cJSON_AddStringToObject(root, "ap_ip", ip_str);
                }
            }
        }
    }

    // Convert to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);

    free(json_str);
    return ret;
}

/**
 * @brief Handler for /rest/features API endpoint
 */
static esp_err_t api_features_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Add firmware information
    cJSON_AddStringToObject(root, "firmware_name", APP_NAME);
    cJSON_AddStringToObject(root, "firmware_version", APP_VERSION);
    cJSON_AddStringToObject(root, "firmware_built_target", "esp32s3");

    // Add feature flags (all disabled for now, can be enabled as needed)
    cJSON_AddBoolToObject(root, "security", false);
    cJSON_AddBoolToObject(root, "mqtt", false);
    cJSON_AddBoolToObject(root, "ntp", false);
    cJSON_AddBoolToObject(root, "upload_firmware", false);
    cJSON_AddBoolToObject(root, "download_firmware", false);
    cJSON_AddBoolToObject(root, "sleep", false);
    cJSON_AddBoolToObject(root, "battery", false);
    cJSON_AddBoolToObject(root, "analytics", false);
    cJSON_AddBoolToObject(root, "coredump", false);
    cJSON_AddBoolToObject(root, "event_use_json", false);

    // Convert to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);

    free(json_str);
    return ret;
}

/**
 * @brief Handler for /rest/systemStatus API endpoint (simplified version)
 */
static esp_err_t api_system_status_simple_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "System status requested");

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Get chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // Add all system information
    cJSON_AddStringToObject(root, "esp_platform", "ESP32-S3");
    cJSON_AddStringToObject(root, "firmware_version", APP_VERSION);

    // Safely get heap information
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    size_t max_alloc = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    cJSON_AddNumberToObject(root, "free_heap", free_heap);
    cJSON_AddNumberToObject(root, "min_free_heap", min_free_heap);
    cJSON_AddNumberToObject(root, "max_alloc_heap", max_alloc);
    cJSON_AddNumberToObject(root, "cpu_freq_mhz", 240);
    cJSON_AddStringToObject(root, "cpu_type", "ESP32-S3");
    cJSON_AddNumberToObject(root, "cpu_rev", chip_info.revision);
    cJSON_AddNumberToObject(root, "cpu_cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "uptime", (uint32_t)(esp_timer_get_time() / 1000000));

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending system status: %zu bytes", strlen(json_str));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);

    ESP_LOGI(TAG, "System status sent successfully");
    return ret;
}

/**
 * @brief Handler for /rest/wifiStatus API endpoint (simplified version)
 */
static esp_err_t api_wifi_status_simple_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddNumberToObject(root, "status", 3); // WL_CONNECTED
            cJSON_AddStringToObject(root, "ssid", (char*)ap_info.ssid);
            cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);

            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    cJSON_AddStringToObject(root, "local_ip", ip_str);
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.gw));
                    cJSON_AddStringToObject(root, "gateway_ip", ip_str);
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.netmask));
                    cJSON_AddStringToObject(root, "subnet_mask", ip_str);
                }
            }
        } else {
            cJSON_AddNumberToObject(root, "status", 6); // WL_DISCONNECTED
        }
    } else {
        cJSON_AddNumberToObject(root, "status", 6); // WL_DISCONNECTED
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}

/**
 * @brief Handler for /rest/apStatus API endpoint
 */
static esp_err_t api_ap_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        cJSON_AddNumberToObject(root, "status", 1); // Active

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                cJSON_AddStringToObject(root, "ip_address", ip_str);
            }
        }

        wifi_sta_list_t sta_list;
        esp_wifi_ap_get_sta_list(&sta_list);
        cJSON_AddNumberToObject(root, "station_num", sta_list.num);
    } else {
        cJSON_AddNumberToObject(root, "status", 0); // Inactive
        cJSON_AddNumberToObject(root, "station_num", 0);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}

/**
 * @brief Generic stub handler for unimplemented endpoints
 */
static esp_err_t api_stub_handler(httpd_req_t *req)
{
    const char *stub_response = "{}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, stub_response);
}

/**
 * @brief Handler for GET /rest/wifiSettings
 */
static esp_err_t api_wifi_settings_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Get current WiFi configuration
    wifi_config_t wifi_config;
    esp_wifi_get_config(WIFI_IF_STA, &wifi_config);

    // Add basic WiFi settings
    cJSON_AddStringToObject(root, "hostname", "unravel-agent");
    cJSON_AddNumberToObject(root, "connection_mode", 1);

    // Create wifi_networks array
    cJSON *wifi_networks = cJSON_CreateArray();
    cJSON *network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "ssid", (char*)wifi_config.sta.ssid);
    cJSON_AddStringToObject(network, "password", ""); // Don't send password for security
    cJSON_AddBoolToObject(network, "static_ip_config", false);
    cJSON_AddItemToArray(wifi_networks, network);
    cJSON_AddItemToObject(root, "wifi_networks", wifi_networks);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}

/**
 * @brief Handler for POST /rest/wifiSettings
 */
static esp_err_t api_wifi_settings_post_handler(httpd_req_t *req)
{
    char *buf = NULL;
    size_t buf_len = req->content_len;

    if (buf_len > 4096) {  // Sanity check
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    buf = (char*)malloc(buf_len + 1);
    if (buf == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int ret = httpd_req_recv(req, buf, buf_len);
    if (ret <= 0) {
        free(buf);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[buf_len] = '\0';

    // Parse JSON
    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Extract WiFi settings
    cJSON *wifi_networks = cJSON_GetObjectItem(root, "wifi_networks");
    if (wifi_networks != NULL && cJSON_IsArray(wifi_networks)) {
        cJSON *network = cJSON_GetArrayItem(wifi_networks, 0);
        if (network != NULL) {
            cJSON *ssid_item = cJSON_GetObjectItem(network, "ssid");
            cJSON *password_item = cJSON_GetObjectItem(network, "password");

            if (ssid_item != NULL && cJSON_IsString(ssid_item)) {
                wifi_config_t wifi_config = {};

                // Copy SSID
                strncpy((char*)wifi_config.sta.ssid, ssid_item->valuestring, sizeof(wifi_config.sta.ssid) - 1);

                // Copy password if provided
                if (password_item != NULL && cJSON_IsString(password_item) && strlen(password_item->valuestring) > 0) {
                    strncpy((char*)wifi_config.sta.password, password_item->valuestring, sizeof(wifi_config.sta.password) - 1);
                }

                ESP_LOGI(TAG, "WiFi settings update requested - SSID: %s", wifi_config.sta.ssid);

                // Save to NVS for persistence
                nvs_handle_t nvs_handle;
                esp_err_t nvs_err = nvs_open("wifi", NVS_READWRITE, &nvs_handle);
                if (nvs_err == ESP_OK) {
                    nvs_set_str(nvs_handle, "ssid", (char*)wifi_config.sta.ssid);
                    nvs_set_str(nvs_handle, "password", (char*)wifi_config.sta.password);
                    nvs_commit(nvs_handle);
                    nvs_close(nvs_handle);
                    ESP_LOGI(TAG, "WiFi credentials saved to NVS");
                }

                // Set WiFi configuration
                esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
                cJSON_Delete(root);

                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(err));
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set WiFi config");
                    return ESP_FAIL;
                }

                // Disconnect and reconnect with new settings
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to ensure disconnect completes
                esp_wifi_connect();

                ESP_LOGI(TAG, "WiFi reconnecting to new network");
            }
        }
    }

    cJSON_Delete(root);

    // Send success response
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, "{\"status\":\"success\"}");
}

/**
 * @brief Handler for WiFi network scanning
 */
static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_list = NULL;

    // Start WiFi scan
    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_num(&ap_count);

    if (ap_count == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        return httpd_resp_sendstr(req, "{\"networks\":[]}");
    }

    ap_list = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    for (int i = 0; i < ap_count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char*)ap_list[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(network, "channel", ap_list[i].primary);
        cJSON_AddNumberToObject(network, "encryption", ap_list[i].authmode);
        cJSON_AddItemToArray(networks, network);
    }

    cJSON_AddItemToObject(root, "networks", networks);
    free(ap_list);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}

/**
 * @brief Handler for OPTIONS requests (CORS preflight)
 */
static esp_err_t api_options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type, Authorization");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "86400");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * @brief Handler for /rest/restart endpoint
 */
static esp_err_t api_restart_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Restart requested via API");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"status\":\"restarting\"}");

    // Schedule restart after response is sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/**
 * @brief Handler for /rest/factoryReset endpoint
 */
static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Factory reset requested via API");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, "{\"status\":\"resetting\"}");

    // Erase NVS and restart
    vTaskDelay(pdMS_TO_TICKS(1000));
    nvs_flash_erase();
    esp_restart();

    return ESP_OK;
}

/**
 * @brief Start the web server
 */
httpd_handle_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 64;  // Increased to handle all endpoints
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192; // Increase stack size for JSON operations
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    // Register API endpoints
    httpd_uri_t api_system_status = {
        .uri = "/rest/system/status",
        .method = HTTP_GET,
        .handler = api_system_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_system_status);

    httpd_uri_t api_wifi_status = {
        .uri = "/rest/wifi/status",
        .method = HTTP_GET,
        .handler = api_wifi_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_status);

    httpd_uri_t api_features = {
        .uri = "/rest/features",
        .method = HTTP_GET,
        .handler = api_features_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_features);

    httpd_uri_t api_features_options = {
        .uri = "/rest/features",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_features_options);

    // Register additional endpoints for Svelte UI compatibility
    httpd_uri_t api_system_status_simple = {
        .uri = "/rest/systemStatus",
        .method = HTTP_GET,
        .handler = api_system_status_simple_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_system_status_simple);

    httpd_uri_t api_system_status_options = {
        .uri = "/rest/systemStatus",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_system_status_options);

    httpd_uri_t api_wifi_status_simple = {
        .uri = "/rest/wifiStatus",
        .method = HTTP_GET,
        .handler = api_wifi_status_simple_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_status_simple);

    httpd_uri_t api_wifi_status_options = {
        .uri = "/rest/wifiStatus",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_status_options);

    httpd_uri_t api_ap_status = {
        .uri = "/rest/apStatus",
        .method = HTTP_GET,
        .handler = api_ap_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_ap_status);

    httpd_uri_t api_ap_status_options = {
        .uri = "/rest/apStatus",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_ap_status_options);

    // WiFi settings endpoints (GET and POST)
    httpd_uri_t api_wifi_settings_get = {
        .uri = "/rest/wifiSettings",
        .method = HTTP_GET,
        .handler = api_wifi_settings_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_settings_get);

    httpd_uri_t api_wifi_settings_post = {
        .uri = "/rest/wifiSettings",
        .method = HTTP_POST,
        .handler = api_wifi_settings_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_settings_post);

    // WiFi settings OPTIONS handler for CORS
    httpd_uri_t api_wifi_settings_options = {
        .uri = "/rest/wifiSettings",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_settings_options);

    // WiFi scan endpoints (both names for compatibility)
    httpd_uri_t api_wifi_scan = {
        .uri = "/rest/wifiScan",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_scan);

    httpd_uri_t api_wifi_scan_options = {
        .uri = "/rest/wifiScan",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_scan_options);

    httpd_uri_t api_list_networks = {
        .uri = "/rest/listNetworks",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_list_networks);

    httpd_uri_t api_list_networks_options = {
        .uri = "/rest/listNetworks",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_list_networks_options);

    httpd_uri_t api_scan_networks = {
        .uri = "/rest/scanNetworks",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_scan_networks);

    httpd_uri_t api_scan_networks_options = {
        .uri = "/rest/scanNetworks",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_scan_networks_options);

    httpd_uri_t api_ap_settings = {
        .uri = "/rest/apSettings",
        .method = HTTP_GET,
        .handler = api_stub_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_ap_settings);

    httpd_uri_t api_ap_settings_options = {
        .uri = "/rest/apSettings",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_ap_settings_options);

    httpd_uri_t api_light_state = {
        .uri = "/rest/lightState",
        .method = HTTP_GET,
        .handler = api_stub_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_light_state);

    httpd_uri_t api_light_state_options = {
        .uri = "/rest/lightState",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_light_state_options);

    // Restart endpoint
    httpd_uri_t api_restart = {
        .uri = "/rest/restart",
        .method = HTTP_POST,
        .handler = api_restart_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_restart);

    httpd_uri_t api_restart_options = {
        .uri = "/rest/restart",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_restart_options);

    // Factory reset endpoint
    httpd_uri_t api_factory_reset = {
        .uri = "/rest/factoryReset",
        .method = HTTP_POST,
        .handler = api_factory_reset_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_factory_reset);

    httpd_uri_t api_factory_reset_options = {
        .uri = "/rest/factoryReset",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_factory_reset_options);

    // Stub endpoints for features not yet implemented
    const char* stub_endpoints[] = {
        "/rest/ntpStatus",
        "/rest/ntpSettings",
        "/rest/mqttStatus",
        "/rest/mqttSettings",
        "/rest/brokerSettings",
        "/rest/securitySettings",
        "/rest/signIn",
        "/rest/verifyAuthorization",
        "/rest/uploadFirmware",
        "/rest/downloadUpdate",
        "/rest/coreDump",
        "/rest/sleep"
    };

    for (int i = 0; i < sizeof(stub_endpoints) / sizeof(stub_endpoints[0]); i++) {
        httpd_uri_t stub_uri = {
            .uri = stub_endpoints[i],
            .method = HTTP_GET,
            .handler = api_stub_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stub_uri);

        // Also register POST handler for these endpoints
        httpd_uri_t stub_uri_post = {
            .uri = stub_endpoints[i],
            .method = HTTP_POST,
            .handler = api_stub_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stub_uri_post);

        // And OPTIONS for CORS
        httpd_uri_t stub_uri_options = {
            .uri = stub_endpoints[i],
            .method = HTTP_OPTIONS,
            .handler = api_options_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stub_uri_options);
    }

    // Register wildcard handler for static files (must be last)
    httpd_uri_t uri_www = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = www_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_www);

    ESP_LOGI(TAG, "Web server started successfully");
    ESP_LOGI(TAG, "Serving %d static files", WWW_DATA_COUNT);
    ESP_LOGI(TAG, "Access the web interface at: http://<device-ip>/");

    return server;
}

/**
 * @brief Stop the web server
 */
esp_err_t web_server_stop(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Stopping web server");
    return httpd_stop(server);
}
