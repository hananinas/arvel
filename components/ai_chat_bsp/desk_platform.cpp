#include "desk_backend.hpp"
#include "agent_runtime.hpp"
#include "bounded_https.hpp"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <cerrno>
#include <cstring>
#include <ctime>
#include <memory>
#include <pthread.h>

namespace {
const std::atomic<bool> *cancel = nullptr;
void (*search_config)(std::string &, bool &, std::string &) = nullptr;
esp_timer_handle_t ticker = nullptr;
int64_t monotonic() { return esp_timer_get_time(); }
int64_t utc() { return time(nullptr); }
void *allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
struct Nvs { nvs_handle_t handle = 0; ~Nvs() { if (handle) nvs_close(handle); } };
int load(uint8_t *data, size_t *size) {
    Nvs nvs; esp_err_t result = nvs_open("desk", NVS_READONLY, &nvs.handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return 0;
    if (result != ESP_OK) return -1;
    result = nvs_get_blob(nvs.handle, "notes", data, size);
    return result == ESP_ERR_NVS_NOT_FOUND ? 0 : result == ESP_OK ? 1 : -1;
}
bool save(const uint8_t *data, size_t size) {
    Nvs nvs;
    return nvs_open("desk", NVS_READWRITE, &nvs.handle) == ESP_OK &&
        nvs_set_blob(nvs.handle, "notes", data, size) == ESP_OK && nvs_commit(nvs.handle) == ESP_OK;
}
bool call_cancelled(void *context) {
    const auto *call = static_cast<const unravel::ToolCallContext *>(context);
    return call && call->cancelled && call->cancelled(call->generation, call->cancellation_context);
}
struct Cancellation { const unravel::ToolCallContext *owner; };
bool execution_cancelled(uint64_t, void *context) {
    const auto *scope = static_cast<const Cancellation *>(context);
    const auto *owner = scope->owner;
    return cancel->load() || (owner && owner->cancelled &&
        owner->cancelled(owner->generation, owner->cancellation_context));
}
bool http(const char *url, const char *key, const char *body, size_t limit, uint32_t timeout,
          std::string &response, int &status, std::string &error, const unravel::ToolCallContext *call) {
    const bool tavily = !std::strcmp(url, "https://api.tavily.com/search");
    const bool parallel = !std::strcmp(url, "https://api.parallel.ai/v1/search");
    constexpr char geocode[] = "https://geocoding-api.open-meteo.com/v1/search?";
    constexpr char forecast[] = "https://api.open-meteo.com/v1/forecast?";
    if (!tavily && !parallel && std::strncmp(url, geocode, sizeof(geocode)-1) &&
        std::strncmp(url, forecast, sizeof(forecast)-1)) { error = "remote_endpoint_forbidden"; return false; }
    if ((tavily || parallel) != (key && *key) || timeout > 8000 || !timeout) { error = "invalid_remote_configuration"; return false; }
    BoundedHttps transport(*cancel, timeout, call ? call->deadline_us : 0,
                           call_cancelled, const_cast<unravel::ToolCallContext *>(call));
    const int64_t started = monotonic();
    if (!transport.initialize(url, error)) return false;
    esp_http_client_config_t config{};
    config.url = url; config.timeout_ms = transport.wait_ms(); config.is_async = true;
    config.transport = transport.transport(); config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.buffer_size = 2048; config.buffer_size_tx = 2048;
    config.method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    config.disable_auto_redirect = true; config.max_authorization_retries = -1;
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    struct Client { esp_http_client_handle_t handle; ~Client() { if (handle) esp_http_client_cleanup(handle); } } client{esp_http_client_init(&config)};
    if (!client.handle) { error = "remote_connection_out_of_memory"; return false; }
    std::string authorization;
    if (tavily) authorization = "Bearer " + std::string(key);
    if (esp_http_client_set_header(client.handle, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_set_header(client.handle, "Accept-Encoding", "identity") != ESP_OK ||
        (tavily && esp_http_client_set_header(client.handle, "Authorization", authorization.c_str()) != ESP_OK) ||
        (parallel && esp_http_client_set_header(client.handle, "x-api-key", key) != ESP_OK)) { error = "remote_headers_out_of_memory"; return false; }
    const size_t length = body ? std::strlen(body) : 0;
    esp_err_t result;
    do {
        if (transport.stopped()) {
            ESP_LOGW("desk_http", "%.40s stopped while connecting after %lld ms", url, static_cast<long long>((monotonic()-started)/1000));
            error = transport.stop_reason(); return false;
        }
        esp_http_client_set_timeout_ms(client.handle, transport.wait_ms());
        result = esp_http_client_open(client.handle, length);
        if (result == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (result == ESP_ERR_HTTP_EAGAIN);
    size_t sent = 0;
    while (result == ESP_OK && sent < length && !transport.stopped()) {
        esp_http_client_set_timeout_ms(client.handle, transport.wait_ms());
        int count = esp_http_client_write(client.handle, body+sent, length-sent);
        if (count <= 0 && errno == EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (count <= 0) result = ESP_FAIL; else sent += count;
    }
    int64_t content_length = -1;
    while (result == ESP_OK && !transport.stopped()) {
        esp_http_client_set_timeout_ms(client.handle, transport.wait_ms());
        content_length = esp_http_client_fetch_headers(client.handle);
        if (content_length == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (content_length < 0) result = ESP_FAIL;
        break;
    }
    status = esp_http_client_get_status_code(client.handle);
    if (result == ESP_OK && status != 200) return true; // Never echo provider error bodies or credentials.
    if (content_length > static_cast<int64_t>(limit)) { error = "remote_response_too_large"; return false; }
    std::unique_ptr<char, decltype(&std::free)> buffer(static_cast<char *>(allocate(limit+1)), std::free);
    if (!buffer) { error = "remote_response_out_of_memory"; return false; }
    size_t received = 0;
    while (result == ESP_OK && received < limit+1 && !transport.stopped()) {
        esp_http_client_set_timeout_ms(client.handle, transport.wait_ms());
        int count = esp_http_client_read(client.handle, buffer.get()+received, limit+1-received);
        if (count < 0 && errno == EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (count < 0) { result = ESP_FAIL; break; }
        if (!count) {
            if (!esp_http_client_is_complete_data_received(client.handle) && errno == EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
            break;
        }
        received += count;
    }
    const bool complete = esp_http_client_is_complete_data_received(client.handle);
    if (transport.stopped() || result != ESP_OK || received > limit || !complete)
        ESP_LOGW("desk_http", "%.40s failed after %lld ms: result=%d status=%d length=%lld received=%u complete=%d stopped=%d",
                 url, static_cast<long long>((monotonic()-started)/1000), result, status,
                 static_cast<long long>(content_length), static_cast<unsigned>(received), complete, transport.stopped());
    if (transport.stopped()) { error = transport.stop_reason(); return false; }
    if (result != ESP_OK || received > limit || !complete) {
        error = "remote_response_incomplete_or_too_large"; return false;
    }
    response.assign(buffer.get(), received); return true;
}
void tick(void *) { desk_backend_tick(); }
struct Call { const char *tool; const cJSON *arguments; cJSON **reply; const unravel::ToolCallContext *context; bool success = false; };
void *run(void *context) {
    auto &call = *static_cast<Call *>(context);
    if (cancel->load() || call_cancelled(const_cast<unravel::ToolCallContext *>(call.context))) {
        *call.reply = cJSON_CreateObject();
        if (*call.reply) { cJSON_AddBoolToObject(*call.reply, "ok", false); cJSON_AddStringToObject(*call.reply, "error", "request_cancelled"); }
    } else call.success = desk_backend_execute(call.tool, call.arguments, call.reply, call.context);
    return nullptr;
}
}
bool desk_platform_initialize(void (*configuration)(std::string &, bool &, std::string &), const std::atomic<bool> &cancelled, std::string &error) {
    search_config = configuration; cancel = &cancelled;
    DeskPlatform platform{monotonic, utc, allocate, load, save, search_config, http};
    if (!desk_backend_initialize(platform, error)) return false;
    esp_timer_create_args_t args{}; args.callback = tick; args.name = "desk_timer"; args.dispatch_method = ESP_TIMER_TASK; args.skip_unhandled_events = true;
    if (esp_timer_create(&args, &ticker) != ESP_OK || esp_timer_start_periodic(ticker, 250000) != ESP_OK) {
        error = "Timer dispatcher unavailable; native snapshots still update countdowns."; return false;
    }
    return true;
}
bool desk_platform_execute(const char *tool, const cJSON *arguments, cJSON **reply, const unravel::ToolCallContext *context) {
    if (!reply) return false;
    *reply = nullptr;
    // Set the remote budget before thread creation so scheduling cannot reset it.
    unravel::ToolCallContext bounded = context ? *context : unravel::ToolCallContext{};
    Cancellation cancellation{context};
    bounded.cancelled = execution_cancelled;
    bounded.cancellation_context = &cancellation;
    if (tool && (!std::strcmp(tool, "search_web") || !std::strcmp(tool, "get_weather"))) {
        const uint64_t deadline = static_cast<uint64_t>(monotonic()) + 8000000;
        if (!bounded.deadline_us || bounded.deadline_us > deadline) bounded.deadline_us = deadline;
    }
    Call call{tool, arguments, reply, &bounded};
    if (!desk_backend_needs_internal_stack(tool, arguments)) { run(&call); return call.success; }
    esp_pthread_cfg_t previous = esp_pthread_get_default_config(); esp_pthread_get_cfg(&previous);
    auto config = previous; config.stack_size = 12*1024; config.stack_alloc_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.thread_name = "desk_io"; config.inherit_cfg = false;
    if (esp_pthread_set_cfg(&config) != ESP_OK) return false;
    pthread_t thread;
    const int created = pthread_create(&thread, nullptr, run, &call);
    esp_pthread_set_cfg(&previous);
    if (created) return false;
    // Borrowed arguments/context cannot be released until this worker has exited.
    if (pthread_join(thread, nullptr)) abort();
    return call.success;
}
