#include "speech_stt.hpp"
#include "speech_stt_protocol.hpp"
#include "bounded_https.hpp"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <memory>

namespace {
std::atomic<bool> cancelled{false};
struct HttpClient {
    esp_http_client_handle_t handle = nullptr;
    ~HttpClient()
    {
        if (handle) esp_http_client_cleanup(handle);
    }
};

bool write_all(esp_http_client_handle_t client, BoundedHttps &transport,
               const void *data, size_t size, std::string &error)
{
    const char *bytes = static_cast<const char *>(data);
    size_t sent = 0;
    while (sent < size) {
        if (transport.stopped()) { error = transport.stop_reason(); return false; }
        const int count = static_cast<int>(std::min<size_t>(size - sent, 8192));
        const int written = esp_http_client_write(client, bytes + sent, count);
        if (written <= 0 && (errno == EAGAIN || written == -ESP_ERR_HTTP_EAGAIN)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (written <= 0 || written > count) {
            error = transport.stopped() ? transport.stop_reason() :
                "STT upload failed; check Wi-Fi and the transcription endpoint, then retry.";
            return false;
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}

bool response_failure(int status, const char *detail, std::string &text,
                      std::string &error)
{
    if (status < 200 || status >= 300) {
        speech_stt_protocol::parse_response(nullptr, 0, status, text, error);
        error += " ";
        error += detail;
    } else {
        error = detail;
    }
    return false;
}
} // namespace

void speech_stt_prepare() { cancelled.store(false); }
void speech_stt_cancel() { cancelled.store(true); }

bool speech_stt_transcribe(const uint8_t *pcm, size_t pcm_size,
                           const char *url, const char *api_key, const char *model,
                           std::string &text, std::string &error)
{
    using namespace speech_stt_protocol;
    text.clear();
    error.clear();
    BoundedHttps transport(cancelled);
    if (!validate_settings(url, api_key, model, error)) return false;
    if (!pcm) {
        error = "STT recording buffer is missing; record speech before transcribing.";
        return false;
    }

    // A fixed alphabet and 128 random bits keep the boundary safe in headers
    // and independent of the model, PCM, and credentials.
    char boundary[64];
    std::snprintf(boundary, sizeof(boundary), "unravel-%08lx%08lx%08lx%08lx",
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()),
                  static_cast<unsigned long>(esp_random()));
    uint8_t wav[wav_header_bytes];
    std::string prefix;
    std::string suffix;
    if (!make_multipart(model, boundary, pcm_size,
                        prefix, wav, suffix, error)) return false;
    const size_t trailing_bytes = sizeof(wav) + pcm_size + suffix.size();
    if (prefix.size() > static_cast<size_t>(INT_MAX) - trailing_bytes) {
        error = "STT multipart request is too large; check the model setting.";
        return false;
    }
    const int request_bytes = static_cast<int>(prefix.size() + trailing_bytes);

    // IDF's HTTP_CLIENT debug log prints full Authorization headers. Never
    // allow its debug/verbose logging for a credential-bearing request.
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.timeout_ms = 100;
    config.is_async = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = false;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    if (!transport.initialize(url, error)) return false;
    config.transport = transport.transport();
    HttpClient client;
    client.handle = esp_http_client_init(&config);
    if (!client.handle) {
        error = "STT HTTPS client initialization failed; check the URL, TLS support, and free heap.";
        return false;
    }
    const std::string authorization = std::string("Bearer ") + api_key;
    const std::string content_type = std::string("multipart/form-data; boundary=") + boundary;
    if (esp_http_client_set_header(client.handle, "Authorization", authorization.c_str()) != ESP_OK ||
        esp_http_client_set_header(client.handle, "Content-Type", content_type.c_str()) != ESP_OK ||
        esp_http_client_set_header(client.handle, "Accept", "application/json") != ESP_OK ||
        esp_http_client_set_header(client.handle, "Accept-Encoding", "identity") != ESP_OK) {
        error = "STT request headers could not be allocated; check free heap.";
        return false;
    }
    esp_err_t opened;
    do {
        if (transport.stopped()) { error = transport.stop_reason(); return false; }
        opened = esp_http_client_open(client.handle, request_bytes);
        if (opened == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (opened == ESP_ERR_HTTP_EAGAIN);
    if (opened != ESP_OK) {
        if (transport.stopped()) { error = transport.stop_reason(); return false; }
        error = std::string("STT HTTPS connection failed (") + esp_err_to_name(opened) +
                "); check Wi-Fi, device clock, endpoint hostname, and its trusted TLS certificate.";
        return false;
    }
    if (!write_all(client.handle, transport, prefix.data(), prefix.size(), error) ||
        !write_all(client.handle, transport, wav, sizeof(wav), error) ||
        !write_all(client.handle, transport, pcm, pcm_size, error) ||
        !write_all(client.handle, transport, suffix.data(), suffix.size(), error)) return false;

    int64_t content_length;
    do {
        if (transport.stopped()) { error = transport.stop_reason(); return false; }
        content_length = esp_http_client_fetch_headers(client.handle);
        if (content_length == -ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (content_length == -ESP_ERR_HTTP_EAGAIN);
    if (content_length < 0) {
        error = transport.stopped() ? transport.stop_reason() :
            "STT response headers failed; check the transcription service and Wi-Fi.";
        return false;
    }
    const int status = esp_http_client_get_status_code(client.handle);
    if (content_length > static_cast<int64_t>(max_response_bytes)) {
        return response_failure(status, "STT response exceeds the 16384-byte limit; no transcript accepted.", text, error);
    }
    // Only a bounded response is allocated. The 320 kB input remains borrowed;
    // the upload consists of framing, a 44-byte WAV header, and the original PCM.
    std::unique_ptr<char, decltype(&heap_caps_free)> response(
        static_cast<char *>(heap_caps_malloc(max_response_bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!response) {
        error = "STT response buffer allocation failed; at least 16385 bytes of free PSRAM are required.";
        return false;
    }
    size_t received = 0;
    for (;;) {
        if (transport.stopped()) { error = transport.stop_reason(); return false; }
        // Always read through EOF, even if IDF reports completion: bytes may
        // still be cached from fetch_headers. One extra byte detects overflow
        // in chunked or close-delimited responses at the exact size limit.
        const int capacity = static_cast<int>(std::min<size_t>(1024, max_response_bytes + 1 - received));
        const int count = esp_http_client_read(client.handle, response.get() + received, capacity);
        if (count == -ESP_ERR_HTTP_EAGAIN) continue;
        if (count < 0) {
            if (transport.stopped()) { error = transport.stop_reason(); return false; }
            return response_failure(status, "STT response read failed or timed out; no partial transcript accepted.", text, error);
        }
        if (count == 0) {
            if (!esp_http_client_is_complete_data_received(client.handle) ||
                (content_length > 0 && received != static_cast<size_t>(content_length))) {
                return response_failure(status, "STT response was truncated; no partial transcript accepted.", text, error);
            }
            break;
        }
        received += static_cast<size_t>(count);
        if (received > max_response_bytes) {
            return response_failure(status, "STT response exceeds the 16384-byte limit; no transcript accepted.", text, error);
        }
    }
    response.get()[received] = '\0';
    if (transport.stopped()) { error = transport.stop_reason(); return false; }
    const bool accepted = parse_response(response.get(), received, status, text, error);
    if (transport.stopped()) { text.clear(); error = transport.stop_reason(); return false; }
    return accepted;
}
