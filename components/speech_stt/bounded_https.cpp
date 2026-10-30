#include "bounded_https.hpp"
#include "esp_crt_bundle.h"
#include "sdkconfig.h"
#if !CONFIG_MBEDTLS_HAVE_TIME_DATE || CONFIG_ESP_TLS_INSECURE
#error "Speech/model transport requires certificate date validation and secure TLS"
#endif
#if !CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT
#error "Bounded HTTPS requires ESP HTTP custom transport support"
#endif
#include "esp_timer.h"
#include "esp_transport_ssl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <new>

namespace {
// A DNS callback can outlive a cancelled operation. It owns only this small
// record, never the HTTP client, caller settings, audio, or callback context.
struct Resolution {
    std::atomic<unsigned> references{2};
    std::atomic<bool> complete{false};
    char hostname[254]{};
    char address[IPADDR_STRLEN_MAX]{};
    void release() { if (references.fetch_sub(1) == 1) delete this; }
};
void resolved(const char *, const ip_addr_t *address, void *context) {
    auto *result = static_cast<Resolution *>(context);
    if (address) ipaddr_ntoa_r(address, result->address, sizeof(result->address));
    result->complete.store(true);
    result->release();
}
void resolve(void *context) {
    auto *result = static_cast<Resolution *>(context);
    ip_addr_t address;
    const err_t status = dns_gethostbyname(result->hostname, &address, resolved, result);
    if (status != ERR_INPROGRESS)
        resolved(nullptr, status == ERR_OK ? &address : nullptr, result);
}
}

BoundedHttps::BoundedHttps(const std::atomic<bool> &cancelled, uint32_t timeout_ms,
                         uint64_t deadline_us, bool (*cancelled_call)(void *), void *call_context)
    : cancelled_(cancelled),
      deadline_(deadline_us ? std::min<uint64_t>(deadline_us, esp_timer_get_time() + uint64_t(timeout_ms)*1000)
                            : esp_timer_get_time() + uint64_t(timeout_ms)*1000),
      cancelled_call_(cancelled_call), call_context_(call_context) {}
BoundedHttps::~BoundedHttps() {
    if (wrapper_) esp_transport_destroy(wrapper_);
    if (tls_) esp_transport_destroy(tls_);
}
bool BoundedHttps::cancelled() const {
    return cancelled_.load() || (cancelled_call_ && cancelled_call_(call_context_));
}
bool BoundedHttps::stopped() const {
    return cancelled() || esp_timer_get_time() >= deadline_;
}
int BoundedHttps::wait_ms() const {
    const int64_t remaining = deadline_ - esp_timer_get_time();
    return cancelled() || remaining <= 0 ? 0 :
        static_cast<int>(std::min<int64_t>(100, (remaining + 999) / 1000));
}
const char *BoundedHttps::stop_reason() const {
    return cancelled() ? "Request cancelled; no partial response accepted." :
        "Request exceeded its total deadline; no partial response accepted.";
}
bool BoundedHttps::initialize(const char *url, std::string &error) {
    if (!url || std::strncmp(url, "https://", 8)) {
        error = "A verified HTTPS endpoint is required."; return false;
    }
    if (time(nullptr) < 1704067200) {
        error = "Synchronize the device clock before verified HTTPS."; return false;
    }
    const char *start = url + 8, *end = std::strchr(start, '/');
    if (!end || start == end || std::memchr(start, '@', end - start)) {
        error = "HTTPS endpoint has an invalid authority."; return false;
    }
    if (*start == '[') {
        ++start;
        const char *bracket = static_cast<const char *>(std::memchr(start, ']', end - start));
        if (!bracket) { error = "Invalid HTTPS IPv6 host."; return false; }
        end = bracket;
    } else {
        const char *port = static_cast<const char *>(std::memchr(start, ':', end - start));
        if (port) end = port;
    }
    if (end == start || end - start > 253) { error = "Invalid HTTPS hostname."; return false; }
    hostname_.assign(start, end - start);
    ip_addr_t numeric;
    if (ipaddr_aton(hostname_.c_str(), &numeric)) {
        address_ = hostname_;
    } else {
        auto *result = new (std::nothrow) Resolution;
        if (!result) { error = "Not enough memory for DNS resolution."; return false; }
        std::memcpy(result->hostname, hostname_.c_str(), hostname_.size() + 1);
        // Non-blocking enqueue; failure releases both references.
        if (tcpip_try_callback(resolve, result) != ERR_OK) {
            result->release(); result->release();
            error = "DNS resolver queue is unavailable."; return false;
        }
        while (!result->complete.load() && !stopped()) vTaskDelay(pdMS_TO_TICKS(20));
        if (result->complete.load()) address_ = result->address;
        result->release();
        if (stopped()) { error = stop_reason(); return false; }
        if (address_.empty()) { error = "HTTPS hostname resolution failed."; return false; }
    }
    if (stopped()) { error = stop_reason(); return false; }
    tls_ = esp_transport_ssl_init();
    wrapper_ = esp_transport_init();
    if (!tls_ || !wrapper_) { error = "Not enough memory for HTTPS transport."; return false; }
    esp_transport_ssl_crt_bundle_attach(tls_, esp_crt_bundle_attach);
    // Numeric connection avoids blocking getaddrinfo in esp_tls. Verification
    // and SNI still use the configured hostname, never the resolved address.
    esp_transport_ssl_set_common_name(tls_, hostname_.c_str());
    esp_transport_set_context_data(wrapper_, this);
    esp_transport_set_default_port(wrapper_, 443);
    esp_transport_set_func(wrapper_, connect, read, write, close, poll_read, poll_write, destroy);
    esp_transport_set_async_connect_func(wrapper_, connect);
    return true;
}
BoundedHttps &BoundedHttps::owner(esp_transport_handle_t handle) {
    return *static_cast<BoundedHttps *>(esp_transport_get_context_data(handle));
}
int BoundedHttps::connect(esp_transport_handle_t handle, const char *, int port, int) {
    auto &self = owner(handle);
    if (self.stopped()) { errno = ECANCELED; return -1; }
    // esp-tls fills its select() sets for the TCP connect only once, and a
    // select timeout clears them, so a connect slower than the first poll can
    // never complete. Give the first poll the whole TCP connect budget.
    int timeout = self.wait_ms();
    if (!self.connect_started_) {
        self.connect_started_ = true;
        timeout = static_cast<int>(std::min<int64_t>(10000, (self.deadline_ - esp_timer_get_time() + 999) / 1000));
    }
    return esp_transport_connect_async(self.tls_, self.address_.c_str(), port, std::max(timeout, 1));
}
int BoundedHttps::read(esp_transport_handle_t handle, char *buffer, int length, int) {
    auto &self = owner(handle);
    // Do not re-enter fetch_headers on ordinary poll timeouts: IDF resets its
    // status code on entry, losing a status line received in an earlier poll.
    for (;;) {
        if (self.stopped()) { errno = ECANCELED; return ERR_TCP_TRANSPORT_CONNECTION_FAILED; }
        const int result = esp_transport_read(self.tls_, buffer, length, self.wait_ms());
        if (result == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT ||
            result == ESP_TLS_ERR_SSL_WANT_READ || result == ESP_TLS_ERR_SSL_WANT_WRITE) continue;
        return result;
    }
}
int BoundedHttps::write(esp_transport_handle_t handle, const char *buffer, int length, int) {
    auto &self = owner(handle);
    // request_send treats zero writes as fatal and can loop internally.
    // Check Stop/deadline between every bounded TLS readiness poll.
    for (;;) {
        errno = 0;
        if (self.stopped()) { errno = ECANCELED; return ERR_TCP_TRANSPORT_CONNECTION_FAILED; }
        const int result = esp_transport_write(self.tls_, buffer, length, self.wait_ms());
        if (result > 0) return result;
        if (result == 0 || result == ESP_TLS_ERR_SSL_WANT_READ ||
            result == ESP_TLS_ERR_SSL_WANT_WRITE || errno == EAGAIN) continue;
        return result;
    }
}
int BoundedHttps::close(esp_transport_handle_t handle) { return esp_transport_close(owner(handle).tls_); }
int BoundedHttps::destroy(esp_transport_handle_t) { return 0; }
int BoundedHttps::poll_read(esp_transport_handle_t handle, int) {
    auto &self = owner(handle);
    return self.stopped() ? -1 : esp_transport_poll_read(self.tls_, self.wait_ms());
}
int BoundedHttps::poll_write(esp_transport_handle_t handle, int) {
    auto &self = owner(handle);
    return self.stopped() ? -1 : esp_transport_poll_write(self.tls_, self.wait_ms());
}
