#include "codex_auth.hpp"
#include "bounded_https.hpp"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>

namespace {
// Provider protocol pinned to Codex 72f999edc7848961b99c4b98bf1e62d12fa35798:
// login/src/device_code_auth.rs, server.rs, oauth/client.rs and auth/manager.rs.
// Authorization-code grants are FORM encoded; ChatGPT refresh grants are JSON.
constexpr char client_id[] = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr char usercode_url[] = "https://auth.openai.com/api/accounts/deviceauth/usercode";
constexpr char poll_url[] = "https://auth.openai.com/api/accounts/deviceauth/token";
constexpr char token_url[] = "https://auth.openai.com/oauth/token";
constexpr char callback_url[] = "https://auth.openai.com/deviceauth/callback";
constexpr size_t token_limit = 8192, account_limit = 128, response_limit = 32768;
constexpr size_t record_limit = 24 + token_limit * 2 + account_limit;
constexpr int64_t login_duration = 900000000;
constexpr char storage_error[] = "Codex credential storage failed; saved account was not changed. Check NVS space and retry.";
constexpr char invalid_response[] = "OpenAI returned an invalid or oversized authentication response; retry Connect Codex.";
constexpr char network_error[] = "Codex verified HTTPS failed or exceeded its 45-second deadline; check Wi-Fi and device clock, then retry.";
constexpr char cancelled_error[] = "Codex authentication request cancelled.";
constexpr char expired_error[] = "Codex sign-in code expired after 15 minutes; select Connect Codex again.";
constexpr char clock_error[] = "Device clock is not synchronized; connect Wi-Fi and retry Codex sign-in.";
constexpr char provider_error[] = "OpenAI rejected device sign-in; enable device-code login in your ChatGPT account or workspace and retry.";
constexpr char refresh_error[] = "OpenAI could not refresh Codex credentials; reconnect your ChatGPT account.";

void wipe(void *memory, size_t bytes) {
    volatile unsigned char *p = static_cast<volatile unsigned char *>(memory);
    while (bytes--) *p++ = 0;
}
void wipe_string(std::string &s) {
    if (!s.empty()) wipe(&s[0], s.size());
    s.clear();
}
struct Secret {
    std::string value;
    explicit Secret(size_t capacity = response_limit) { value.reserve(capacity); }
    ~Secret() { wipe_string(value); }
    Secret(const Secret &) = delete;
    Secret &operator=(const Secret &) = delete;
};
struct Buffer {
    char *data;
    size_t capacity;
    explicit Buffer(size_t size = 0) : data(size ? static_cast<char *>(heap_caps_malloc(
        size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) : nullptr), capacity(size) {}
    bool allocate(size_t size) {
        if (data) return false;
        capacity = size;
        data = static_cast<char *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return data != nullptr;
    }
    ~Buffer() { if (data) { wipe(data, capacity); heap_caps_free(data); } }
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
};
struct Tokens {
    Secret access{token_limit}, refresh{token_limit}, account{account_limit};
    int64_t expires = 0;
    void clear() {
        wipe_string(access.value); wipe_string(refresh.value); wipe_string(account.value);
        expires = 0;
    }
    void swap(Tokens &other) {
        access.value.swap(other.access.value); refresh.value.swap(other.refresh.value);
        account.value.swap(other.account.value); std::swap(expires, other.expires);
    }
};
struct Operation {
    std::atomic<bool> stop{false};
    const std::atomic<bool> *external = nullptr;
    int64_t deadline = 0;
};
std::mutex state_mutex, refresh_mutex;
bool init_attempted = false, initialized = false, configured = false, busy = false;
bool storage_load_failed = false;
Tokens saved;
CodexAuthStatus public_status{};
uint64_t login_generation = 0, account_generation = 0;
int64_t code_deadline = 0;
Operation *login_operation = nullptr, *refresh_operation = nullptr;
TaskHandle_t login_task = nullptr;

void copy_text(char *dest, size_t capacity, const char *source) {
    const size_t size = std::min(capacity - 1, std::strlen(source));
    std::memcpy(dest, source, size); dest[size] = 0;
}
void state(const char *name, const char *error = "") {
    copy_text(public_status.state, sizeof(public_status.state), name);
    copy_text(public_status.error, sizeof(public_status.error), error);
    wipe(public_status.user_code, sizeof(public_status.user_code));
    code_deadline = 0;
}
void idle_state() { state(configured ? "connected" : "signed_out"); }
void expire_login() {
    if (busy && code_deadline && esp_timer_get_time() >= code_deadline) {
        if (login_operation) login_operation->stop.store(true);
        ++login_generation; busy = false; state("error", expired_error);
    }
}
void watchdog(void *) {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            const int64_t now = esp_timer_get_time();
            for (Operation *op : {login_operation, refresh_operation}) {
                if (op && ((op->external && op->external->load()) ||
                           (op->deadline && now >= op->deadline))) op->stop.store(true);
            }
            expire_login();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
// Registry lifetime is protected by state_mutex; the watchdog never retains a
// pointer after unlocking. HTTP owns its transport on one task only. Logout and
// account replacement interrupt HTTP, including BoundedHttps's internal loops.
struct RegisteredOperation {
    Operation &op;
    Operation *&slot;
    RegisteredOperation(Operation &operation, Operation *&where) : op(operation), slot(where) {}
    ~RegisteredOperation() {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (slot == &op) slot = nullptr;
    }
};
bool wait_clock(Operation &op, std::string &error) {
    const int64_t deadline = std::min(op.deadline, esp_timer_get_time() + 30000000);
    while (std::time(nullptr) < 1704067200) {
        if (op.stop.load() || (op.external && op.external->load())) {
            error = cancelled_error; return false;
        }
        if (esp_timer_get_time() >= deadline) { error = clock_error; return false; }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (op.stop.load() || (op.external && op.external->load())) {
        error = cancelled_error; return false;
    }
    return true;
}

// Bound both recursion and allocation before cJSON sees provider-controlled
// data. Reject embedded NULs (including escaped NUL) so a parsed C string
// cannot silently truncate a credential or key.
bool json_envelope(const char *data, size_t size) {
    unsigned depth = 0, structure = 0;
    bool in_string = false;
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = data[i];
        if (!c) return false;
        if (!in_string && (c == '{' || c == '[' || c == ',' || c == ':') &&
            ++structure > 256) return false;
        if (in_string) {
            if (c < 0x20) return false;
            if (c == '\\') {
                if (++i == size) return false;
                if (data[i] == 'u' && i + 4 < size &&
                    std::memcmp(data + i + 1, "0000", 4) == 0) return false;
            } else if (c == '"') in_string = false;
        } else if (c == '"') in_string = true;
        else if (c == '{' || c == '[') { if (++depth > 12) return false; }
        else if (c == '}' || c == ']') { if (!depth) return false; --depth; }
    }
    return !in_string && !depth;
}
bool json_tree_valid(const cJSON *node, unsigned &count) {
    for (; node; node = node->next) {
        if (++count > 256) return false;
        if (cJSON_IsObject(node)) {
            for (const cJSON *child = node->child; child; child = child->next)
                for (const cJSON *other = child->next; other; other = other->next)
                    if (child->string && other->string &&
                        !std::strcmp(child->string, other->string)) return false;
        }
        if (!json_tree_valid(node->child, count)) return false;
    }
    return true;
}
// cJSON's ordinary parser frees partially parsed strings without wiping them
// on malformed JSON/OOM. Own every node and decoded string in bounded,
// wipe-on-destruction PSRAM arenas, including failure paths. Nodes retain
// cJSON's layout for its read-only access/type helpers only.
struct Json {
    cJSON *root = nullptr;
    Buffer text, nodes;
    char *cursor = nullptr;
    size_t used = 0;
    void whitespace() {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') ++cursor;
    }
    bool hex4(uint32_t &code) {
        code = 0;
        for (unsigned i = 0; i < 4; ++i) {
            const char c = *cursor;
            int value = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (value < 0) return false;
            ++cursor; code = code * 16 + static_cast<unsigned>(value);
        }
        return true;
    }
    static void utf8(char *&out, uint32_t code) {
        if (code <= 0x7f) *out++ = static_cast<char>(code);
        else if (code <= 0x7ff) {
            *out++ = static_cast<char>(0xc0 | (code >> 6));
            *out++ = static_cast<char>(0x80 | (code & 63));
        } else if (code <= 0xffff) {
            *out++ = static_cast<char>(0xe0 | (code >> 12));
            *out++ = static_cast<char>(0x80 | ((code >> 6) & 63));
            *out++ = static_cast<char>(0x80 | (code & 63));
        } else {
            *out++ = static_cast<char>(0xf0 | (code >> 18));
            *out++ = static_cast<char>(0x80 | ((code >> 12) & 63));
            *out++ = static_cast<char>(0x80 | ((code >> 6) & 63));
            *out++ = static_cast<char>(0x80 | (code & 63));
        }
    }
    bool string(char *&result) {
        if (*cursor++ != '"') return false;
        result = cursor;
        char *out = cursor;
        while (*cursor && *cursor != '"') {
            const unsigned char c = static_cast<unsigned char>(*cursor++);
            if (c < 0x20) return false;
            if (c == '\\') {
                const char escaped = *cursor;
                if (!escaped) return false;
                ++cursor;
                if (escaped == '"' || escaped == '\\' || escaped == '/') *out++ = escaped;
                else if (escaped == 'b') *out++ = '\b';
                else if (escaped == 'f') *out++ = '\f';
                else if (escaped == 'n') *out++ = '\n';
                else if (escaped == 'r') *out++ = '\r';
                else if (escaped == 't') *out++ = '\t';
                else if (escaped == 'u') {
                    uint32_t code;
                    if (!hex4(code) || !code) return false;
                    if (code >= 0xd800 && code <= 0xdbff) {
                        if (*cursor != '\\' || cursor[1] != 'u') return false;
                        cursor += 2;
                        uint32_t low;
                        if (!hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
                        code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
                    } else if (code >= 0xdc00 && code <= 0xdfff) return false;
                    utf8(out, code);
                } else return false;
            } else if (c < 0x80) *out++ = static_cast<char>(c);
            else {
                unsigned following;
                uint32_t code, minimum;
                if (c >= 0xc2 && c <= 0xdf) { following = 1; code = c & 31; minimum = 0x80; }
                else if (c >= 0xe0 && c <= 0xef) { following = 2; code = c & 15; minimum = 0x800; }
                else if (c >= 0xf0 && c <= 0xf4) { following = 3; code = c & 7; minimum = 0x10000; }
                else return false;
                for (unsigned i = 0; i < following; ++i) {
                    const unsigned char next = static_cast<unsigned char>(*cursor);
                    if (next < 0x80 || next > 0xbf) return false;
                    ++cursor; code = (code << 6) | (next & 63);
                }
                if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
                utf8(out, code);
            }
        }
        if (*cursor != '"') return false;
        ++cursor; *out = 0;
        return true;
    }
    bool number(cJSON *node) {
        char *start = cursor;
        if (*cursor == '-') ++cursor;
        if (*cursor == '0') ++cursor;
        else {
            if (*cursor < '1' || *cursor > '9') return false;
            do { ++cursor; } while (*cursor >= '0' && *cursor <= '9');
        }
        if (*cursor == '.') {
            ++cursor;
            if (*cursor < '0' || *cursor > '9') return false;
            do { ++cursor; } while (*cursor >= '0' && *cursor <= '9');
        }
        if (*cursor == 'e' || *cursor == 'E') {
            ++cursor;
            if (*cursor == '+' || *cursor == '-') ++cursor;
            if (*cursor < '0' || *cursor > '9') return false;
            do { ++cursor; } while (*cursor >= '0' && *cursor <= '9');
        }
        char *end;
        node->valuedouble = std::strtod(start, &end);
        if (end != cursor || !std::isfinite(node->valuedouble)) return false;
        node->type = cJSON_Number;
        return true;
    }
    bool value(cJSON *&node, unsigned depth) {
        whitespace();
        if (depth > 12 || used == 256) return false;
        node = reinterpret_cast<cJSON *>(nodes.data) + used++;
        std::memset(node, 0, sizeof(*node));
        if (*cursor == '{' || *cursor == '[') {
            const bool object = *cursor++ == '{';
            const char close = object ? '}' : ']';
            node->type = object ? cJSON_Object : cJSON_Array;
            whitespace();
            if (*cursor == close) { ++cursor; return true; }
            cJSON *last = nullptr;
            for (;;) {
                char *key = nullptr;
                if (object) {
                    if (*cursor != '"' || !string(key)) return false;
                    whitespace();
                    if (*cursor++ != ':') return false;
                }
                cJSON *child = nullptr;
                if (!value(child, depth + 1)) return false;
                child->string = key;
                child->prev = last;
                if (last) last->next = child; else node->child = child;
                last = child;
                whitespace();
                if (*cursor == close) { ++cursor; return true; }
                if (*cursor++ != ',') return false;
                whitespace();
            }
        }
        if (*cursor == '"') {
            node->type = cJSON_String;
            return string(node->valuestring);
        }
        for (const char *literal : {"true", "false", "null"}) {
            const size_t length = std::strlen(literal);
            if (!std::strncmp(cursor, literal, length)) {
                cursor += length;
                node->type = literal[0] == 't' ? cJSON_True :
                    literal[0] == 'f' ? cJSON_False : cJSON_NULL;
                return true;
            }
        }
        return number(node);
    }
    bool parse(const char *data, size_t size) {
        if (!json_envelope(data, size) || !text.allocate(size + 1) ||
            !nodes.allocate(256 * sizeof(cJSON))) return false;
        std::memcpy(text.data, data, size); text.data[size] = 0;
        cursor = text.data;
        if (!value(root, 1) || !cJSON_IsObject(root)) return false;
        whitespace();
        unsigned count = 0;
        return !*cursor && json_tree_valid(root, count);
    }
};
const cJSON *member(const cJSON *root, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(root, key);
}
bool ascii_string(const char *s, size_t limit, bool account = false) {
    if (!s) return false;
    size_t size = 0;
    for (; s[size]; ++size) {
        const unsigned char c = s[size];
        if (size >= limit || c < 0x21 || c > 0x7e) return false;
        if (account && !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                         (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return size != 0;
}
bool string_field(const cJSON *root, const char *name, Secret &out, size_t limit) {
    const cJSON *item = member(root, name);
    if (!cJSON_IsString(item) || !ascii_string(item->valuestring, limit)) return false;
    wipe_string(out.value); out.value.assign(item->valuestring); return true;
}
int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '-' ? 62 : c == '_' ? 63 : -1;
}
bool jwt_claims(const std::string &jwt, Secret &account, int64_t *expiration) {
    const size_t first = jwt.find('.');
    const size_t second = first == std::string::npos ? first : jwt.find('.', first + 1);
    if (first == 0 || first == std::string::npos || second == std::string::npos ||
        second == first + 1 || second + 1 == jwt.size() ||
        jwt.find('.', second + 1) != std::string::npos) return false;
    for (size_t i = 0; i < jwt.size(); ++i)
        if (i != first && i != second && base64_value(jwt[i]) < 0) return false;
    const size_t encoded = second - first - 1;
    if (encoded % 4 == 1) return false;
    Buffer decoded(encoded * 3 / 4 + 2);
    if (!decoded.data) return false;
    uint32_t bits = 0; unsigned available = 0; size_t size = 0;
    for (size_t i = first + 1; i < second; ++i) {
        bits = (bits << 6) | static_cast<unsigned>(base64_value(jwt[i])); available += 6;
        if (available >= 8) { available -= 8; decoded.data[size++] = static_cast<char>(bits >> available); }
    }
    if (available && (bits & ((1U << available) - 1))) return false;
    decoded.data[size] = 0;
    Json claims;
    if (!claims.parse(decoded.data, size)) return false;
    if (expiration) {
        const cJSON *exp = member(claims.root, "exp");
        if (!cJSON_IsNumber(exp) || !std::isfinite(exp->valuedouble) ||
            exp->valuedouble < 1704067200 || exp->valuedouble > 4102444800.0 ||
            std::floor(exp->valuedouble) != exp->valuedouble) return false;
        *expiration = static_cast<int64_t>(exp->valuedouble);
    }
    const cJSON *auth = member(claims.root, "https://api.openai.com/auth");
    if (auth && !cJSON_IsObject(auth)) return false;
    const cJSON *id = auth ? member(auth, "chatgpt_account_id") : nullptr;
    if (id) {
        if (!cJSON_IsString(id) || !ascii_string(id->valuestring, account_limit, true)) return false;
        wipe_string(account.value); account.value.assign(id->valuestring);
    }
    return true;
}

struct HttpClient {
    esp_http_client_handle_t handle = nullptr;
    ~HttpClient() { if (handle) esp_http_client_cleanup(handle); }
};
bool post(const char *url, const char *content_type, const Secret &body,
          Operation &op, Buffer &response, size_t &size, int &status, std::string &error) {
    size = 0; status = 0;
    if (!response.data) { error = "Not enough PSRAM for Codex authentication."; return false; }
    if (op.stop.load()) { error = cancelled_error; return false; }
    BoundedHttps transport(op.stop);
    // IDF debug logging can expose a POST body; never enable it for auth.
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
    esp_http_client_config_t config{};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.timeout_ms = 100;
    config.is_async = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.skip_cert_common_name_check = false;
    config.disable_auto_redirect = true;
    config.max_authorization_retries = -1;
    config.buffer_size = 1024; config.buffer_size_tx = 1024;
    std::string transport_error;
    if (!transport.initialize(url, transport_error)) { error = op.stop.load() ? cancelled_error : network_error; return false; }
    config.transport = transport.transport();
    HttpClient client;
    client.handle = esp_http_client_init(&config);
    if (!client.handle ||
        esp_http_client_set_header(client.handle, "Content-Type", content_type) != ESP_OK ||
        esp_http_client_set_header(client.handle, "Accept", "application/json") != ESP_OK ||
        esp_http_client_set_header(client.handle, "Accept-Encoding", "identity") != ESP_OK) {
        error = "Not enough memory for Codex HTTPS authentication."; return false;
    }
    auto failed = [&]() { error = op.stop.load() ? cancelled_error : network_error; return false; };
    esp_err_t opened;
    do {
        if (transport.stopped()) return failed();
        opened = esp_http_client_open(client.handle, static_cast<int>(body.value.size()));
        if (opened == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (opened == ESP_ERR_HTTP_EAGAIN);
    if (opened != ESP_OK) return failed();
    size_t sent = 0;
    while (sent < body.value.size()) {
        if (transport.stopped()) return failed();
        const int wanted = static_cast<int>(std::min<size_t>(1024, body.value.size() - sent));
        const int count = esp_http_client_write(client.handle, body.value.data() + sent, wanted);
        if (count <= 0 && (errno == EAGAIN || count == -ESP_ERR_HTTP_EAGAIN)) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (count <= 0 || count > wanted) return failed();
        sent += static_cast<size_t>(count);
    }
    int64_t length;
    do {
        if (transport.stopped()) return failed();
        length = esp_http_client_fetch_headers(client.handle);
        if (length == -ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(10));
    } while (length == -ESP_ERR_HTTP_EAGAIN);
    if (length < 0) return failed();
    status = esp_http_client_get_status_code(client.handle);
    if (length > static_cast<int64_t>(response_limit)) { error = invalid_response; return false; }
    for (;;) {
        if (transport.stopped()) return failed();
        const int wanted = static_cast<int>(std::min<size_t>(1024, response_limit + 1 - size));
        const int count = esp_http_client_read(client.handle, response.data + size, wanted);
        if (count == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        if (count < 0 || count > wanted) return failed();
        if (!count) {
            if (!esp_http_client_is_complete_data_received(client.handle) ||
                (length > 0 && size != static_cast<size_t>(length))) return failed();
            break;
        }
        size += static_cast<size_t>(count);
        if (size > response_limit) { error = invalid_response; return false; }
    }
    response.data[size] = 0;
    if (transport.stopped()) return failed();
    return true;
}
void json_quote(std::string &out, const std::string &text) {
    out += '"';
    for (char c : text) { if (c == '"' || c == '\\') out += '\\'; out += c; }
    out += '"';
}
void form_value(std::string &out, const std::string &text) {
    constexpr char hex[] = "0123456789ABCDEF";
    for (unsigned char c : text) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
}
bool parse_tokens(const char *data, size_t size, Tokens &tokens, bool refreshing) {
    Json json;
    if (!json.parse(data, size)) return false;
    // Refresh responses may omit ID/refresh tokens, as in the first-party
    // RefreshResponse. Access must either be replaced or still be unexpired.
    for (const char *key : {"access_token", "refresh_token"}) {
        const cJSON *field = member(json.root, key);
        Secret &dest = key[0] == 'a' ? tokens.access : tokens.refresh;
        if ((!refreshing || (field && !cJSON_IsNull(field))) &&
            !string_field(json.root, key, dest, token_limit)) return false;
    }
    const cJSON *id = member(json.root, "id_token");
    Secret access_account(account_limit);
    if (!refreshing || (id && !cJSON_IsNull(id))) {
        Secret id_token(token_limit), id_account(account_limit);
        if (!string_field(json.root, "id_token", id_token, token_limit) ||
            !jwt_claims(id_token.value, id_account, nullptr)) return false;
        if (!id_account.value.empty()) {
            if (refreshing && !tokens.account.value.empty() &&
                tokens.account.value != id_account.value) return false;
            wipe_string(tokens.account.value); tokens.account.value.assign(id_account.value);
        }
    }
    if (tokens.access.value.empty() || tokens.refresh.value.empty() ||
        !jwt_claims(tokens.access.value, access_account, &tokens.expires)) return false;
    if (!access_account.value.empty() && !tokens.account.value.empty() &&
        access_account.value != tokens.account.value) return false;
    // Account selection follows server.rs: ID-token auth.chatgpt_account_id.
    // No access-token fallback invents an account absent from the ID token.
    return tokens.expires > static_cast<int64_t>(std::time(nullptr)) + 60;
}

void put_u32(char *dest, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) dest[i] = static_cast<char>(n >> (i * 8));
}
uint32_t get_u32(const char *src) {
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= static_cast<uint32_t>(static_cast<unsigned char>(src[i])) << (i * 8);
    return n;
}
struct Nvs {
    nvs_handle_t handle = 0;
    ~Nvs() { if (handle) nvs_close(handle); }
};
bool store_tokens(const Tokens &tokens) {
    const uint32_t a = tokens.access.value.size(), r = tokens.refresh.value.size(), c = tokens.account.value.size();
    Buffer record(24 + a + r + c);
    if (!record.data) return false;
    std::memcpy(record.data, "CAX1", 4);
    put_u32(record.data + 4, a); put_u32(record.data + 8, r); put_u32(record.data + 12, c);
    const uint64_t expiry = static_cast<uint64_t>(tokens.expires);
    put_u32(record.data + 16, static_cast<uint32_t>(expiry));
    put_u32(record.data + 20, static_cast<uint32_t>(expiry >> 32));
    std::memcpy(record.data + 24, tokens.access.value.data(), a);
    std::memcpy(record.data + 24 + a, tokens.refresh.value.data(), r);
    std::memcpy(record.data + 24 + a + r, tokens.account.value.data(), c);
    Nvs nvs;
    return nvs_open("codex_auth", NVS_READWRITE, &nvs.handle) == ESP_OK &&
        nvs_set_blob(nvs.handle, "credentials", record.data, 24 + a + r + c) == ESP_OK &&
        nvs_commit(nvs.handle) == ESP_OK;
}
bool load_tokens(Tokens &tokens, bool &present) {
    present = false;
    Nvs nvs;
    esp_err_t result = nvs_open("codex_auth", NVS_READONLY, &nvs.handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return false;
    size_t size = 0;
    result = nvs_get_blob(nvs.handle, "credentials", nullptr, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK || size < 24 || size > record_limit) return false;
    Buffer record(size);
    if (!record.data || nvs_get_blob(nvs.handle, "credentials", record.data, &size) != ESP_OK ||
        std::memcmp(record.data, "CAX1", 4)) return false;
    const size_t a = get_u32(record.data + 4), r = get_u32(record.data + 8), c = get_u32(record.data + 12);
    if (!a || a > token_limit || !r || r > token_limit || c > account_limit || size != 24 + a + r + c) return false;
    tokens.access.value.assign(record.data + 24, a);
    tokens.refresh.value.assign(record.data + 24 + a, r);
    tokens.account.value.assign(record.data + 24 + a + r, c);
    if (std::memchr(tokens.access.value.data(), 0, a) || std::memchr(tokens.refresh.value.data(), 0, r) ||
        std::memchr(tokens.account.value.data(), 0, c) ||
        !ascii_string(tokens.access.value.c_str(), token_limit) ||
        !ascii_string(tokens.refresh.value.c_str(), token_limit) ||
        (c && !ascii_string(tokens.account.value.c_str(), account_limit, true))) return false;
    Secret account(account_limit);
    if (!jwt_claims(tokens.access.value, account, &tokens.expires) ||
        (!account.value.empty() && c && account.value != tokens.account.value)) return false;
    const uint64_t expiry = get_u32(record.data + 16) | (static_cast<uint64_t>(get_u32(record.data + 20)) << 32);
    if (expiry != static_cast<uint64_t>(tokens.expires)) return false;
    present = true;
    return true;
}
bool erase_tokens() {
    Nvs nvs;
    if (nvs_open("codex_auth", NVS_READWRITE, &nvs.handle) != ESP_OK) return false;
    const esp_err_t erased = nvs_erase_key(nvs.handle, "credentials");
    return (erased == ESP_OK || erased == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(nvs.handle) == ESP_OK;
}

bool interval_field(const cJSON *root, uint64_t &interval) {
    const cJSON *field = member(root, "interval");
    if (!field) { interval = 1; return true; }
    if (!cJSON_IsString(field) || !field->valuestring) return false;
    const char *p = field->valuestring;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p < '0' || *p > '9') return false;
    interval = 0;
    while (*p >= '0' && *p <= '9') {
        const unsigned digit = *p++ - '0';
        if (interval > (UINT64_MAX - digit) / 10) return false;
        interval = interval * 10 + digit;
    }
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    if (*p) return false;
    // A zero interval must not spin; positive provider intervals are never
    // shortened. An interval >900 simply waits until the code expires.
    if (!interval) interval = 1;
    return true;
}
bool run_login(uint64_t generation, Operation &op, std::string &error) {
    if (!wait_clock(op, error)) return false;
    Buffer response(response_limit + 1);
    Secret body;
    body.value = "{\"client_id\":\""; body.value += client_id; body.value += "\"}";
    size_t size; int status;
    if (!post(usercode_url, "application/json", body, op, response, size, status, error)) return false;
    if (status < 200 || status >= 300) { error = provider_error; return false; }
    Secret device_id(1024), user_code(63);
    uint64_t interval;
    {
        Json json;
        if (!json.parse(response.data, size) ||
            !string_field(json.root, "device_auth_id", device_id, 1024) ||
            !interval_field(json.root, interval)) { error = invalid_response; return false; }
        const bool has_primary = member(json.root, "user_code") != nullptr;
        if ((has_primary && member(json.root, "usercode")) ||
            !string_field(json.root, has_primary ? "user_code" : "usercode", user_code, 63)) {
            error = invalid_response; return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!busy || generation != login_generation || op.stop.load()) { error = cancelled_error; return false; }
        state("awaiting_approval");
        code_deadline = esp_timer_get_time() + login_duration;
        op.deadline = code_deadline;
        copy_text(public_status.user_code, sizeof(public_status.user_code), user_code.value.c_str());
    }
    wipe_string(body.value);
    body.value = "{\"device_auth_id\":"; json_quote(body.value, device_id.value);
    body.value += ",\"user_code\":"; json_quote(body.value, user_code.value); body.value += '}';
    Secret authorization_code(8192), verifier(128);
    for (;;) {
        if (op.stop.load()) { error = cancelled_error; return false; }
        wipe(response.data, response.capacity);
        if (!post(poll_url, "application/json", body, op, response, size, status, error)) return false;
        if (status >= 200 && status < 300) {
            Json json;
            if (!json.parse(response.data, size) ||
                !string_field(json.root, "authorization_code", authorization_code, 8192) ||
                !string_field(json.root, "code_verifier", verifier, 128)) { error = invalid_response; return false; }
            if (verifier.value.size() < 43 || !ascii_string(verifier.value.c_str(), 128, true)) {
                // RFC 7636 also permits dot and tilde; the check below handles
                // the full unreserved alphabet without accepting header bytes.
                bool valid = verifier.value.size() >= 43;
                for (char c : verifier.value)
                    if (!(base64_value(c) >= 0 || c == '.' || c == '~')) valid = false;
                if (!valid) { error = invalid_response; return false; }
            }
            break;
        }
        if (status != 403 && status != 404) { error = provider_error; return false; }
        const int64_t delay = static_cast<int64_t>(std::min<uint64_t>(interval, 900)) * 1000000;
        const int64_t until = std::min(op.deadline, esp_timer_get_time() + delay);
        while (esp_timer_get_time() < until && !op.stop.load()) vTaskDelay(pdMS_TO_TICKS(20));
        if (esp_timer_get_time() >= op.deadline) { op.stop.store(true); error = expired_error; return false; }
    }
    wipe_string(body.value);
    body.value = "grant_type=authorization_code&client_id="; body.value += client_id;
    body.value += "&code="; form_value(body.value, authorization_code.value);
    body.value += "&redirect_uri="; form_value(body.value, callback_url);
    body.value += "&code_verifier="; form_value(body.value, verifier.value);
    wipe(response.data, response.capacity);
    if (!post(token_url, "application/x-www-form-urlencoded", body, op, response, size, status, error)) return false;
    if (status < 200 || status >= 300) { error = provider_error; return false; }
    Tokens next;
    if (!parse_tokens(response.data, size, next, false)) { error = invalid_response; return false; }
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!busy || generation != login_generation || op.stop.load() || esp_timer_get_time() >= op.deadline) {
        error = cancelled_error; return false;
    }
    // The old account remains usable throughout login. One NVS blob contains
    // the whole new account; publish only after the storage commit succeeds.
    if (!store_tokens(next)) { error = storage_error; return false; }
    saved.swap(next); configured = true; busy = false; ++account_generation;
    if (++public_status.account_revision == 0) ++public_status.account_revision;
    if (refresh_operation) refresh_operation->stop.store(true);
    state("connected");
    return true;
}
void login_worker(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint64_t generation;
        Operation op;
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!busy || std::strcmp(public_status.state, "starting")) continue;
            generation = login_generation;
            op.deadline = esp_timer_get_time() + login_duration;
            login_operation = &op;
        }
        {
            RegisteredOperation registered(op, login_operation);
            std::string error;
            const bool success = run_login(generation, op, error);
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!success && generation == login_generation && busy) {
                busy = false;
                state("error", error.empty() ? provider_error : error.c_str());
            }
        }
    }
}
} // namespace

CodexCredentials::CodexCredentials() = default;
CodexCredentials::~CodexCredentials() { wipe_string(access_token); wipe_string(account_id); }

bool codex_auth_init(std::string &error) {
    error.clear();
    std::lock_guard<std::mutex> lock(state_mutex);
    if (init_attempted) {
        if (!initialized || storage_load_failed) error = public_status.error;
        return initialized && !storage_load_failed;
    }
    init_attempted = true;
    idle_state();
    Tokens loaded;
    bool present = false;
    storage_load_failed = !load_tokens(loaded, present);
    if (xTaskCreate(login_worker, "codex_login", 8192, nullptr, 3, &login_task) != pdPASS) {
        state("error", "Codex authentication task allocation failed; restart the device.");
        error = public_status.error; return false;
    }
    if (xTaskCreate(watchdog, "codex_auth_watch", 3072, nullptr, 3, nullptr) != pdPASS) {
        // The worker is only waiting for a notification at this point.
        vTaskDelete(login_task); login_task = nullptr;
        state("error", "Codex authentication task allocation failed; restart the device.");
        error = public_status.error; return false;
    }
    initialized = true;
    if (storage_load_failed) {
        state("error", "Codex saved credentials could not be loaded; sign out in Settings, then reconnect.");
        error = public_status.error;
        return false;
    }
    saved.swap(loaded); configured = present; idle_state();
    if (present) public_status.account_revision = 1;
    return true;
}
CodexAuthStatus codex_auth_status() {
    std::lock_guard<std::mutex> lock(state_mutex);
    expire_login();
    CodexAuthStatus result = public_status;
    if (!result.state[0]) copy_text(result.state, sizeof(result.state), "signed_out");
    result.configured = configured; result.busy = busy;
    const int64_t remaining = code_deadline - esp_timer_get_time();
    result.expires_in = busy && code_deadline && remaining > 0 ?
        static_cast<unsigned>(std::min<int64_t>(900, (remaining + 999999) / 1000000)) : 0;
    return result;
}
bool codex_auth_begin(std::string &error) {
    if (!codex_auth_init(error)) return false;
    std::lock_guard<std::mutex> lock(state_mutex);
    expire_login();
    if (busy) { error = "Codex sign-in is already pending; finish or cancel it first."; return false; }
    ++login_generation; busy = true; state("starting");
    xTaskNotifyGive(login_task);
    return true;
}
void codex_auth_cancel() {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!busy) return;
    ++login_generation; busy = false;
    if (login_operation) login_operation->stop.store(true);
    idle_state();
}
bool codex_auth_disconnect(std::string &error) {
    const bool ready = codex_auth_init(error);
    std::lock_guard<std::mutex> lock(state_mutex);
    // A damaged saved record must remain recoverable through an explicit,
    // committed sign-out, without loading it again or erasing unrelated NVS.
    if (!ready && !initialized) return false;
    if (!erase_tokens()) {
        error = storage_error;
        if (!busy) state("error", storage_error);
        return false;
    }
    // Linearization point: storage committed under the same lock used by both
    // publishers. No pre-logout login/refresh can resurrect the saved account.
    ++account_generation; ++login_generation;
    if (++public_status.account_revision == 0) ++public_status.account_revision;
    if (login_operation) login_operation->stop.store(true);
    if (refresh_operation) refresh_operation->stop.store(true);
    saved.clear(); configured = false; busy = false; storage_load_failed = false;
    error.clear(); state("signed_out");
    return true;
}
bool codex_auth_credentials(CodexCredentials &out, const std::atomic<bool> &cancelled,
                            std::string &error) {
    wipe_string(out.access_token); wipe_string(out.account_id);
    if (cancelled.load()) { error = cancelled_error; return false; }
    if (!codex_auth_init(error)) return false;
    const int64_t deadline = esp_timer_get_time() + 90000000;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!configured) { error = "Connect a ChatGPT account in Settings before using Codex voice."; return false; }
        generation = account_generation;
    }
    // Waiting for another refresh is cancellable and bounded; no HTTP task
    // waits behind a mutex while holding the status/storage lock.
    std::unique_lock<std::mutex> refresh_lock(refresh_mutex, std::defer_lock);
    while (!refresh_lock.try_lock()) {
        if (cancelled.load()) { error = cancelled_error; return false; }
        if (esp_timer_get_time() >= deadline) { error = "Codex credential refresh is busy; retry shortly."; return false; }
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!configured || generation != account_generation) { error = cancelled_error; return false; }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    Operation op;
    op.external = &cancelled; op.deadline = deadline;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!configured || generation != account_generation || cancelled.load()) { error = cancelled_error; return false; }
        refresh_operation = &op;
    }
    RegisteredOperation registered(op, refresh_operation);
    if (!wait_clock(op, error)) return false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!configured || generation != account_generation || cancelled.load() || op.stop.load() ||
            esp_timer_get_time() >= deadline) {
            error = cancelled_error; return false;
        }
        if (saved.expires > static_cast<int64_t>(std::time(nullptr)) + 120) {
            out.access_token = saved.access.value; out.account_id = saved.account.value;
            if (cancelled.load()) { wipe_string(out.access_token); wipe_string(out.account_id); error = cancelled_error; return false; }
            return true;
        }
    }
    Tokens next;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!configured || generation != account_generation || cancelled.load() || op.stop.load() ||
            esp_timer_get_time() >= deadline) {
            error = cancelled_error; return false;
        }
        next.access.value.assign(saved.access.value); next.refresh.value.assign(saved.refresh.value);
        next.account.value.assign(saved.account.value); next.expires = saved.expires;
    }
    Secret body;
    body.value = "{\"grant_type\":\"refresh_token\",\"client_id\":\"";
    body.value += client_id; body.value += "\",\"refresh_token\":";
    json_quote(body.value, next.refresh.value); body.value += '}';
    Buffer response(response_limit + 1);
    size_t size; int status;
    bool ok = post(token_url, "application/json", body, op, response, size, status, error);
    if (ok && (status < 200 || status >= 300)) { error = refresh_error; ok = false; }
    if (ok && !parse_tokens(response.data, size, next, true)) { error = invalid_response; ok = false; }
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!configured || generation != account_generation || op.stop.load() || cancelled.load() ||
        esp_timer_get_time() >= deadline) { error = cancelled_error; return false; }
    if (!ok) { if (!busy) state("error", error.c_str()); return false; }
    if (!store_tokens(next)) {
        error = storage_error; if (!busy) state("error", storage_error); return false;
    }
    saved.swap(next);
    if (!busy) state("connected");
    if (cancelled.load()) { error = cancelled_error; return false; }
    out.access_token = saved.access.value; out.account_id = saved.account.value;
    if (cancelled.load()) { wipe_string(out.access_token); wipe_string(out.account_id); error = cancelled_error; return false; }
    return true;
}
