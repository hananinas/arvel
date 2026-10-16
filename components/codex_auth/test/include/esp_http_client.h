#pragma once
#include "esp_err.h"
#include "bounded_https.hpp"
#include <cstdint>
struct HostHttp;
using esp_http_client_handle_t = HostHttp *;
constexpr int HTTP_METHOD_POST = 1, HTTP_TRANSPORT_OVER_SSL = 2;
struct esp_http_client_config_t {
    const char *url;
    int method, transport_type, timeout_ms;
    bool is_async;
    esp_err_t (*crt_bundle_attach)(void *);
    bool skip_cert_common_name_check, disable_auto_redirect;
    int max_authorization_retries, buffer_size, buffer_size_tx;
    esp_transport_handle_t transport;
};
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
