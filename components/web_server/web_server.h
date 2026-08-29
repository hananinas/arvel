#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_http_server.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the web server
 *
 * Initializes and starts the HTTP server that serves the embedded
 * Svelte UI and provides REST API endpoints.
 *
 * @return httpd_handle_t Handle to the HTTP server, or NULL on failure
 */
httpd_handle_t web_server_start(void);

/**
 * @brief Stop the web server
 *
 * @param server Handle to the HTTP server to stop
 * @return esp_err_t ESP_OK on success, error code otherwise
 */
esp_err_t web_server_stop(httpd_handle_t server);

#ifdef __cplusplus
}
#endif

#endif // WEB_SERVER_H
