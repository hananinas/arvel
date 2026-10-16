#pragma once
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_HTTP_EAGAIN = 0x7007;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;
constexpr esp_err_t ESP_ERR_NVS_NOT_ENOUGH_SPACE = 0x1105;
