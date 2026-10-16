#include "speech_native.hpp"
#include "speech_native_protocol.hpp"
#include "speech_live_protocol.hpp"
#include "bounded_https.hpp"
#include "user_audio_bsp.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha1.h"
#include "sdkconfig.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>
#include <strings.h>

#if CONFIG_ESP_TLS_INSECURE || !CONFIG_MBEDTLS_HAVE_TIME_DATE
#error "Native audio requires certificate and certificate-date verification"
#endif
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
#error "Native audio requires the finite-timeout new-I2S codec backend"
#endif

namespace {
namespace protocol = speech_native_protocol;
constexpr unsigned slots = 4, result_slots = 4, pending_slots = 8;
constexpr size_t config_limit = 32768;
constexpr const char *memory_error = "Not enough memory for native audio.";
constexpr const char *network_error = "Native audio connection failed. Check Wi-Fi and device clock, then reconnect.";
constexpr const char *auth_error = "Native audio authentication was rejected. Check the deployment key.";
constexpr const char *protocol_error = "Native audio server returned an unsupported or malformed protocol event.";
constexpr const char *pressure_error = "Native audio bounded queue filled. Conversation stopped; reconnect explicitly.";
constexpr const char *codec_error = "Native audio codec I/O failed. Conversation stopped.";
constexpr const char *end_error = "Native audio server ended the session. Reconnect explicitly.";

StaticSemaphore_t mutex_storage;
SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
struct Guard { Guard() { xSemaphoreTake(mutex, portMAX_DELAY); } ~Guard() { xSemaphoreGive(mutex); } };
struct Endpoint { char host[254]{}, authority[264]{}; int port = 443; };
bool bounded(const char *s, size_t maximum, size_t &length) {
    if (!s) return false;
    length = strnlen(s, maximum + 1);
    return length <= maximum && speech_live_protocol::utf8(s, length);
}
bool endpoint_parse(const char *url, Endpoint &out) {
    size_t length;
    if (!bounded(url, 300, length) || std::strncmp(url, "wss://", 6)) return false;
    const char *host = url + 6, *slash = std::strchr(host, '/');
    if (!slash || std::strcmp(slash, "/v1/realtime") || slash == host || size_t(slash - host) >= sizeof(out.authority)) return false;
    // DNS/IPv4 authorities only. Reject URL userinfo, encoded authorities, query,
    // fragments, whitespace and ambiguous IPv6 instead of guessing a TLS identity.
    const char *colon = nullptr;
    for (const char *p = host; p < slash; ++p) {
        if (*p == ':') { if (colon) return false; colon = p; }
        else if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
    }
    const size_t host_length = (colon ? colon : slash) - host;
    if (!host_length || host_length > 253 || host[0] == '.' || host[host_length - 1] == '.') return false;
    if (colon) {
        if (colon + 1 == slash) return false;
        unsigned port = 0;
        for (const char *p = colon + 1; p < slash; ++p) {
            if (*p < '0' || *p > '9' || port > 6553) return false;
            port = port * 10 + unsigned(*p - '0');
        }
        if (!port || port > 65535) return false;
        out.port = static_cast<int>(port);
    }
    std::memcpy(out.host, host, host_length);
    std::memcpy(out.authority, host, slash - host);
    return true;
}
bool valid_key(const char *key) {
    size_t length;
    if (!bounded(key, 4096, length) || !length) return false;
    for (size_t i = 0; i < length; ++i) if (static_cast<unsigned char>(key[i]) <= 32 || static_cast<unsigned char>(key[i]) >= 127) return false;
    return true;
}
void uuid(char (&out)[37]) {
    uint8_t b[16]; esp_fill_random(b, sizeof(b)); b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128;
    std::snprintf(out, sizeof(out), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}
struct Writer {
    char *data; size_t capacity, length = 0; bool ok = true;
    void raw(const char *s) { raw(s, std::strlen(s)); }
    void raw(const char *s, size_t n) {
        if (!ok || n >= capacity - length) { ok = false; return; }
        std::memcpy(data + length, s, n); length += n; data[length] = 0;
    }
    void quote(const char *s) {
        if (!ok) return;
        size_t written = 0;
        if (!protocol::escape_json(s, std::strlen(s), data + length, capacity - length, written)) { ok = false; return; }
        length += written;
    }
    void event(const char *type) {
        char id[37]; uuid(id); raw("{\"event_id\":"); quote(id); raw(",\"type\":"); quote(type);
    }
};
struct Packet { uint32_t epoch = 0; size_t samples = 0; int16_t pcm[1920]{}; };
struct Result { char session[129], call[129], text[8193]; };
struct Text {
    char item[129]{}, response[129]{}, text[8193]{};
    uint32_t turn = 0; bool assistant = false, final = false, interrupted = false;
};
struct Pending { char id[129]{}; bool queued = false; };
struct Working {
    protocol::Event event;
    protocol::JsonWorkspace second_workspace;
    protocol::CaptureConverter capture;
    protocol::PlaybackConverter playback;
    char incoming[32769]{}, outgoing[65537]{}, config[32769]{}, tools[protocol::max_tools_bytes + 1]{}, key[4097]{};
    uint8_t tls_in[2048]{}, tls_out[1294]{};
    int16_t capture_pcm[2560]{}, playback_pcm[2560]{}, decoded[12288]{};
    Packet tx[slots], rx[slots]; Result results[result_slots];
    Text text[8];
};
struct Session {
    Endpoint endpoint;
    Working *work = nullptr;
    speech_live_event_callback_t callback = nullptr; void *context = nullptr;
    std::atomic<bool> stop{false}, muted{false}, ready{false}, user_stop{false};
    std::atomic<bool> capture_done{true}, playback_done{true};
    std::atomic<uint32_t> capture_epoch{1}, playback_epoch{1};
    std::atomic<const char *> failure{nullptr};
    // Includes queued and dequeued packets until recycled; no dequeue/play gap.
    std::atomic<unsigned> playback_outstanding{0};
    QueueHandle_t tx = nullptr, tx_free = nullptr, rx = nullptr, rx_free = nullptr, result = nullptr, result_free = nullptr;
    StaticQueue_t queue_controls[6]; uint8_t queue_bytes[6][slots * sizeof(void *)]{};
    Pending pending[pending_slots];
    char provider_session[129]{}, response[129]{}, obsolete[129]{};
    uint32_t turn = 0; bool audio_done = true, response_done = true, speaking = false, waiting_response = false;
    const char *last_phase = nullptr;
    void fail(const char *message) {
        const char *none = nullptr; failure.compare_exchange_strong(none, message);
        muted.store(true); capture_epoch.fetch_add(1); playback_epoch.fetch_add(1); stop.store(true);
    }
    void emit(const char *json) { if (callback) callback(json, std::strlen(json), context); }
    void phase(const char *value) {
        if (last_phase && !std::strcmp(last_phase, value)) return;
        last_phase = value; Writer out{work->outgoing, sizeof(work->outgoing)};
        out.raw("{\"type\":\"phase\",\"phase\":"); out.quote(value); out.raw("}"); if (out.ok) emit(out.data);
    }
};
Session *live = nullptr;
void *psram(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void release(Session *s) {
    if (!s) return;
    QueueHandle_t queues[] = {s->tx, s->tx_free, s->rx, s->rx_free, s->result, s->result_free};
    for (auto q : queues) if (q) vQueueDelete(q);
    if (s->work) { mbedtls_platform_zeroize(s->work, sizeof(Working)); heap_caps_free(s->work); }
    s->~Session(); heap_caps_free(s);
}
void clear_audio(Session &s, bool capture) {
    Packet *p = nullptr;
    while (xQueueReceive(capture ? s.tx : s.rx, &p, 0) == pdTRUE) {
        xQueueSend(capture ? s.tx_free : s.rx_free, &p, 0);
        if (!capture) s.playback_outstanding.fetch_sub(1);
    }
}
void capture_task(void *argument) {
    auto &s = *static_cast<Session *>(argument); auto &w = *s.work;
    uint32_t previous_epoch = 0;
    while (!s.stop.load()) {
        const uint32_t epoch = s.capture_epoch.load();
        // Checked production I2S uses a finite 1000 ms driver timeout. An 80 ms
        // block is read in 20 ms pieces; never retain room audio across mute.
        if (!audio_playback_read_chunked_checked(w.capture_pcm, sizeof(w.capture_pcm), 1280)) { s.fail(codec_error); break; }
        if (s.stop.load()) break;
        if (epoch != previous_epoch) { w.capture.reset(); previous_epoch = epoch; }
        if (s.muted.load() || epoch != s.capture_epoch.load()) std::memset(w.capture_pcm, 0, sizeof(w.capture_pcm));
        Packet *packet = nullptr;
        if (xQueueReceive(s.tx_free, &packet, 0) != pdTRUE) { s.fail(pressure_error); break; }
        packet->epoch = epoch;
        if (!w.capture.process(w.capture_pcm, 1280, packet->pcm, 1920, packet->samples) || packet->samples != 1920) {
            xQueueSend(s.tx_free, &packet, 0); s.fail(protocol_error); break;
        }
        if (s.stop.load() || epoch != s.capture_epoch.load()) {
            xQueueSend(s.tx_free, &packet, 0); continue;
        }
        if (xQueueSend(s.tx, &packet, 0) != pdTRUE) { xQueueSend(s.tx_free, &packet, 0); s.fail(pressure_error); break; }
    }
    s.capture_done.store(true); vTaskDeleteWithCaps(nullptr);
}
void playback_task(void *argument) {
    auto &s = *static_cast<Session *>(argument); auto &w = *s.work;
    uint32_t previous_epoch = 0;
    while (!s.stop.load()) {
        Packet *packet = nullptr;
        if (xQueueReceive(s.rx, &packet, pdMS_TO_TICKS(20)) != pdTRUE) continue;
        if (packet->epoch == s.playback_epoch.load()) {
            if (packet->epoch != previous_epoch) { w.playback.reset(); previous_epoch = packet->epoch; }
            size_t frames = 0;
            if (!w.playback.process(packet->pcm, packet->samples, w.playback_pcm, 1280, frames)) s.fail(protocol_error);
            for (size_t offset = 0; offset < frames && !s.stop.load() && packet->epoch == s.playback_epoch.load();) {
                const size_t count = std::min<size_t>(320, frames - offset);
                if (!audio_playback_write_checked(w.playback_pcm + offset * 2, count * 4)) { s.fail(codec_error); break; }
                offset += count;
            }
        }
        xQueueSend(s.rx_free, &packet, 0); s.playback_outstanding.fetch_sub(1);
    }
    s.playback_done.store(true); vTaskDeleteWithCaps(nullptr);
}

// DNS callbacks may outlive cancellation, but own no session/callback or secret.
struct Resolution {
    std::atomic<unsigned> references{2}; std::atomic<bool> complete{false};
    char host[254]{}, address[IPADDR_STRLEN_MAX]{};
    void release() { if (references.fetch_sub(1) == 1) delete this; }
};
void resolved(const char *, const ip_addr_t *address, void *context) {
    auto *r = static_cast<Resolution *>(context);
    if (address) ipaddr_ntoa_r(address, r->address, sizeof(r->address));
    r->complete.store(true); r->release();
}
void resolve(void *context) {
    auto *r = static_cast<Resolution *>(context); ip_addr_t address;
    const err_t status = dns_gethostbyname(r->host, &address, resolved, r);
    if (status != ERR_INPROGRESS) resolved(nullptr, status == ERR_OK ? &address : nullptr, r);
}
void provider_event(Session &s, const char *json, size_t length);
struct Connection {
    Session &s; esp_transport_handle_t transport = nullptr;
    size_t position = 0, available = 0, header_used = 0, header_size = 2, payload_used = 0;
    bool upgraded = false, header_complete = false, closing = false;
    uint8_t header[10]{}, control[125]{};
    int64_t frame_deadline = 0, message_deadline = 0, pending_ping = 0;
    speech_live_protocol::Frame current{}; speech_live_protocol::Message message{};
    ~Connection() { if (transport) { esp_transport_close(transport); esp_transport_destroy(transport); } }
    int read(uint8_t *target, size_t count, int wait = 20) {
        if (position == available) {
            const int n = esp_transport_read(transport, reinterpret_cast<char *>(s.work->tls_in), 2048, wait);
            if (n == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT || n == ESP_TLS_ERR_SSL_WANT_READ ||
                n == ESP_TLS_ERR_SSL_WANT_WRITE || (n < 0 && errno == EAGAIN)) return 0;
            if (n <= 0) return n;
            position = 0; available = n;
        }
        const size_t n = std::min(count, available - position);
        std::memcpy(target, s.work->tls_in + position, n); position += n; return n;
    }
    bool exact(uint8_t *target, size_t count, int64_t deadline) {
        while (count && !s.stop.load() && esp_timer_get_time() < deadline) {
            const int n = read(target, count, 100);
            if (n < 0) return false;
            count -= n; target += n;
        }
        return !count;
    }
    bool write(const uint8_t *data, size_t count, int64_t deadline) {
        while (count && (!s.stop.load() || closing) && esp_timer_get_time() < deadline) {
            const int n = esp_transport_write(transport, reinterpret_cast<const char *>(data), count, 100);
            if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE ||
                n == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT || (n < 0 && errno == EAGAIN)) continue;
            if (n < 0) return false;
            count -= n; data += n;
        }
        return !count;
    }
    bool frame(uint8_t opcode, const char *data, size_t length) {
        uint8_t *out = s.work->tls_out, mask[4]; size_t used = 0;
        out[used++] = 0x80 | opcode;
        if (length < 126) out[used++] = 0x80 | uint8_t(length);
        else if (length < 65536) { out[used++] = 0xfe; out[used++] = length >> 8; out[used++] = length; }
        else { out[used++] = 0xff; for (int shift = 56; shift >= 0; shift -= 8) out[used++] = uint64_t(length) >> shift; }
        esp_fill_random(mask, sizeof(mask)); std::memcpy(out + used, mask, 4); used += 4;
        const int64_t deadline = esp_timer_get_time() + (closing ? 200000 : 2000000);
        if (!length) return write(out, used, deadline);
        for (size_t offset = 0; offset < length;) {
            const size_t prefix = offset ? 0 : used, n = std::min<size_t>(1280, length - offset);
            for (size_t i = 0; i < n; ++i) out[prefix + i] = uint8_t(data[offset + i]) ^ mask[(offset + i) % 4];
            if (!write(out, prefix + n, deadline)) return false;
            offset += n;
        }
        return true;
    }
    bool upgrade() {
        if (time(nullptr) < 1704067200) { s.fail("Synchronize device clock before native audio."); return false; }
        const int64_t deadline = esp_timer_get_time() + 15000000;
        char address[IPADDR_STRLEN_MAX]{}; ip_addr_t numeric;
        if (ipaddr_aton(s.endpoint.host, &numeric)) {
            if (!ipaddr_ntoa_r(&numeric, address, sizeof(address))) return false;
        }
        else {
            auto *r = new (std::nothrow) Resolution;
            if (!r) { s.fail(memory_error); return false; }
            std::strcpy(r->host, s.endpoint.host);
            if (tcpip_try_callback(resolve, r) != ERR_OK) { r->release(); r->release(); return false; }
            while (!r->complete.load() && !s.stop.load() && esp_timer_get_time() < deadline) vTaskDelay(pdMS_TO_TICKS(10));
            if (r->complete.load()) std::strcpy(address, r->address);
            r->release();
        }
        if (!address[0] || s.stop.load() || esp_timer_get_time() >= deadline) return false;
        transport = esp_transport_ssl_init(); if (!transport) { s.fail(memory_error); return false; }
        esp_transport_ssl_crt_bundle_attach(transport, esp_crt_bundle_attach);
        esp_transport_ssl_set_common_name(transport, s.endpoint.host);
        int connected = 0;
        do {
            if (s.stop.load() || esp_timer_get_time() >= deadline) return false;
            connected = esp_transport_connect_async(transport, address, s.endpoint.port, 100);
            if (!connected) vTaskDelay(pdMS_TO_TICKS(10));
        } while (!connected);
        if (connected < 0) return false;
        uint8_t nonce[16], digest[20]; char key[25], expected[29], challenge[61]; size_t n = 0;
        esp_fill_random(nonce, sizeof(nonce));
        if (mbedtls_base64_encode(reinterpret_cast<uint8_t *>(key), sizeof(key), &n, nonce, sizeof(nonce))) return false;
        key[n] = 0;
        if (std::snprintf(challenge, sizeof(challenge), "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key) != 60 ||
            mbedtls_sha1(reinterpret_cast<uint8_t *>(challenge), 60, digest) ||
            mbedtls_base64_encode(reinterpret_cast<uint8_t *>(expected), sizeof(expected), &n, digest, sizeof(digest))) return false;
        expected[n] = 0;
        const int request_size = std::snprintf(s.work->outgoing, sizeof(s.work->outgoing),
            "GET /v1/realtime HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\nAuthorization: Bearer %s\r\n\r\n",
            s.endpoint.authority, key, s.work->key);
        const bool sent = request_size > 0 && size_t(request_size) < sizeof(s.work->outgoing) &&
            write(reinterpret_cast<uint8_t *>(s.work->outgoing), request_size, deadline);
        mbedtls_platform_zeroize(s.work->outgoing, sizeof(s.work->outgoing));
        mbedtls_platform_zeroize(s.work->key, sizeof(s.work->key));
        if (!sent) return false;
        size_t used = 0; char *buffer = s.work->incoming;
        while (used < 8192 && !s.stop.load()) {
            if (!exact(reinterpret_cast<uint8_t *>(buffer + used), 1, deadline)) return false;
            const unsigned char c = buffer[used];
            if ((c < 32 && c != '\r' && c != '\n' && c != '\t') || c >= 127) return false;
            if (++used >= 4 && !std::memcmp(buffer + used - 4, "\r\n\r\n", 4)) break;
        }
        if (used >= 8192 || used < 4) return false;
        buffer[used] = 0; char *line = buffer, *end = std::strstr(line, "\r\n");
        if (!end) return false;
        *end = 0;
        if (!std::strncmp(line, "HTTP/1.1 401", 12) || !std::strncmp(line, "HTTP/1.1 403", 12)) { s.fail(auth_error); return false; }
        if (std::strncmp(line, "HTTP/1.1 101", 12) || (line[12] && line[12] != ' ')) return false;
        bool upgrade_header = false, connection_header = false, accept_header = false, seen_connection = false;
        for (line = end + 2; *line;) {
            if (line[0] == '\r' && line[1] == '\n') break;
            end = std::strstr(line, "\r\n"); if (!end) return false; *end = 0;
            char *colon = std::strchr(line, ':'); if (!colon || line[0] == ' ' || line[0] == '\t') return false;
            *colon = 0; char *value = colon + 1; while (*value == ' ' || *value == '\t') ++value;
            char *tail = end; while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
            if (!strcasecmp(line, "Upgrade")) { if (upgrade_header || strcasecmp(value, "websocket")) return false; upgrade_header = true; }
            else if (!strcasecmp(line, "Connection")) {
                if (seen_connection) return false;
                seen_connection = true;
                for (char *token = value; *token;) {
                    char *comma = std::strchr(token, ','); if (comma) *comma = 0;
                    while (*token == ' ' || *token == '\t') ++token;
                    char *last = token + std::strlen(token); while (last > token && (last[-1] == ' ' || last[-1] == '\t')) *--last = 0;
                    if (!strcasecmp(token, "Upgrade")) connection_header = true;
                    if (!comma) break;
                    token = comma + 1;
                }
            } else if (!strcasecmp(line, "Sec-WebSocket-Accept")) { if (accept_header || std::strcmp(value, expected)) return false; accept_header = true; }
            else if (!strcasecmp(line, "Sec-WebSocket-Extensions") || !strcasecmp(line, "Sec-WebSocket-Protocol")) return false;
            line = end + 2;
        }
        upgraded = upgrade_header && connection_header && accept_header; return upgraded;
    }
    bool pump() {
        const int64_t now = esp_timer_get_time();
        if ((frame_deadline && now >= frame_deadline) || (message_deadline && now >= message_deadline)) return false;
        for (unsigned iteration = 0; iteration < 32 && !s.stop.load(); ++iteration) {
            if (position == available) {
                const int poll = esp_transport_poll_read(transport, 0);
                if (poll < 0) return false;
                if (!poll) return true;
            }
            if (!header_complete) {
                const int n = read(header + header_used, header_size - header_used);
                if (n < 0) return false;
                if (!n) return true;
                if (!frame_deadline) frame_deadline = now + 5000000;
                header_used += n; if (header_used < header_size) continue;
                if (header_size == 2) {
                    const unsigned extended = header[1] & 127;
                    header_size = extended == 126 ? 4 : extended == 127 ? 10 : 2;
                    if (header_used < header_size) continue;
                }
                if (!speech_live_protocol::decode_header(header, header_size, current) || !message.begin(current) ||
                    (!(current.opcode & 8) && (message.length > protocol::max_event_bytes || current.length > protocol::max_event_bytes - message.length))) {
                    s.fail(protocol_error); return false;
                }
                if (!(current.opcode & 8) && !message_deadline) message_deadline = now + 10000000;
                payload_used = 0; header_complete = true;
            }
            if (payload_used < current.length) {
                uint8_t *target = current.opcode & 8 ? control : reinterpret_cast<uint8_t *>(s.work->incoming + message.length);
                const int n = read(target + payload_used, current.length - payload_used);
                if (n < 0) return false;
                if (!n) return true;
                payload_used += n; if (payload_used < current.length) continue;
            }
            if (current.opcode == 9) { if (!frame(10, reinterpret_cast<char *>(control), current.length)) return false; }
            else if (current.opcode == 10) pending_ping = 0;
            else if (current.opcode == 8) {
                if (current.length == 1) { s.fail(protocol_error); return false; }
                if (current.length >= 2) {
                    const unsigned code = (control[0] << 8) | control[1];
                    if (code < 1000 || code >= 5000 || code == 1004 || code == 1005 || code == 1006 || code == 1015 ||
                        (code >= 1016 && code < 3000) || !speech_live_protocol::utf8(reinterpret_cast<char *>(control + 2), current.length - 2)) {
                        s.fail(protocol_error); return false;
                    }
                }
                s.fail(end_error); return false;
            } else if (message.finish(current)) {
                s.work->incoming[message.length] = 0;
                provider_event(s, s.work->incoming, message.length); message_deadline = 0;
            }
            header_used = 0; header_size = 2; header_complete = false; frame_deadline = 0;
        }
        return true;
    }
};

void emit_text(Session &s, Text &text) {
    Writer out{s.work->outgoing, sizeof(s.work->outgoing)};
    out.raw("{\"type\":\"transcript\",\"role\":"); out.quote(text.assistant ? "assistant" : "user");
    out.raw(",\"text\":"); out.quote(text.text); char number[24]; std::snprintf(number, sizeof(number), "%lu", static_cast<unsigned long>(text.turn));
    out.raw(",\"turn\":"); out.raw(number); out.raw(",\"final\":"); out.raw(text.final ? "true" : "false");
    out.raw(",\"providerSession\":"); out.quote(s.provider_session); out.raw(",\"itemId\":"); out.quote(text.item);
    out.raw(",\"responseId\":"); out.quote(text.response); out.raw(",\"interrupted\":"); out.raw(text.interrupted ? "true}" : "false}");
    if (!out.ok) s.fail(protocol_error); else s.emit(out.data);
}
void transcript(Session &s, const protocol::Event &event, bool assistant, bool final) {
    if (!event.item_id[0]) { s.fail(protocol_error); return; }
    Text *target = nullptr, *oldest = nullptr;
    for (auto &text : s.work->text) {
        if (text.turn && text.assistant == assistant && !std::strcmp(text.item, event.item_id) && !std::strcmp(text.response, event.response_id)) { target = &text; break; }
        if (!text.turn || (text.final && (!oldest || (oldest->turn && text.turn < oldest->turn)))) oldest = &text;
    }
    if (!target) {
        if (!oldest || s.turn == UINT32_MAX) { s.fail(pressure_error); return; }
        target = oldest; std::memset(target, 0, sizeof(*target)); target->assistant = assistant; target->turn = ++s.turn;
        std::strcpy(target->item, event.item_id); std::strcpy(target->response, event.response_id);
    }
    if (target->final) return;
    if (!protocol::update_transcript(target->text, sizeof(target->text), event.text, final)) {
        s.fail(protocol_error); return;
    }
    target->final = final;
    if (assistant && event.response_id[0] && !std::strcmp(s.obsolete, event.response_id)) target->interrupted = true;
    emit_text(s, *target);
}
void provider_event(Session &s, const char *json, size_t length) {
    const char *error = nullptr; auto &event = s.work->event;
    if (!protocol::parse_event(json, length, event, error)) {
        s.fail(error && !std::strcmp(error, "native_json_node_limit") ?
               "Native audio event exceeded the fixed JSON node limit." : protocol_error); return;
    }
    using Kind = protocol::EventKind;
    if (event.kind == Kind::SessionCreated) {
        if (s.provider_session[0] || !event.provider_session[0]) { s.fail(protocol_error); return; }
        std::strcpy(s.provider_session, event.provider_session); return;
    }
    if (event.kind == Kind::SessionUpdated) {
        if (s.ready.load() || !s.provider_session[0] || !event.effective_pcm24 || !event.tools_acknowledged ||
            (event.provider_session[0] && std::strcmp(event.provider_session, s.provider_session)) ||
            !protocol::json_equal(s.work->tools, std::strlen(s.work->tools), event.tools_json, std::strlen(event.tools_json), event.workspace, s.work->second_workspace)) {
            s.fail("Native audio server did not acknowledge PCM24 and the configured tools."); return;
        }
        s.ready.store(true); return;
    }
    if (event.kind == Kind::Error) {
        s.fail(!std::strcmp(event.error_code, "session_timeout") ? "Native audio provider session limit reached. Start a new session explicitly." :
               !std::strcmp(event.error_code, "inference_timeout") ? "Native audio inference timed out. Reconnect explicitly." :
               "Native audio provider reported an error. Check deployment status before reconnecting."); return;
    }
    if (event.kind == Kind::SessionEnd) { s.fail(end_error); return; }
    if (!s.ready.load()) {
        if (event.kind != Kind::Ignore) s.fail(protocol_error);
        return;
    }
    switch (event.kind) {
    case Kind::SpeechStarted: {
        if (s.response[0] && (!s.audio_done || s.playback_outstanding.load())) {
            std::strcpy(s.obsolete, s.response); s.playback_epoch.fetch_add(1); clear_audio(s, false);
            for (auto &text : s.work->text) if (text.assistant && text.turn && !std::strcmp(text.response, s.response)) {
                text.interrupted = true; emit_text(s, text);
            }
        }
        s.speaking = false; s.waiting_response = false; s.phase(s.muted.load() ? "muted" : "listening"); break;
    }
    case Kind::SpeechStopped: s.waiting_response = true; s.phase(s.muted.load() ? "muted" : "working"); break;
    case Kind::ResponseCreated:
        if (!event.response_id[0]) { s.fail(protocol_error); break; }
        if (!std::strcmp(event.response_id, s.response)) break;
        std::strcpy(s.response, event.response_id); s.audio_done = false; s.response_done = false; s.waiting_response = false; break;
    case Kind::AudioDelta: {
        if (s.response_done || !event.response_id[0] || std::strcmp(event.response_id, s.response) || !std::strcmp(event.response_id, s.obsolete)) break;
        size_t samples = 0;
        if (!protocol::decode_audio(event.audio_base64, std::strlen(event.audio_base64), s.work->decoded, 12288, samples, error) || !samples) { s.fail(protocol_error); break; }
        for (size_t offset = 0; offset < samples && !s.stop.load();) {
            Packet *packet = nullptr;
            if (xQueueReceive(s.rx_free, &packet, 0) != pdTRUE) { s.fail(pressure_error); break; }
            packet->epoch = s.playback_epoch.load(); packet->samples = std::min<size_t>(1920, samples - offset);
            std::memcpy(packet->pcm, s.work->decoded + offset, packet->samples * sizeof(int16_t)); offset += packet->samples;
            s.playback_outstanding.fetch_add(1);
            if (xQueueSend(s.rx, &packet, 0) != pdTRUE) {
                s.playback_outstanding.fetch_sub(1);
                xQueueSend(s.rx_free, &packet, 0); s.fail(pressure_error); break;
            }
        }
        s.speaking = true; s.phase(s.muted.load() ? "muted" : "speaking"); break;
    }
    case Kind::AudioDone: if (!std::strcmp(event.response_id, s.response)) s.audio_done = true; break;
    case Kind::UserDelta: transcript(s, event, false, false); break;
    case Kind::UserFinal: transcript(s, event, false, true); break;
    case Kind::AssistantDelta: transcript(s, event, true, false); break;
    case Kind::AssistantFinal: transcript(s, event, true, true); break;
    case Kind::FunctionDone: {
        bool admitted = false;
        { Guard guard;
          for (auto &pending : s.pending) if (!std::strcmp(pending.id, event.call_id)) { admitted = true; break; }
          if (!admitted) for (auto &pending : s.pending) if (!pending.id[0]) { std::strcpy(pending.id, event.call_id); admitted = true; break; }
        }
        if (!admitted) { s.fail(pressure_error); break; }
        Writer out{s.work->outgoing, sizeof(s.work->outgoing)};
        out.raw("{\"type\":\"tool_call\",\"providerSession\":"); out.quote(s.provider_session);
        out.raw(",\"callId\":"); out.quote(event.call_id); out.raw(",\"name\":"); out.quote(event.name);
        out.raw(",\"arguments\":"); out.quote(event.arguments); out.raw("}");
        if (!out.ok) s.fail(protocol_error); else { s.emit(out.data); s.phase(s.muted.load() ? "muted" : "working"); }
        break;
    }
    case Kind::ResponseDone:
        if (!std::strcmp(event.response_id, s.response)) { s.audio_done = true; s.response_done = true; s.waiting_response = false; }
        break;
    default: break;
    }
}
bool spawn(TaskFunction_t function, const char *name, Session &s, std::atomic<bool> &done) {
    done.store(false);
    if (xTaskCreateWithCaps(function, name, 6144, &s, 5, nullptr, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) { done.store(true); s.fail(memory_error); return false; }
    return true;
}
void control_task(void *argument) {
    auto &s = *static_cast<Session *>(argument);
    {
        Connection connection{s};
        if (!connection.upgrade() && !s.stop.load()) s.fail(network_error);
        if (!s.stop.load() && !connection.frame(1, s.work->config, std::strlen(s.work->config))) s.fail(network_error);
        const int64_t setup_deadline = esp_timer_get_time() + 15000000;
        while (!s.stop.load() && !s.ready.load() && esp_timer_get_time() < setup_deadline) {
            if (!connection.pump()) { s.fail(network_error); break; }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!s.stop.load() && !s.ready.load()) s.fail("Native audio session configuration acknowledgment timed out.");
        if (!s.stop.load()) {
            if (spawn(playback_task, "native_speaker", s, s.playback_done) && spawn(capture_task, "native_mic", s, s.capture_done)) {
                s.emit("{\"type\":\"ready\"}"); s.phase(s.muted.load() ? "muted" : "listening");
            }
        }
        int64_t ping_at = esp_timer_get_time() + 10000000;
        while (!s.stop.load()) {
            if (!connection.pump()) { if (!s.stop.load()) s.fail(network_error); break; }
            Result *result = nullptr;
            if (xQueueReceive(s.result, &result, 0) == pdTRUE) {
                Writer out{s.work->outgoing, sizeof(s.work->outgoing)};
                out.event("conversation.item.create"); out.raw(",\"item\":{\"type\":\"function_call_output\",\"call_id\":");
                out.quote(result->call); out.raw(",\"output\":"); out.quote(result->text); out.raw("}}");
                const bool sent = out.ok && connection.frame(1, out.data, out.length);
                { Guard guard;
                  for (auto &pending : s.pending) if (!std::strcmp(pending.id, result->call)) { pending.id[0] = 0; pending.queued = false; }
                }
                mbedtls_platform_zeroize(result, sizeof(*result)); xQueueSend(s.result_free, &result, 0);
                if (!sent) { s.fail(network_error); break; }
                s.waiting_response = true;
            }
            Packet *packet = nullptr;
            if (xQueueReceive(s.tx, &packet, 0) == pdTRUE) {
                // Commit one selected frame at the privacy gate, not the queue.
                // Mute invalidates every queued frame. An already handed-off
                // transport frame cannot be unsent; lifecycle locks never cover I/O.
                bool admitted = false, sent = true;
                { Guard guard;
                  if (!s.stop.load() && packet->epoch == s.capture_epoch.load()) {
                      if (s.muted.load()) std::memset(packet->pcm, 0, packet->samples * sizeof(int16_t));
                      admitted = true;
                  }
                }
                if (admitted) {
                    Writer out{s.work->outgoing, sizeof(s.work->outgoing)};
                    out.event("input_audio_buffer.append"); out.raw(",\"audio\":\""); size_t encoded = 0;
                    if (!out.ok || !protocol::encode_audio(packet->pcm, packet->samples, out.data + out.length, out.capacity - out.length, encoded)) sent = false;
                    else {
                        out.length += encoded; out.raw("\"}");
                        if (!s.stop.load() && packet->epoch == s.capture_epoch.load())
                            sent = out.ok && connection.frame(1, out.data, out.length);
                    }
                }
                xQueueSend(s.tx_free, &packet, 0);
                if (!sent) { s.fail(network_error); break; }
            }
            if (s.audio_done && !s.playback_outstanding.load()) s.speaking = false;
            bool pending_tool = false;
            { Guard guard; for (const auto &pending : s.pending) if (pending.id[0]) pending_tool = true; }
            if (s.muted.load()) s.phase("muted");
            else if (s.speaking) s.phase("speaking");
            else if (s.waiting_response || pending_tool) s.phase("working");
            else if (s.audio_done) s.phase("listening");
            const int64_t now = esp_timer_get_time();
            if (connection.pending_ping && now - connection.pending_ping > 30000000) { s.fail(network_error); break; }
            if (now >= ping_at) {
                if (!connection.frame(9, nullptr, 0)) { s.fail(network_error); break; }
                if (!connection.pending_ping) connection.pending_ping = now;
                ping_at = now + 10000000;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        s.ready.store(false); s.muted.store(true); s.stop.store(true); s.capture_epoch.fetch_add(1); s.playback_epoch.fetch_add(1);
        if (connection.upgraded && s.user_stop.load()) {
            connection.closing = true;
            Writer out{s.work->outgoing, sizeof(s.work->outgoing)}; out.event("session.close"); out.raw("}");
            if (out.ok) connection.frame(1, out.data, out.length);
            const char close[] = {3, static_cast<char>(232)}; connection.frame(8, close, sizeof(close));
        }
        // Never delete a task blocked in codec I/O. The production new-I2S
        // backend's finite driver waits bound this join (four reads + one write).
        while (!s.capture_done.load() || !s.playback_done.load()) vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s.failure.load() && !s.user_stop.load()) {
        Writer out{s.work->outgoing, sizeof(s.work->outgoing)}; out.raw("{\"type\":\"error\",\"message\":"); out.quote(s.failure.load()); out.raw("}"); s.emit(out.data);
    }
    s.emit("{\"type\":\"closed\"}");
    { Guard guard; live = nullptr; release(&s); }
    vTaskDeleteWithCaps(nullptr);
}
} // namespace

bool speech_native_start(const char *endpoint, const char *key, const char *instructions, const char *tools,
                         speech_live_event_callback_t callback, void *context, std::string &error) {
    error.clear(); Endpoint parsed; size_t instruction_length, tools_length;
    if (!endpoint_parse(endpoint, parsed)) { error = "Use an exact wss://host[:port]/v1/realtime endpoint without URL credentials, query or fragment."; return false; }
    if (!valid_key(key)) { error = "Native audio requires a valid deployment bearer key."; return false; }
    if (!callback || !bounded(instructions, 8192, instruction_length) || !bounded(tools, protocol::max_tools_bytes, tools_length)) {
        error = "Native audio requires an event consumer, UTF-8 instructions within 8192 bytes, and tools within 32768 bytes."; return false;
    }
    Guard guard;
    if (live) { error = "Native audio is active or still closing."; return false; }
    auto *storage = heap_caps_calloc(1, sizeof(Session), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!storage) { error = memory_error; return false; }
    auto *s = new (storage) Session;
    auto *work = psram(sizeof(Working)); if (work) s->work = new (work) Working;
    if (!s->work) { release(s); error = memory_error; return false; }
    s->endpoint = parsed; s->callback = callback; s->context = context;
    const char *json_error = nullptr;
    if (!protocol::validate_json(tools, tools_length, s->work->second_workspace, json_error) ||
        s->work->second_workspace.nodes[0].type != protocol::JsonType::Array) {
        release(s); error = json_error && !std::strcmp(json_error, "native_json_node_limit") ?
            "Native tool schemas exceed the fixed 512 JSON-node setup limit." :
            "Native tool schemas must be a bounded valid JSON array."; return false;
    }
    std::strcpy(s->work->tools, tools); std::strcpy(s->work->key, key);
    Writer config{s->work->config, sizeof(s->work->config)};
    config.event("session.update"); config.raw(",\"session\":{\"audio\":{\"input\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000}},\"output\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000}}},\"instructions\":");
    config.quote(instructions); config.raw(",\"tools\":"); config.raw(tools); config.raw("}}");
    if (!config.ok || config.length > config_limit) { release(s); error = "Native session configuration exceeds the 32 KiB setup bound."; return false; }
    QueueHandle_t *queues[] = {&s->tx, &s->tx_free, &s->rx, &s->rx_free, &s->result, &s->result_free};
    for (unsigned i = 0; i < 6; ++i) {
        *queues[i] = xQueueCreateStatic(slots, sizeof(void *), s->queue_bytes[i], &s->queue_controls[i]);
        if (!*queues[i]) { release(s); error = memory_error; return false; }
    }
    for (unsigned i = 0; i < slots; ++i) {
        Packet *tx = &s->work->tx[i], *rx = &s->work->rx[i]; Result *result = &s->work->results[i];
        xQueueSend(s->tx_free, &tx, 0); xQueueSend(s->rx_free, &rx, 0); xQueueSend(s->result_free, &result, 0);
    }
    live = s;
    if (xTaskCreateWithCaps(control_task, "native_socket", 8192, s, 5, nullptr, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        live = nullptr; release(s); error = memory_error; return false;
    }
    return true;
}
bool speech_native_active() { Guard guard; return live != nullptr; }
bool speech_native_muted() { Guard guard; return !live || live->muted.load(); }
bool speech_native_set_muted(bool value, std::string &error) {
    error.clear(); Guard guard;
    if (!live || live->stop.load()) { error = "Native audio is not active."; return false; }
    if (live->muted.exchange(value) != value) { live->capture_epoch.fetch_add(1); clear_audio(*live, true); }
    return true;
}
void speech_native_stop() {
    Guard guard;
    if (live) { live->user_stop.store(true); live->muted.store(true); live->capture_epoch.fetch_add(1); live->playback_epoch.fetch_add(1); live->stop.store(true); }
}
bool speech_native_wait_stopped(uint32_t timeout_ms) {
    const int64_t deadline = esp_timer_get_time() + int64_t(timeout_ms) * 1000;
    do {
        { Guard guard; if (!live) return true; }
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (true);
}
bool speech_native_tool_result(const char *session, const char *call, const char *result, std::string &error) {
    error.clear(); size_t session_length, call_length, result_length;
    if (!bounded(session, 128, session_length) || !session_length || !bounded(call, 128, call_length) || !call_length ||
        !bounded(result, 8192, result_length) || !result_length) { error = "Native tool results require bounded session/call IDs and JSON within 8192 bytes."; return false; }
    Guard guard;
    if (!live || live->stop.load() || !live->ready.load() || std::strcmp(session, live->provider_session)) { error = "Native tool result belongs to an inactive provider session."; return false; }
    Pending *pending = nullptr;
    for (auto &p : live->pending) if (!std::strcmp(p.id, call)) pending = &p;
    if (!pending || pending->queued) { error = "Native tool call is no longer awaiting a result."; return false; }
    const char *json_error = nullptr;
    if (!protocol::validate_json(result, result_length, live->work->second_workspace, json_error)) { error = "Native tool result is not valid bounded JSON."; return false; }
    Result *slot = nullptr;
    if (xQueueReceive(live->result_free, &slot, 0) != pdTRUE) { live->fail(pressure_error); error = pressure_error; return false; }
    std::strcpy(slot->session, session); std::strcpy(slot->call, call); std::strcpy(slot->text, result); pending->queued = true;
    if (xQueueSend(live->result, &slot, 0) != pdTRUE) {
        xQueueSend(live->result_free, &slot, 0); live->fail(pressure_error); error = pressure_error; return false;
    }
    return true;
}

bool speech_native_check_readiness(const char *endpoint, const char *key, std::atomic<bool> &cancel, std::string &error) {
    error.clear(); Endpoint parsed;
    if (!endpoint_parse(endpoint, parsed) || !valid_key(key)) { error = "Native readiness requires an exact WSS endpoint and deployment bearer key."; return false; }
    if (cancel.load()) { error = "Native readiness cancelled."; return false; }
    char url[320]; std::snprintf(url, sizeof(url), "https://%s/v1/realtime/health", parsed.authority);
    struct Buffer {
        char *body = static_cast<char *>(psram(8194));
        char *authorization = static_cast<char *>(psram(4104));
        protocol::JsonWorkspace *workspace = static_cast<protocol::JsonWorkspace *>(psram(sizeof(protocol::JsonWorkspace)));
        ~Buffer() { if (authorization) { mbedtls_platform_zeroize(authorization, 4104); heap_caps_free(authorization); } heap_caps_free(body); heap_caps_free(workspace); }
    } buffer;
    if (!buffer.body || !buffer.authorization || !buffer.workspace) { error = memory_error; return false; }
    std::snprintf(buffer.authorization, 4104, "Bearer %s", key);
    BoundedHttps transport(cancel, 15000); std::string transport_error;
    if (!transport.initialize(url, transport_error)) { error = cancel.load() ? "Native readiness cancelled." : network_error; return false; }
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_http_client_config_t config{}; config.url = url; config.method = HTTP_METHOD_GET;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL; config.timeout_ms = 100; config.is_async = true;
    config.crt_bundle_attach = esp_crt_bundle_attach; config.skip_cert_common_name_check = false;
    config.disable_auto_redirect = true; config.max_authorization_retries = -1;
    config.buffer_size = 1024; config.buffer_size_tx = 6144; config.transport = transport.transport();
    struct Client { esp_http_client_handle_t handle = nullptr; ~Client() { if (handle) esp_http_client_cleanup(handle); } } client;
    client.handle = esp_http_client_init(&config);
    if (!client.handle || esp_http_client_set_header(client.handle, "Authorization", buffer.authorization) != ESP_OK ||
        esp_http_client_set_header(client.handle, "Accept", "application/json") != ESP_OK ||
        esp_http_client_set_header(client.handle, "Accept-Encoding", "identity") != ESP_OK) { error = memory_error; return false; }
    auto failed = [&]() { error = cancel.load() ? "Native readiness cancelled." : network_error; return false; };
    esp_err_t opened;
    do {
        if (transport.stopped()) return failed();
        opened = esp_http_client_open(client.handle, 0);
        if (opened == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (opened == ESP_ERR_HTTP_EAGAIN);
    if (opened != ESP_OK) return failed();
    int64_t length;
    do {
        if (transport.stopped()) return failed();
        length = esp_http_client_fetch_headers(client.handle);
        if (length == -ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (length == -ESP_ERR_HTTP_EAGAIN);
    if (length < 0) return failed();
    const int status = esp_http_client_get_status_code(client.handle);
    if (status == 401 || status == 403) { error = auth_error; return false; }
    if (status != 200) { error = "Native audio deployment is not ready (health HTTP status rejected)."; return false; }
    if (length > 8192) { error = "Native audio health response exceeds 8 KiB."; return false; }
    size_t used = 0;
    for (;;) {
        if (transport.stopped()) return failed();
        const int wanted = static_cast<int>(std::min<size_t>(1024, 8193 - used));
        const int n = esp_http_client_read(client.handle, buffer.body + used, wanted);
        if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (n < 0 || n > wanted) return failed();
        if (!n) {
            if (!esp_http_client_is_complete_data_received(client.handle) || (length > 0 && used != size_t(length))) return failed();
            break;
        }
        used += n;
        if (used > 8192) { error = "Native audio health response exceeds 8 KiB."; return false; }
    }
    buffer.body[used] = 0;
    const char *json_error = nullptr;
    if (transport.stopped()) return failed();
    if (!protocol::validate_health(buffer.body, used, *buffer.workspace, json_error)) {
        error = "Native audio health is not the documented healthy Triton-backed service."; return false;
    }
    return true;
}
