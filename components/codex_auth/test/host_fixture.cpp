#include "host_fixture.hpp"
#include "bounded_https.hpp"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "cJSON.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>

struct HostTask {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    unsigned notifications = 0;
    bool stop = false;
};
struct esp_transport_item_t { BoundedHttps *owner; };
struct HostHttp {
    std::string url, body;
    std::map<std::string, std::string> headers;
    esp_transport_handle_t transport;
    std::shared_ptr<fixture::Reply> response;
    size_t expected = 0, offset = 0;
    bool recorded = false, active_refresh = false, retry_open = true, retry_write = true;
};
namespace {
std::atomic<int64_t> monotonic{1000000}, wall_seconds{fixture::initial_wall};
std::atomic<uint64_t> consumer_wait_count{0};
std::vector<std::unique_ptr<HostTask>> tasks;
thread_local HostTask *current_task = nullptr;
struct StopTask {};
std::mutex http_mutex, nvs_mutex;
std::deque<std::shared_ptr<fixture::Reply>> script;
std::vector<std::pair<fixture::Grant, std::string>> observed;
unsigned active_refresh = 0, max_refresh = 0;
std::string store_path;
fixture::Fault next_fault = fixture::Fault::none;
size_t store_capacity = 24576;
struct NvsState { int mode; bool staged = false, erased = false; std::vector<unsigned char> bytes; };
std::map<nvs_handle_t, NvsState> handles;
nvs_handle_t next_handle = 1;
void secret_clear(std::string &s) {
    volatile char *p = s.empty() ? nullptr : &s[0];
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}
void bytes_clear(std::vector<unsigned char> &v) {
    volatile unsigned char *p = v.data();
    for (size_t i = 0; i < v.size(); ++i) p[i] = 0;
    v.clear();
}
std::vector<unsigned char> read_record() {
    std::ifstream stream(store_path, std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
std::string committed_refresh() {
    std::lock_guard<std::mutex> lock(nvs_mutex);
    auto bytes = read_record();
    REQUIRE(bytes.size() >= 24, "outgoing refresh has an actual committed NVS record");
    auto length = [&](size_t offset) {
        uint32_t n = 0;
        for (unsigned i = 0; i < 4; ++i) n |= static_cast<uint32_t>(bytes[offset + i]) << (8 * i);
        return static_cast<size_t>(n);
    };
    const size_t a = length(4), r = length(8);
    REQUIRE(a <= 8192 && r <= 8192 && 24 + a + r <= bytes.size(), "committed refresh fields are bounded");
    std::string result(bytes.begin() + 24 + a, bytes.begin() + 24 + a + r);
    bytes_clear(bytes);
    return result;
}
void key(nvs_handle_t h, const char *name) {
    REQUIRE(handles.count(h) && !std::strcmp(name, "credentials"), "NVS uses its own credentials key and valid handle");
}
bool consume(fixture::Fault fault) {
    if (next_fault != fault) return false;
    next_fault = fixture::Fault::none;
    return true;
}
std::string field(const cJSON *root, const char *name) {
    auto *value = cJSON_GetObjectItemCaseSensitive(root, name);
    REQUIRE(cJSON_IsString(value), "HTTP grant contains required string field");
    return value->valuestring;
}
void validate_grant(HostHttp *h) {
    REQUIRE(h->headers["Accept"] == "application/json" && h->headers["Accept-Encoding"] == "identity", "HTTP requests bounded uncompressed JSON");
    REQUIRE(h->headers.count("Authorization") == 0, "OAuth does not send bearer tokens in headers");
    const auto grant = h->response->grant;
    if (grant == fixture::Grant::authorization) {
        REQUIRE(h->url == "https://auth.openai.com/oauth/token" && h->headers["Content-Type"] == "application/x-www-form-urlencoded", "official authorization exchange uses form encoding");
        REQUIRE(h->body == "grant_type=authorization_code&client_id=app_EMoamEEZ73f0CkXaXp7hrann&code=synthetic%2Bcode%2F%3D&redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback&code_verifier=abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ", "authorization grant has correct client callback verifier and percent escaping");
    } else {
        REQUIRE(h->headers["Content-Type"] == "application/json", "device and refresh grants use JSON");
        cJSON *json = cJSON_Parse(h->body.c_str());
        REQUIRE(json && cJSON_IsObject(json), "outgoing OAuth JSON is well formed");
        if (grant == fixture::Grant::usercode) {
            REQUIRE(h->url == "https://auth.openai.com/api/accounts/deviceauth/usercode" && field(json, "client_id") == "app_EMoamEEZ73f0CkXaXp7hrann" && cJSON_GetArraySize(json) == 1, "official usercode request uses only pinned client id");
        } else if (grant == fixture::Grant::poll) {
            REQUIRE(h->url == "https://auth.openai.com/api/accounts/deviceauth/token" && field(json, "device_auth_id") == "synthetic-device" && field(json, "user_code") == "TEST-CODE" && cJSON_GetArraySize(json) == 2, "official poll request carries provider device and user codes");
        } else {
            REQUIRE(h->url == "https://auth.openai.com/oauth/token" && field(json, "grant_type") == "refresh_token" && field(json, "client_id") == "app_EMoamEEZ73f0CkXaXp7hrann" && field(json, "refresh_token") == committed_refresh() && cJSON_GetArraySize(json) == 3, "refresh uses the exact actual committed synthetic refresh grant");
        }
        cJSON_Delete(json);
    }
}
void observe(HostHttp *h) {
    if (h->recorded) return;
    REQUIRE(h->body.size() == h->expected, "HTTP sends complete declared request body");
    validate_grant(h);
    std::lock_guard<std::mutex> lock(http_mutex);
    observed.emplace_back(h->response->grant, h->body);
    h->recorded = true;
    if (h->response->grant == fixture::Grant::refresh) {
        h->active_refresh = true;
        max_refresh = std::max(max_refresh, ++active_refresh);
    }
    h->response->seen.store(true);
}
bool hold(HostHttp *h) {
    // Model blocking inside the custom transport, not an instant mock reply.
    while (h->response->held.load() && !h->transport->owner->stopped())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return h->transport->owner->stopped();
}
}
// This counter is only a scheduler barrier for concurrent consumer scenarios;
// their behavioral assertions remain on public results and observed grants.
uint64_t fixture::consumer_waits() { return consumer_wait_count.load(); }
namespace fixture {
[[noreturn]] void fail(const char *description, int line) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, description);
    std::abort();
}
void initialize(const char *path) { store_path = path; }
void shutdown() {
    for (auto &task : tasks) {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->stop = true; task->cv.notify_all();
    }
    for (auto &task : tasks) if (task->thread.joinable()) task->thread.join();
    for (auto &grant : observed) secret_clear(grant.second);
    observed.clear();
}
void wall(int64_t seconds) { wall_seconds.store(seconds); }
int64_t wall() { return wall_seconds.load(); }
void advance(int64_t microseconds) { monotonic.fetch_add(microseconds); }
void await(const std::function<bool()> &predicate, const char *description) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        REQUIRE(std::chrono::steady_clock::now() < end, description);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void fault(Fault value) { std::lock_guard<std::mutex> lock(nvs_mutex); next_fault = value; }
void capacity(size_t bytes) { std::lock_guard<std::mutex> lock(nvs_mutex); store_capacity = bytes; }
std::vector<unsigned char> record() { std::lock_guard<std::mutex> lock(nvs_mutex); return read_record(); }
void corrupt_record() {
    std::ofstream file(store_path, std::ios::binary | std::ios::trunc);
    file << "corrupt synthetic saved record";
    REQUIRE(file.good(), "fixture writes corrupt record");
}
std::shared_ptr<Reply> reply(Grant grant, int status, std::string body, bool held, bool hold_read) {
    auto response = std::make_shared<Reply>();
    response->grant = grant; response->status = status; response->body = std::move(body);
    response->held.store(held); response->hold_read = hold_read;
    std::lock_guard<std::mutex> lock(http_mutex); script.push_back(response);
    return response;
}
void release(const std::shared_ptr<Reply> &r) { r->held.store(false); }
size_t grants(Grant grant) {
    std::lock_guard<std::mutex> lock(http_mutex);
    return std::count_if(observed.begin(), observed.end(), [grant](const auto &v) { return v.first == grant; });
}
unsigned maximum_active_refresh() { std::lock_guard<std::mutex> lock(http_mutex); return max_refresh; }
void drained() { std::lock_guard<std::mutex> lock(http_mutex); REQUIRE(script.empty(), "all expected HTTP grants executed"); }
std::string jwt(const std::string &account, int64_t expiry) {
    const std::string payload = "{\"exp\":" + std::to_string(expiry) + ",\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"" + account + "\"}}";
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string encoded; unsigned bits = 0, available = 0;
    for (unsigned char c : payload) {
        bits = (bits << 8) | c; available += 8;
        while (available >= 6) { available -= 6; encoded += alphabet[(bits >> available) & 63]; }
    }
    if (available) encoded += alphabet[(bits << (6 - available)) & 63];
    return "e30." + encoded + ".c3ludGhldGlj";
}
std::string token_response(const std::string &account, int64_t expiry, const std::string &refresh) {
    return "{\"access_token\":\"" + jwt(account, expiry) + "\",\"refresh_token\":\"" + refresh + "\",\"id_token\":\"" + jwt(account, expiry) + "\"}";
}
std::string usercode_response() { return "{\"device_auth_id\":\"synthetic-device\",\"user_code\":\"TEST-CODE\",\"interval\":\"1\"}"; }
std::string approval_response() { return "{\"authorization_code\":\"synthetic+code/=\",\"code_verifier\":\"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ\"}"; }
}
extern "C" time_t __wrap_time(time_t *out) {
    const auto value = static_cast<time_t>(wall_seconds.load());
    if (out) *out = value;
    return value;
}
int64_t esp_timer_get_time() { return monotonic.load(); }
BaseType_t xTaskCreate(void (*entry)(void *), const char *, uint32_t, void *arg, unsigned, TaskHandle_t *out) {
    auto task = std::make_unique<HostTask>(); auto *raw = task.get();
    raw->thread = std::thread([raw, entry, arg] { current_task = raw; try { entry(arg); } catch (const StopTask &) {} });
    if (out) *out = raw;
    tasks.push_back(std::move(task)); return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task) {
    std::lock_guard<std::mutex> lock(task->mutex); ++task->notifications; task->cv.notify_all();
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) {
    REQUIRE(current_task && ticks == portMAX_DELAY, "worker waits using real task notification");
    std::unique_lock<std::mutex> lock(current_task->mutex);
    current_task->cv.wait(lock, [] { return current_task->stop || current_task->notifications; });
    if (current_task->stop) throw StopTask{};
    const unsigned result = current_task->notifications;
    if (clear) current_task->notifications = 0; else --current_task->notifications;
    return result;
}
void vTaskDelay(TickType_t ticks) {
    // Scheduling yields never move either clock. Scenarios explicitly advance
    // monotonic time, independently from provider/TLS wall time.
    if (!current_task) {
        consumer_wait_count.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); return;
    }
    std::unique_lock<std::mutex> lock(current_task->mutex);
    current_task->cv.wait_for(lock, std::chrono::milliseconds(std::min<TickType_t>(ticks, 1)), [] { return current_task->stop; });
    if (current_task->stop) throw StopTask{};
}
void vTaskDelete(TaskHandle_t task) {
    { std::lock_guard<std::mutex> lock(task->mutex); task->stop = true; task->cv.notify_all(); }
    if (task->thread.joinable()) task->thread.join();
}
esp_err_t esp_crt_bundle_attach(void *) { return ESP_OK; }
void esp_log_level_set(const char *tag, int level) {
    REQUIRE(!std::strcmp(tag, "HTTP_CLIENT") && level == ESP_LOG_WARN, "production disables token-bearing HTTP debug logs");
}
BoundedHttps::BoundedHttps(const std::atomic<bool> &cancelled, uint32_t timeout_ms,
                         uint64_t deadline_us, bool (*cancelled_call)(void *), void *call_context)
    : cancelled_(cancelled),
      deadline_(deadline_us ? std::min<uint64_t>(deadline_us, esp_timer_get_time() + uint64_t(timeout_ms) * 1000)
                            : esp_timer_get_time() + uint64_t(timeout_ms) * 1000),
      cancelled_call_(cancelled_call), call_context_(call_context) {}
BoundedHttps::~BoundedHttps() { delete wrapper_; }
bool BoundedHttps::cancelled() const {
    return cancelled_.load() || (cancelled_call_ && cancelled_call_(call_context_));
}
bool BoundedHttps::stopped() const { return cancelled() || esp_timer_get_time() >= deadline_; }
int BoundedHttps::wait_ms() const {
    const int64_t remaining = deadline_ - esp_timer_get_time();
    return cancelled() || remaining <= 0 ? 0 : static_cast<int>(std::min<int64_t>(100, (remaining + 999) / 1000));
}
const char *BoundedHttps::stop_reason() const { return cancelled() ? "cancelled" : "deadline"; }
bool BoundedHttps::initialize(const char *url, std::string &error) {
    if (!url || std::strncmp(url, "https://auth.openai.com/", 24) || wall_seconds.load() < 1704067200 || stopped()) {
        error = "fixture transport rejected insecure or expired operation"; return false;
    }
    wrapper_ = new esp_transport_item_t{this}; return true;
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    REQUIRE(config->method == HTTP_METHOD_POST && config->transport_type == HTTP_TRANSPORT_OVER_SSL && config->transport, "auth uses custom bounded HTTPS transport");
    REQUIRE(config->crt_bundle_attach == esp_crt_bundle_attach && !config->skip_cert_common_name_check, "auth requires CA bundle and hostname validation");
    REQUIRE(config->is_async && config->timeout_ms == 100 && config->disable_auto_redirect && config->max_authorization_retries == -1, "HTTP is asynchronous with bounded waits no redirects or credential retries");
    REQUIRE(config->buffer_size == 1024 && config->buffer_size_tx == 1024, "HTTP buffers remain bounded");
    std::lock_guard<std::mutex> lock(http_mutex);
    REQUIRE(!script.empty(), "unexpected provider HTTP request");
    auto *h = new HostHttp;
    h->url = config->url; h->transport = config->transport; h->response = script.front(); script.pop_front(); return h;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) {
    std::lock_guard<std::mutex> lock(http_mutex);
    if (h->active_refresh) --active_refresh;
    h->response->closed.store(true); secret_clear(h->body); delete h; return ESP_OK;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h, const char *key, const char *value) { h->headers[key] = value; return ESP_OK; }
esp_err_t esp_http_client_open(esp_http_client_handle_t h, int bytes) {
    REQUIRE(bytes > 0 && bytes < 32768, "request body has bounded positive size"); h->expected = bytes;
    if (h->retry_open) { h->retry_open = false; return ESP_ERR_HTTP_EAGAIN; }
    return h->transport->owner->stopped() ? ESP_FAIL : ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t h, const char *body, int size) {
    if (h->transport->owner->stopped()) return -1;
    if (h->retry_write) { h->retry_write = false; return -ESP_ERR_HTTP_EAGAIN; }
    const int count = std::min(size, 17); h->body.append(body, count); return count;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) {
    observe(h);
    if (!h->response->hold_read && hold(h)) return -1;
    return h->response->body.size();
}
int esp_http_client_get_status_code(esp_http_client_handle_t h) { return h->response->status; }
int esp_http_client_read(esp_http_client_handle_t h, char *out, int wanted) {
    if (h->response->hold_read && hold(h)) return -1;
    if (h->transport->owner->stopped()) return -1;
    const size_t count = std::min<size_t>({static_cast<size_t>(wanted), 11, h->response->body.size() - h->offset});
    std::memcpy(out, h->response->body.data() + h->offset, count); h->offset += count; return static_cast<int>(count);
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h) { return h->offset == h->response->body.size(); }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *out) {
    std::lock_guard<std::mutex> lock(nvs_mutex);
    REQUIRE(!std::strcmp(name, "codex_auth"), "auth never touches unrelated namespaces");
    if (mode == NVS_READONLY && access(store_path.c_str(), F_OK)) return ESP_ERR_NVS_NOT_FOUND;
    *out = next_handle++; handles.emplace(*out, NvsState{mode, false, false, {}}); return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *name, void *out, size_t *size) {
    std::lock_guard<std::mutex> lock(nvs_mutex); key(h, name);
    if (access(store_path.c_str(), F_OK)) return ESP_ERR_NVS_NOT_FOUND;
    auto bytes = read_record();
    if (out && *size < bytes.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    if (out) std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size(); bytes_clear(bytes); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *name, const void *data, size_t size) {
    std::lock_guard<std::mutex> lock(nvs_mutex); key(h, name);
    auto &entry = handles.at(h);
    REQUIRE(entry.mode == NVS_READWRITE && size <= 24 + 8192 * 2 + 128, "NVS blob write stays bounded");
    if (consume(fixture::Fault::stage)) return ESP_FAIL;
    // Rotation requires both old and new blobs plus conservative overhead;
    // capacity represents this namespace's share of the board's 24KiB NVS.
    if (read_record().size() + size + 128 > store_capacity) return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    const auto *bytes = static_cast<const unsigned char *>(data);
    entry.bytes.assign(bytes, bytes + size); entry.staged = true; entry.erased = false; return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *name) {
    std::lock_guard<std::mutex> lock(nvs_mutex); key(h, name);
    REQUIRE(handles.at(h).mode == NVS_READWRITE, "erase requires writable NVS");
    if (consume(fixture::Fault::stage)) return ESP_FAIL;
    handles.at(h).staged = true; handles.at(h).erased = true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    std::lock_guard<std::mutex> lock(nvs_mutex); auto &entry = handles.at(h);
    REQUIRE(entry.staged, "NVS commit follows staged mutation");
    if (consume(fixture::Fault::commit)) return ESP_FAIL;
    if (entry.erased) {
        if (unlink(store_path.c_str()) && access(store_path.c_str(), F_OK) == 0) return ESP_FAIL;
    } else {
        const std::string temp = store_path + ".staged";
        FILE *file = std::fopen(temp.c_str(), "wb");
        REQUIRE(file, "fixture opens temporary NVS record");
        bool ok = std::fwrite(entry.bytes.data(), 1, entry.bytes.size(), file) == entry.bytes.size();
        if (std::fflush(file) || fsync(fileno(file))) ok = false;
        if (std::fclose(file)) ok = false;
        if (!ok || std::rename(temp.c_str(), store_path.c_str())) { unlink(temp.c_str()); return ESP_FAIL; }
    }
    entry.staged = false; return ESP_OK;
}
void nvs_close(nvs_handle_t h) {
    std::lock_guard<std::mutex> lock(nvs_mutex); auto found = handles.find(h);
    REQUIRE(found != handles.end(), "NVS closes valid handle");
    bytes_clear(found->second.bytes); handles.erase(found);
}
// NVS fixture limitation: stage/commit failures are deterministic transaction
// failures, NOT ESP-IDF flash emulation. Real nvs_set_blob may write flash before
// nvs_commit, and power loss/commit failure may leave old OR new data readable.
// Atomic file rename and retained committed bytes prove API publication order;
// they cannot prove real flash power-loss atomicity, wear or page accounting.
