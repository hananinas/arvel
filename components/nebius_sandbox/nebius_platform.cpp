#include "nebius_sandbox.hpp"
#include "sdkconfig.h"

#if CONFIG_UNRAVEL_NEBIUS_SANDBOX
#include "bounded_https.hpp"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>

namespace {
constexpr char origin[] = "https://api.tokenfactory.nebius.com/sandboxes/v1";
constexpr size_t response_limit = 24 * 1024;
constexpr size_t request_limit = 32 * 1024;
constexpr uint64_t request_timeout_us = 8000000;

struct Production {
    const std::atomic<bool> *cancelled = nullptr;
};

bool header_value(const char *value, size_t limit) {
    const size_t length = strnlen(value, limit + 1);
    if (!length || length > limit) return false;
    for (size_t i = 0; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte < 0x21 || byte > 0x7e) return false;
    }
    return true;
}

bool alphanumeric(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9');
}

bool uuid(const char *value) {
    if (!value || strnlen(value, 37) != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else if (!((value[i] >= '0' && value[i] <= '9') ||
                     (value[i] >= 'a' && value[i] <= 'f') ||
                     (value[i] >= 'A' && value[i] <= 'F'))) return false;
    }
    return true;
}

bool image_source(const char *value) {
    if (!header_value(value, 256)) return false;
    if (uuid(value)) return true;
    if (std::strncmp(value, "tag:", 4) || !alphanumeric(value[4])) return false;
    // ImageSourceTag: ^tag:[A-Za-z0-9][A-Za-z0-9_-]*(?:[:/.][A-Za-z0-9_-]+)*$
    bool separator = false;
    for (const char *cursor = value + 5; *cursor; ++cursor) {
        if (alphanumeric(*cursor) || *cursor == '_' || *cursor == '-') {
            separator = false;
        } else if ((*cursor == ':' || *cursor == '/' || *cursor == '.') && !separator) {
            separator = true;
        } else return false;
    }
    return !separator;
}

bool call_cancelled(void *context) {
    const auto &call = *static_cast<const unravel::ToolCallContext *>(context);
    return call.cancelled && call.cancelled(call.generation, call.cancellation_context);
}

struct HttpCall {
    const char *method;
    const char *path;
    const char *body;
    std::string &response;
    int &status;
    const unravel::ToolCallContext &call;
    const Production &production;
    uint64_t deadline;
    bool success = false;
};

bool http(HttpCall &request) {
    const bool create = !std::strcmp(request.method, "POST") && !std::strcmp(request.path, "/instances");
    constexpr char operations[] = "/operations/";
    const bool operation = !std::strncmp(request.path, operations, sizeof(operations) - 1) &&
                           uuid(request.path + sizeof(operations) - 1);
    const bool get = operation && !std::strcmp(request.method, "GET");
    const bool remove = operation && !std::strcmp(request.method, "DELETE");
    if (!create && !get && !remove) return false;
    const size_t length = request.body ? strnlen(request.body, request_limit + 1) : 0;
    if (length > request_limit || (create ? !length : length != 0)) return false;

    BoundedHttps transport(*request.production.cancelled, 8000, request.deadline,
                           call_cancelled, const_cast<unravel::ToolCallContext *>(&request.call));
    if (transport.stopped()) return false;
    const std::string url = std::string(origin) + request.path;
    std::string error;
    if (!transport.initialize(url.c_str(), error)) return false;
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.timeout_ms = transport.wait_ms();
    config.is_async = true;
    config.transport = transport.transport();
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    config.method = create ? HTTP_METHOD_POST : get ? HTTP_METHOD_GET : HTTP_METHOD_DELETE;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    // Cleanup must finish before the borrowed BoundedHttps transport is destroyed.
    struct Client {
        esp_http_client_handle_t handle;
        ~Client() { if (handle) esp_http_client_cleanup(handle); }
    } client{esp_http_client_init(&config)};
    if (!client.handle) return false;
    if (esp_http_client_set_header(client.handle, "Authorization", "Bearer " CONFIG_UNRAVEL_NEBIUS_API_KEY) != ESP_OK ||
        esp_http_client_set_header(client.handle, "Project", CONFIG_UNRAVEL_NEBIUS_PROJECT_ID) != ESP_OK ||
        esp_http_client_set_header(client.handle, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_set_header(client.handle, "Accept-Encoding", "identity") != ESP_OK) return false;

    esp_err_t result;
    do {
        if (transport.stopped()) return false;
        esp_http_client_set_timeout_ms(client.handle, transport.wait_ms());
        result = esp_http_client_open(client.handle, static_cast<int>(length));
        if (result == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (result == ESP_ERR_HTTP_EAGAIN);
    size_t sent = 0;
    while (result == ESP_OK && sent < length && !transport.stopped()) {
        esp_http_client_set_timeout_ms(client.handle, transport.wait_ms());
        errno = 0;
        const int count = esp_http_client_write(client.handle, request.body + sent, static_cast<int>(length - sent));
        if (count <= 0 && errno == EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (count <= 0) result = ESP_FAIL;
        else sent += static_cast<size_t>(count);
    }
    int64_t content_length = -1;
    while (result == ESP_OK && !transport.stopped()) {
        esp_http_client_set_timeout_ms(client.handle, transport.wait_ms());
        content_length = esp_http_client_fetch_headers(client.handle);
        if (content_length == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (content_length < 0) result = ESP_FAIL;
        break;
    }
    if (result != ESP_OK || transport.stopped()) return false;
    request.status = esp_http_client_get_status_code(client.handle);
    const int expected_status = create ? 201 : get ? 200 : 202;
    // Never read or expose provider error bodies, redirects, or authentication challenges.
    if (request.status != expected_status) return true;
    // DELETE's 202 acknowledges cancellation, not operation completion; no body is required.
    if (remove) return true;
    if (content_length > static_cast<int64_t>(response_limit)) return false;
    std::unique_ptr<char, decltype(&std::free)> buffer(
        static_cast<char *>(heap_caps_malloc(response_limit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), std::free);
    if (!buffer) return false;
    size_t received = 0;
    while (received < response_limit + 1 && !transport.stopped()) {
        esp_http_client_set_timeout_ms(client.handle, transport.wait_ms());
        errno = 0;
        const int count = esp_http_client_read(client.handle, buffer.get() + received,
                                              static_cast<int>(response_limit + 1 - received));
        if (count < 0 && errno == EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (count < 0) return false;
        if (!count) {
            if (!esp_http_client_is_complete_data_received(client.handle) && errno == EAGAIN) {
                vTaskDelay(pdMS_TO_TICKS(10)); continue;
            }
            break;
        }
        received += static_cast<size_t>(count);
    }
    if (transport.stopped() || received > response_limit ||
        !esp_http_client_is_complete_data_received(client.handle)) return false;
    request.response.assign(buffer.get(), received);
    return true;
}

void *run(void *context) {
    auto &request = *static_cast<HttpCall *>(context);
    request.success = http(request);
    return nullptr;
}

bool request(const char *method, const char *path, const char *body, std::string &response,
             int &status, const unravel::ToolCallContext &call, void *context) {
    // Capture the deadline before configuration/thread creation, including scheduling time.
    uint64_t deadline = static_cast<uint64_t>(esp_timer_get_time()) + request_timeout_us;
    if (call.deadline_us && call.deadline_us < deadline) deadline = call.deadline_us;
    response.clear();
    status = 0;
    const auto &production = *static_cast<const Production *>(context);
    if (!method || !path || !production.cancelled || production.cancelled->load() ||
        call_cancelled(const_cast<unravel::ToolCallContext *>(&call)) ||
        static_cast<uint64_t>(esp_timer_get_time()) >= deadline) return false;
    HttpCall request{method, path, body, response, status, call, production, deadline};
    esp_pthread_cfg_t previous = esp_pthread_get_default_config();
    esp_pthread_get_cfg(&previous);
    auto config = previous;
    config.stack_size = 12 * 1024;
    config.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.thread_name = "nebius_io";
    config.inherit_cfg = false;
    if (esp_pthread_set_cfg(&config) != ESP_OK) return false;
    pthread_t thread;
    const int created = pthread_create(&thread, nullptr, run, &request);
    esp_pthread_set_cfg(&previous);
    if (created) return false;
    // All borrowed inputs and cancellation context remain alive until the worker exits.
    if (pthread_join(thread, nullptr)) std::abort();
    return request.success;
}
} // namespace
#endif

namespace nebius_sandbox {
bool register_tools(unravel::Agent &agent, const std::atomic<bool> &cancelled) {
#if CONFIG_UNRAVEL_NEBIUS_SANDBOX
    if (!header_value(CONFIG_UNRAVEL_NEBIUS_API_KEY, 4096) ||
        !header_value(CONFIG_UNRAVEL_NEBIUS_PROJECT_ID, 256) ||
        !image_source(CONFIG_UNRAVEL_NEBIUS_IMAGE)) return false;
    static Production production{&cancelled};
    if (production.cancelled != &cancelled) return false;
#if CONFIG_UNRAVEL_NEBIUS_ALLOW_NETWORK
    constexpr bool allow_network = true;
#else
    constexpr bool allow_network = false;
#endif
    static Tools tools(Config{CONFIG_UNRAVEL_NEBIUS_IMAGE, allow_network}, request, &production);
    return tools.register_tools(agent);
#else
    (void)agent;
    (void)cancelled;
    return true;
#endif
}
} // namespace nebius_sandbox
