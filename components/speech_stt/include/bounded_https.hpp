#pragma once

struct esp_transport_item_t;
using esp_transport_handle_t = esp_transport_item_t *;
#include <atomic>
#include <cstdint>
#include <string>

// Single I/O owner. Keep alive until after esp_http_client_cleanup(). The custom
// transport checks the deadline inside IDF's otherwise unbounded header loops.
class BoundedHttps {
public:
    // The optional borrowed predicate remains valid through client cleanup.
    // An absolute deadline includes scheduling time before HTTP/DNS starts.
    explicit BoundedHttps(const std::atomic<bool> &cancelled, uint32_t timeout_ms = 45000,
                          uint64_t deadline_us = 0, bool (*cancelled_call)(void *) = nullptr,
                          void *call_context = nullptr);
    ~BoundedHttps();
    BoundedHttps(const BoundedHttps &) = delete;
    BoundedHttps &operator=(const BoundedHttps &) = delete;
    bool initialize(const char *url, std::string &error);
    bool stopped() const;
    int wait_ms() const;
    esp_transport_handle_t transport() const { return wrapper_; }
    const char *stop_reason() const;
private:
    static BoundedHttps &owner(esp_transport_handle_t);
    static int connect(esp_transport_handle_t, const char *, int, int);
    static int read(esp_transport_handle_t, char *, int, int);
    static int write(esp_transport_handle_t, const char *, int, int);
    static int close(esp_transport_handle_t);
    static int destroy(esp_transport_handle_t);
    static int poll_read(esp_transport_handle_t, int);
    static int poll_write(esp_transport_handle_t, int);
    const std::atomic<bool> &cancelled_;
    const int64_t deadline_;
    bool (*cancelled_call_)(void *);
    void *call_context_;
    bool cancelled() const;
    esp_transport_handle_t tls_ = nullptr, wrapper_ = nullptr;
    std::string hostname_, address_;
};
