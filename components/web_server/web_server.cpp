#include "web_server.h"
#include "WWWData.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "ai_chat_bsp.h"
#include "esp_wifi_bsp.h"
#include "web_auth.h"
#include "codex_auth.hpp"
#include "speech_live.hpp"
#include "conversation_store.hpp"
#include "desk_buddy.h"
#include "mbedtls/platform_util.h"
#include <cmath>
#include <algorithm>
#include <memory>
#include <new>
#include <string>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>

static const char *TAG = "web_server";

// Forward declarations
static esp_err_t www_handler(httpd_req_t *req);
static esp_err_t api_system_status_handler(httpd_req_t *req);
static esp_err_t api_wifi_status_handler(httpd_req_t *req);
static esp_err_t api_features_handler(httpd_req_t *req);
static esp_err_t api_system_status_simple_handler(httpd_req_t *req);
static esp_err_t api_wifi_status_simple_handler(httpd_req_t *req);
static esp_err_t api_ap_status_handler(httpd_req_t *req);
static esp_err_t api_stub_handler(httpd_req_t *req);
static esp_err_t api_wifi_settings_get_handler(httpd_req_t *req);
static esp_err_t api_wifi_settings_post_handler(httpd_req_t *req);
static esp_err_t api_wifi_scan_handler(httpd_req_t *req);
static esp_err_t api_options_handler(httpd_req_t *req);
static esp_err_t api_restart_handler(httpd_req_t *req);
static esp_err_t api_factory_reset_handler(httpd_req_t *req);
static esp_err_t api_agent_get_handler(httpd_req_t *req);
static esp_err_t api_agent_chat_handler(httpd_req_t *req);
static esp_err_t api_agent_reset_handler(httpd_req_t *req);
static esp_err_t api_agent_options_handler(httpd_req_t *req);
static esp_err_t api_agent_settings_get_handler(httpd_req_t *req);
static esp_err_t api_agent_settings_post_handler(httpd_req_t *req);

static esp_err_t agent_response(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static bool legacy_window_allowed(httpd_req_t *req) {
    // Socket local address, not Host/Origin/client JSON, establishes AP access.
    if (espwifi_provisioning_request_allowed(httpd_req_to_sockfd(req))) return true;
    agent_response(req, "403 Forbidden", "{\"error\":\"physical_local_access_required\"}");
    return false;
}

// This server serves HTTP only. Browser writes must originate at the same host
// and port; requests without Origin (e.g. a local command-line client) are allowed.
static bool agent_same_origin(httpd_req_t *req)
{
    const size_t origin_len = httpd_req_get_hdr_value_len(req, "Origin");
    if (origin_len == 0) return true;
    const size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
    if (origin_len >= 256 || host_len == 0 || host_len >= 248) return false;
    char origin[256];
    char host[248];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    if (strncmp(origin, "http://", 7) != 0) return false;
    const char *authority = origin + 7;
    if (strchr(authority, '/') || strchr(authority, '?') || strchr(authority, '#') ||
        strchr(authority, '@')) return false;
    if (strcasecmp(authority, host) == 0) return true;
    // Browsers omit the default port in serialized Origin.
    return host_len > 3 && strcmp(host + host_len - 3, ":80") == 0 &&
           strlen(authority) == host_len - 3 &&
           strncasecmp(authority, host, host_len - 3) == 0;
}

static bool agent_valid_utf8(const char *text)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
    while (*p) {
        const unsigned char first = *p++;
        if (first < 0x80) continue;
        unsigned continuation;
        uint32_t code;
        uint32_t minimum;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1; code = first & 0x1f; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2; code = first & 0x0f; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3; code = first & 0x07; minimum = 0x10000;
        } else {
            return false;
        }
        while (continuation--) {
            if ((*p & 0xc0) != 0x80) return false;
            code = (code << 6) | (*p++ & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}

static void wipe_private_json(cJSON *node)
{
    for (; node; node = node->next) {
        if (node->child) wipe_private_json(node->child);
        if (node->valuestring) mbedtls_platform_zeroize(node->valuestring, strlen(node->valuestring));
        if (node->string) mbedtls_platform_zeroize(node->string, strlen(node->string));
    }
}

static cJSON *agent_read_json(httpd_req_t *req, size_t max_body = 9 * 1024)
{
    if (req->content_len == 0) {
        agent_response(req, "400 Bad Request", "{\"error\":\"A JSON object is required.\"}");
        return NULL;
    }
    if (req->content_len > max_body) {
        httpd_resp_set_hdr(req, "Connection", "close");
        agent_response(req, "413 Payload Too Large", "{\"error\":\"JSON body exceeds this route's bound.\"}");
        return NULL;
    }
    char content_type[80];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/json", 16) != 0 ||
        (content_type[16] != '\0' && content_type[16] != ';')) {
        agent_response(req, "400 Bad Request", "{\"error\":\"Use Content-Type: application/json.\"}");
        return NULL;
    }
    char *body = static_cast<char *>(malloc(req->content_len + 1));
    if (!body) {
        agent_response(req, "503 Service Unavailable", "{\"error\":\"Device memory is unavailable. Try again when idle.\"}");
        return NULL;
    }
    struct BodyWiper {
        char *body; size_t size;
        ~BodyWiper() { mbedtls_platform_zeroize(body, size); free(body); }
    } body_wiper{body, req->content_len + 1};
    size_t received = 0;
    const int64_t deadline = esp_timer_get_time() + 5000000;
    while (received < req->content_len) {
        if (esp_timer_get_time() >= deadline) {
            httpd_resp_set_hdr(req, "Connection", "close");
            agent_response(req, "408 Request Timeout", "{\"error\":\"Request body timed out.\"}");
            return NULL;
        }
        const int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) {
            httpd_resp_set_hdr(req, "Connection", "close");
            agent_response(req, count == HTTPD_SOCK_ERR_TIMEOUT ? "408 Request Timeout" : "400 Bad Request",
                           "{\"error\":\"Request body was not received completely.\"}");
            return NULL;
        }
        received += count;
    }
    body[received] = '\0';
    bool valid = memchr(body, '\0', received) == NULL && agent_valid_utf8(body);
    // cJSON strings are NUL-terminated: reject embedded NUL rather than silently
    // truncating text. Reject unescaped controls, which cJSON otherwise accepts.
    bool in_string = false;
    unsigned depth = 0;
    for (size_t i = 0; valid && i < received; ++i) {
        const unsigned char ch = body[i];
        if (ch == '"') in_string = !in_string;
        else if (in_string && ch < 0x20) valid = false;
        else if (!in_string && ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') valid = false;
        else if (!in_string && (ch == '{' || ch == '[')) {
            // Chat/reset, settings groups and one Wi-Fi array need at most three levels.
            // Bound parser recursion before cJSON runs on the HTTP task stack.
            if (++depth > 3) valid = false;
        } else if (!in_string && (ch == '}' || ch == ']')) {
            if (depth == 0) valid = false;
            else --depth;
        }
        else if (in_string && ch == '\\') {
            if (i + 5 < received && strncmp(body + i + 1, "u0000", 5) == 0) valid = false;
            ++i;
        }
    }
    cJSON *root = valid ? cJSON_ParseWithLengthOpts(body, received + 1, NULL, true) : NULL;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        agent_response(req, "400 Bad Request", "{\"error\":\"Provide a complete, valid JSON object.\"}");
        return NULL;
    }
    return root;
}

static bool owner_cookie(httpd_req_t *req, char token[WEB_AUTH_TOKEN_CAPACITY])
{
    token[0] = '\0';
    const size_t length = httpd_req_get_hdr_value_len(req, "Cookie");
    if (!length || length > 1024) return false;
    char cookies[1025];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookies, sizeof(cookies)) != ESP_OK) return false;
    bool found = false;
    for (const char *part = cookies; *part;) {
        while (*part == ' ' || *part == '\t' || *part == ';') ++part;
        const char *end = strchr(part, ';');
        if (!end) end = part + strlen(part);
        if (strncmp(part, "arvel_owner=", 12) == 0) {
            if (found) return false; // Duplicate cookies cannot select an owner session.
            const char *value = part + 12;
            const char *value_end = end;
            while (value_end > value && (value_end[-1] == ' ' || value_end[-1] == '\t')) --value_end;
            if (value_end - value != WEB_AUTH_TOKEN_CAPACITY - 1) return false;
            for (const char *p = value; p < value_end; ++p)
                if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f')) return false;
            memcpy(token, value, WEB_AUTH_TOKEN_CAPACITY - 1);
            token[WEB_AUTH_TOKEN_CAPACITY - 1] = '\0';
            found = true;
        }
        part = *end ? end + 1 : end;
    }
    return found;
}

static bool owner_origin_allowed(httpd_req_t *req, bool require_origin)
{
    if ((!require_origin || httpd_req_get_hdr_value_len(req, "Origin") != 0) &&
        agent_same_origin(req)) return true;
    agent_response(req, "403 Forbidden", "{\"error\":\"cross_origin_denied\"}");
    return false;
}

static bool owner_authenticated(httpd_req_t *req)
{
    char token[WEB_AUTH_TOKEN_CAPACITY];
    return owner_cookie(req, token) && web_auth_authorized(token);
}

static bool owner_settings_allowed(httpd_req_t *req)
{
    if (!owner_origin_allowed(req, false)) return false;
    if (owner_authenticated(req)) return true;
    agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    return false;
}

static esp_err_t owner_status_response(httpd_req_t *req, bool authenticated)
{
    cJSON *status = cJSON_CreateObject();
    if (!status) return agent_response(req, "503 Service Unavailable", "{\"error\":\"auth_unavailable\"}");
    bool valid = cJSON_AddBoolToObject(status, "configured", web_auth_configured()) &&
        cJSON_AddBoolToObject(status, "authenticated", authenticated) &&
        cJSON_AddBoolToObject(status, "approvalOpen", web_auth_approval_remaining() != 0) &&
        cJSON_AddBoolToObject(status, "storageAvailable", web_auth_storage_available());
    char username[WEB_AUTH_USERNAME_CAPACITY];
    if (authenticated && web_auth_username(username, sizeof(username)))
        valid = valid && cJSON_AddStringToObject(status, "username", username);
    char *body = valid ? cJSON_PrintUnformatted(status) : nullptr;
    cJSON_Delete(status);
    if (!body) return agent_response(req, "503 Service Unavailable", "{\"error\":\"auth_unavailable\"}");
    const esp_err_t sent = agent_response(req, "200 OK", body);
    free(body);
    return sent;
}

static esp_err_t api_owner_status_handler(httpd_req_t *req)
{
    if (!owner_origin_allowed(req, false)) return ESP_OK;
    return owner_status_response(req, owner_authenticated(req));
}

static esp_err_t owner_auth_error(httpd_req_t *req, web_auth_result_t result, bool setup)
{
    switch (result) {
        case WEB_AUTH_INVALID:
            return agent_response(req, "400 Bad Request", "{\"error\":\"invalid_credentials_format\"}");
        case WEB_AUTH_DENIED:
            return agent_response(req, setup ? "403 Forbidden" : "401 Unauthorized",
                setup ? (web_auth_approval_remaining() ? "{\"error\":\"approval_code_invalid\"}" :
                    "{\"error\":\"physical_approval_required\"}") : "{\"error\":\"invalid_credentials\"}");
        case WEB_AUTH_RATE_LIMITED:
            httpd_resp_set_hdr(req, "Retry-After", "60");
            return agent_response(req, "429 Too Many Requests", "{\"error\":\"too_many_attempts\"}");
        case WEB_AUTH_STORAGE_ERROR:
            return agent_response(req, "500 Internal Server Error", "{\"error\":\"auth_storage_error\"}");
        default:
            return agent_response(req, "503 Service Unavailable", "{\"error\":\"auth_unavailable\"}");
    }
}

static bool owner_credentials_json(const cJSON *root, bool setup)
{
    unsigned fields = 0;
    for (const cJSON *item = root->child; item; item = item->next) {
        unsigned field;
        if (strcmp(item->string, "username") == 0 && cJSON_IsString(item)) field = 1;
        else if (strcmp(item->string, "password") == 0 && cJSON_IsString(item)) field = 2;
        else if (strcmp(item->string, "remember") == 0 && cJSON_IsBool(item)) field = 4;
        else if (setup && strcmp(item->string, "code") == 0 && cJSON_IsString(item)) field = 8;
        else return false;
        if (fields & field) return false;
        fields |= field;
    }
    return fields == (setup ? 15U : 7U);
}

struct OwnerArchiveAuthority {
    char token[WEB_AUTH_TOKEN_CAPACITY]{};
    ~OwnerArchiveAuthority() { mbedtls_platform_zeroize(token, sizeof(token)); }
    static bool cancelled(void *context) {
        return !web_auth_authorized(static_cast<OwnerArchiveAuthority *>(context)->token);
    }
};
static esp_err_t owner_credentials_handler(httpd_req_t *req, bool setup)
{
    if (!owner_origin_allowed(req, true)) return ESP_OK;
    if (setup && !web_auth_approval_remaining())
        return agent_response(req, "403 Forbidden", "{\"error\":\"physical_approval_required\"}");
    if (req->content_len > 1024) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return agent_response(req, "413 Payload Too Large", "{\"error\":\"invalid_credentials_format\"}");
    }
    cJSON *root = agent_read_json(req);
    if (!root) return ESP_FAIL;
    if (!owner_credentials_json(root, setup)) {
        cJSON_Delete(root);
        return owner_auth_error(req, WEB_AUTH_INVALID, setup);
    }
    const char *username = cJSON_GetObjectItemCaseSensitive(root, "username")->valuestring;
    char *password = cJSON_GetObjectItemCaseSensitive(root, "password")->valuestring;
    const bool remember = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "remember"));
    char token[WEB_AUTH_TOKEN_CAPACITY] = {};
    const web_auth_result_t result = setup ?
        web_auth_setup(username, password, cJSON_GetObjectItemCaseSensitive(root, "code")->valuestring,
                       remember, token, sizeof(token)) :
        web_auth_login(username, password, remember, token, sizeof(token));
    memset(password, 0, strlen(password));
    cJSON_Delete(root);
    if (result != WEB_AUTH_OK) return owner_auth_error(req, result, setup);
    if (setup) {
        codex_auth_cancel(); // Owner recovery invalidates pending account replacement.
        ai_chat_voice_stop(AI_CHAT_NATIVE); // Revoke queued/native activity before a new owner session.
    }
    char cookie[160];
    snprintf(cookie, sizeof(cookie), "arvel_owner=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%u",
             token, remember ? 31536000U : 28800U);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    // The request still carries its old cookie; status must describe the new session.
    return owner_status_response(req, web_auth_authorized(token));
}

static esp_err_t api_owner_setup_handler(httpd_req_t *req)
{
    return owner_credentials_handler(req, true);
}

static esp_err_t api_owner_login_handler(httpd_req_t *req)
{
    return owner_credentials_handler(req, false);
}

static esp_err_t api_owner_logout_handler(httpd_req_t *req)
{
    if (!owner_origin_allowed(req, true)) return ESP_OK;
    cJSON *root = agent_read_json(req);
    if (!root) return ESP_FAIL;
    const bool valid = root->child == nullptr;
    cJSON_Delete(root);
    if (!valid) return owner_auth_error(req, WEB_AUTH_INVALID, false);
    OwnerArchiveAuthority authority;
    const bool had_cookie = owner_cookie(req, authority.token);
    const bool was_authorized = had_cookie && web_auth_authorized(authority.token);
    const web_auth_result_t result = had_cookie ? web_auth_logout(authority.token) : WEB_AUTH_OK;
    if (result != WEB_AUTH_OK) return owner_auth_error(req, result, false);
    if (was_authorized) ai_chat_voice_stop(AI_CHAT_NATIVE);
    httpd_resp_set_hdr(req, "Set-Cookie", "arvel_owner=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return owner_status_response(req, false);
}

static esp_err_t codex_status_response(httpd_req_t *req, const char *http_status = "200 OK")
{
    const CodexAuthStatus current = codex_auth_status();
    cJSON *status = cJSON_CreateObject();
    if (!status) return agent_response(req, "503 Service Unavailable", "{\"error\":\"codex_unavailable\"}");
    const bool valid = cJSON_AddBoolToObject(status, "configured", current.configured) &&
        cJSON_AddBoolToObject(status, "busy", current.busy) &&
        cJSON_AddStringToObject(status, "state", current.state) &&
        cJSON_AddStringToObject(status, "userCode", current.user_code) &&
        cJSON_AddNumberToObject(status, "expiresIn", current.expires_in) &&
        cJSON_AddStringToObject(status, "error", current.error);
    char *body = valid ? cJSON_PrintUnformatted(status) : nullptr;
    cJSON_Delete(status);
    if (!body) return agent_response(req, "503 Service Unavailable", "{\"error\":\"codex_unavailable\"}");
    if (!owner_authenticated(req)) {
        free(body);
        return agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    }
    const esp_err_t result = agent_response(req, http_status, body);
    free(body);
    return result;
}

static esp_err_t api_codex_status_handler(httpd_req_t *req)
{
    if (!owner_settings_allowed(req)) return ESP_OK;
    return codex_status_response(req);
}

enum class CodexAccountAction { CONNECT, CANCEL, DISCONNECT };

static esp_err_t codex_account_handler(httpd_req_t *req, CodexAccountAction action)
{
    if (!owner_settings_allowed(req) || !owner_origin_allowed(req, true)) return ESP_OK;
    if (req->content_len > 64) {
        httpd_resp_set_hdr(req, "Connection", "close");
        return agent_response(req, "413 Payload Too Large", "{\"error\":\"empty_object_required\"}");
    }
    cJSON *body = agent_read_json(req);
    if (!body) return ESP_FAIL;
    const bool empty = body->child == nullptr;
    cJSON_Delete(body);
    if (!owner_authenticated(req))
        return agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    if (!empty)
        return agent_response(req, "400 Bad Request", "{\"error\":\"empty_object_required\"}");
    std::string error;
    if (action == CodexAccountAction::CONNECT) {
        if (!codex_auth_begin(error)) {
            const bool busy = codex_auth_status().busy;
            return agent_response(req, busy ? "409 Conflict" : "503 Service Unavailable",
                busy ? "{\"error\":\"codex_login_pending\"}" : "{\"error\":\"codex_unavailable\"}");
        }
        return codex_status_response(req, "202 Accepted");
    }
    if (action == CodexAccountAction::CANCEL) {
        codex_auth_cancel();
        return codex_status_response(req);
    }
    if (!codex_auth_disconnect(error))
        return agent_response(req, "500 Internal Server Error", "{\"error\":\"codex_storage_error\"}");
    // Removing account credentials is a configuration change, never a mic-start permission.
    speech_live_stop();
    speech_live_cancel_speech();
    return codex_status_response(req);
}

static esp_err_t api_codex_connect_handler(httpd_req_t *req)
{
    return codex_account_handler(req, CodexAccountAction::CONNECT);
}

static esp_err_t api_codex_cancel_handler(httpd_req_t *req)
{
    return codex_account_handler(req, CodexAccountAction::CANCEL);
}

static esp_err_t api_codex_disconnect_handler(httpd_req_t *req)
{
    return codex_account_handler(req, CodexAccountAction::DISCONNECT);
}

static esp_err_t agent_submit_response(httpd_req_t *req, ai_chat_submit_result_t result)
{
    switch (result) {
        case AI_CHAT_FORBIDDEN:
            return agent_response(req, "403 Forbidden", "{\"error\":\"physical_local_access_required\"}");
        case AI_CHAT_ACCEPTED:
            return agent_response(req, "202 Accepted", "{\"accepted\":true}");
        case AI_CHAT_BUSY:
            return agent_response(req, "409 Conflict", "{\"error\":\"Buddy is busy. Wait for the current operation to finish.\"}");
        case AI_CHAT_OFFLINE:
            return agent_response(req, "503 Service Unavailable", "{\"error\":\"Device Wi-Fi is offline. Open Wi-Fi setup and connect.\"}");
        case AI_CHAT_NOT_CONFIGURED:
            return agent_response(req, "412 Precondition Failed", "{\"error\":\"Log in to Settings and add the chat API key; never paste a key in chat.\"}");
        case AI_CHAT_INVALID_INPUT:
            return agent_response(req, "400 Bad Request", "{\"error\":\"Enter a non-empty message of at most 8192 UTF-8 bytes.\"}");
        case AI_CHAT_STORAGE_ERROR:
            return agent_response(req, "500 Internal Server Error", "{\"error\":\"Settings could not be saved. Existing settings are unchanged.\"}");
        case AI_CHAT_NO_MEMORY:
        default:
            return agent_response(req, "503 Service Unavailable", "{\"error\":\"Agent is unavailable. Wait until idle, then try again.\"}");
    }
}

/* These fixed routes are the only HTTP-to-native authority bridge. Origin and
 * a current owner session are firmware-verified; neither source, tool names nor
 * native authority can be supplied in client JSON. Legacy handlers stay AP-only. */
static bool owner_device_allowed(httpd_req_t *req, bool write)
{
    if (!owner_origin_allowed(req, write)) return false;
    if (owner_authenticated(req)) return true;
    agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    return false;
}

static cJSON *owner_device_body(httpd_req_t *req)
{
    if (!owner_device_allowed(req, true)) return nullptr;
    cJSON *body = agent_read_json(req);
    if (!body) return nullptr;
    // A body may take seconds to arrive: authority must still be current.
    if (!owner_authenticated(req)) {
        cJSON_Delete(body);
        agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
        return nullptr;
    }
    return body;
}

static esp_err_t api_device_get_handler(httpd_req_t *req)
{
    if (!owner_device_allowed(req, false)) return ESP_OK;
    char *snapshot = ai_chat_snapshot_json(AI_CHAT_NATIVE);
    if (!snapshot) return agent_submit_response(req, AI_CHAT_NO_MEMORY);
    if (!owner_authenticated(req)) {
        free(snapshot);
        return agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    }
    const esp_err_t result = agent_response(req, "200 OK", snapshot);
    free(snapshot);
    return result;
}

static esp_err_t api_device_control_handler(httpd_req_t *req)
{
    cJSON *body = owner_device_body(req);
    if (!body) return ESP_FAIL;
    const cJSON *action = body->child;
    if (!action || action->next || strcmp(action->string, "action") || !cJSON_IsString(action)) {
        cJSON_Delete(body);
        return agent_response(req, "400 Bad Request", "{\"error\":\"Expected exactly {action: string}.\"}");
    }
    ai_chat_submit_result_t result;
    if (!strcmp(action->valuestring, "voice_toggle")) {
        OwnerArchiveAuthority authority;
        result = owner_cookie(req, authority.token) && web_auth_authorized(authority.token)
            ? ai_chat_voice_toggle(AI_CHAT_NATIVE, authority.token) : AI_CHAT_FORBIDDEN;
        if (result == AI_CHAT_NOT_CONFIGURED) {
            cJSON_Delete(body);
            return agent_response(req, "412 Precondition Failed",
                "{\"error\":\"Voice is not configured. Open Settings to select and configure the conversation mode.\"}");
        }
    }
    else if (!strcmp(action->valuestring, "voice_stop")) result = ai_chat_voice_stop(AI_CHAT_NATIVE);
    else if (!strcmp(action->valuestring, "screen_toggle")) {
        ai_chat_toggle_screen();
        result = AI_CHAT_ACCEPTED;
    } else if (!strcmp(action->valuestring, "brightness_cycle")) {
        result = ai_chat_cycle_brightness() ? AI_CHAT_ACCEPTED : AI_CHAT_NO_MEMORY;
    } else {
        cJSON_Delete(body);
        return agent_response(req, "400 Bad Request", "{\"error\":\"unknown_device_action\"}");
    }
    cJSON_Delete(body);
    return agent_submit_response(req, result);
}

static esp_err_t api_device_chat_handler(httpd_req_t *req)
{
    cJSON *body = owner_device_body(req);
    if (!body) return ESP_FAIL;
    const cJSON *text = nullptr, *speak = nullptr;
    bool valid = true;
    for (const cJSON *field = body->child; field; field = field->next) {
        if (!strcmp(field->string, "text") && !text) text = field;
        else if (!strcmp(field->string, "speak") && !speak) speak = field;
        else valid = false;
    }
    if (!valid || !cJSON_IsString(text) || (speak && !cJSON_IsBool(speak))) {
        cJSON_Delete(body);
        return agent_response(req, "400 Bad Request", "{\"error\":\"Expected {text: string, speak?: boolean}, without duplicate or unknown fields.\"}");
    }
    if (strlen(text->valuestring) > 8192) {
        cJSON_Delete(body);
        return agent_response(req, "413 Payload Too Large", "{\"error\":\"Message exceeds 8192 UTF-8 bytes.\"}");
    }
    const ai_chat_speech_t speech = !speak ? AI_CHAT_USE_DEFAULT_SPEECH :
        cJSON_IsTrue(speak) ? AI_CHAT_READ_ALOUD : AI_CHAT_SILENT;
    OwnerArchiveAuthority authority;
    const ai_chat_submit_result_t result = owner_cookie(req, authority.token) && web_auth_authorized(authority.token)
        ? ai_chat_submit_text(AI_CHAT_NATIVE, text->valuestring, speech, authority.token) : AI_CHAT_FORBIDDEN;
    cJSON_Delete(body);
    return agent_submit_response(req, result);
}

static esp_err_t api_device_reset_handler(httpd_req_t *req)
{
    cJSON *body = owner_device_body(req);
    if (!body) return ESP_FAIL;
    const bool empty = body->child == nullptr;
    cJSON_Delete(body);
    if (!empty) return agent_response(req, "400 Bad Request", "{\"error\":\"empty_object_required\"}");
    return agent_submit_response(req, ai_chat_reset(AI_CHAT_NATIVE));
}

static esp_err_t api_agent_get_handler(httpd_req_t *req)
{
    if (!legacy_window_allowed(req)) return ESP_OK;
    char *snapshot = ai_chat_snapshot_json(AI_CHAT_LEGACY_HTTP);
    if (!snapshot) return agent_submit_response(req, AI_CHAT_NO_MEMORY);
    esp_err_t result = agent_response(req, "200 OK", snapshot);
    free(snapshot);
    return result;
}

static esp_err_t api_agent_settings_get_handler(httpd_req_t *req)
{
    if (!owner_settings_allowed(req)) return ESP_OK;
    char *settings = ai_chat_settings_json();
    if (!settings) return agent_submit_response(req, AI_CHAT_NO_MEMORY);
    if (!owner_authenticated(req)) {
        free(settings);
        return agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    }
    const esp_err_t result = agent_response(req, "200 OK", settings);
    free(settings);
    return result;
}

static esp_err_t api_agent_settings_post_handler(httpd_req_t *req)
{
    if (!owner_settings_allowed(req)) return ESP_OK;
    if (!owner_origin_allowed(req, true)) return ESP_OK;
    cJSON *root = agent_read_json(req, 48 * 1024);
    if (!root) return ESP_FAIL;
    // Recheck the owner session after receiving a bounded settings body.
    if (!owner_authenticated(req)) {
        wipe_private_json(root);
        cJSON_Delete(root);
        return agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    }
    char error[192] = {};
    const ai_chat_submit_result_t result = ai_chat_update_settings(root, error, sizeof(error));
    wipe_private_json(root);
    cJSON_Delete(root);
    if (result == AI_CHAT_ACCEPTED) return api_agent_settings_get_handler(req);
    if (result != AI_CHAT_INVALID_INPUT && result != AI_CHAT_STORAGE_ERROR)
        return agent_submit_response(req, result);
    cJSON *response = cJSON_CreateObject();
    if (!response) return agent_submit_response(req, AI_CHAT_NO_MEMORY);
    char *body = cJSON_AddStringToObject(response, "error", error) ? cJSON_PrintUnformatted(response) : nullptr;
    cJSON_Delete(response);
    if (!body) return agent_submit_response(req, AI_CHAT_NO_MEMORY);
    const char *status = result == AI_CHAT_INVALID_INPUT ? "400 Bad Request" :
        !strncmp(error, "NVS settings capacity is too small", 34) ? "507 Insufficient Storage" : "500 Internal Server Error";
    const esp_err_t sent = agent_response(req, status, body);
    free(body);
    return sent;
}

// Query values are data, never SQL, paths, MQTT topics or model instructions.
struct OwnerQuery {
    int64_t after = 0, utc_from = 0, utc_to = 0;
    unsigned count = conversation_store::kPageLimit;
    char literal[257]{};
    conversation_store::ExportKind kind = conversation_store::ExportKind::messages;
};
static bool owner_query(httpd_req_t *req, OwnerQuery &out)
{
    const size_t length = httpd_req_get_url_query_len(req);
    if (!length) return true;
    char query[2049];
    if (length >= sizeof(query) || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    const char *keys[] = {"after", "count", "literal", "utcFrom", "utcTo", "kind"};
    unsigned seen = 0;
    for (char *part = query; *part;) {
        char *end = strchr(part, '&');
        if (end) *end = 0;
        char *equal = strchr(part, '=');
        if (!equal) return false;
        *equal++ = 0;
        unsigned key = 0;
        while (key < 6 && strcmp(part, keys[key])) ++key;
        if (key == 6 || (seen & (1U << key))) return false;
        seen |= 1U << key;
        char value[257]; size_t used = 0;
        for (const char *p = equal; *p; ++p) {
            unsigned char ch = static_cast<unsigned char>(*p);
            if (ch == '%') {
                if (!p[1] || !p[2]) return false;
                unsigned decoded = 0;
                for (unsigned n = 1; n <= 2; ++n) {
                    const char digit = p[n];
                    unsigned nibble;
                    if (digit >= '0' && digit <= '9') nibble = digit - '0';
                    else if (digit >= 'a' && digit <= 'f') nibble = digit - 'a' + 10;
                    else if (digit >= 'A' && digit <= 'F') nibble = digit - 'A' + 10;
                    else return false;
                    decoded = (decoded << 4) | nibble;
                }
                ch = static_cast<unsigned char>(decoded); p += 2;
            } else if (ch == '+') ch = ' ';
            if (!ch || used == sizeof(value) - 1) return false;
            value[used++] = static_cast<char>(ch);
        }
        value[used] = 0;
        if (!agent_valid_utf8(value)) return false;
        if (key == 2) memcpy(out.literal, value, used + 1);
        else if (key == 5) {
            const char *kinds[] = {"messages", "session_boundaries", "tool_calls", "sources", "tool_evidence"};
            unsigned selected = 0;
            while (selected < 5 && strcmp(value, kinds[selected])) ++selected;
            if (selected == 5) return false;
            out.kind = static_cast<conversation_store::ExportKind>(selected);
        } else {
            if (!used) return false;
            uint64_t number = 0;
            constexpr uint64_t maximum = 9007199254740991ULL; // Exact browser integer.
            for (size_t n = 0; n < used; ++n) {
                if (value[n] < '0' || value[n] > '9' || number > (maximum - (value[n] - '0')) / 10) return false;
                number = number * 10 + value[n] - '0';
            }
            if (key == 0) out.after = number;
            else if (key == 1) {
                if (!number || number > conversation_store::kPageLimit) return false;
                out.count = static_cast<unsigned>(number);
            } else if (key == 3) out.utc_from = number;
            else out.utc_to = number;
        }
        if (!end) break;
        part = end + 1;
        if (!*part) return false;
    }
    return !out.utc_to || out.utc_to >= out.utc_from;
}

static esp_err_t owner_archive_response(httpd_req_t *req, const conversation_store::Result &result, bool stream)
{
    if (!owner_authenticated(req)) return agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    using Error = conversation_store::Error;
    if (!result.ok()) {
        switch (result.error) {
        case Error::invalid: return agent_response(req, "400 Bad Request", "{\"error\":\"invalid_history_query\"}");
        case Error::too_large: return agent_response(req, "413 Payload Too Large", "{\"error\":\"history_group_exceeds_page_bound\"}");
        case Error::not_found: return agent_response(req, "404 Not Found", "{\"error\":\"conversation_not_found\"}");
        case Error::busy: return agent_response(req, "409 Conflict", "{\"error\":\"history_storage_busy\"}");
        case Error::conflict: return agent_response(req, "409 Conflict", "{\"error\":\"history_conflict\"}");
        case Error::cancelled: return agent_response(req, "409 Conflict", "{\"error\":\"history_request_cancelled\"}");
        case Error::full: return agent_response(req, "507 Insufficient Storage", "{\"error\":\"history_storage_full\"}");
        default: return agent_response(req, "503 Service Unavailable", "{\"error\":\"history_storage_unavailable\"}");
        }
    }
    if (!stream) return agent_response(req, "200 OK", result.json.c_str());
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (size_t offset = 0; offset < result.json.size();) {
        const size_t count = std::min<size_t>(2048, result.json.size() - offset);
        const esp_err_t sent = httpd_resp_send_chunk(req, result.json.data() + offset, count);
        if (sent != ESP_OK) return sent;
        offset += count;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t api_device_history_handler(httpd_req_t *req)
{
    if (!owner_device_allowed(req, req->method != HTTP_GET)) return ESP_OK;
    constexpr char prefix[] = "/rest/device/history";
    const size_t path_length = strcspn(req->uri, "?");
    if (path_length < sizeof(prefix) - 1 || strncmp(req->uri, prefix, sizeof(prefix) - 1))
        return agent_response(req, "404 Not Found", "{\"error\":\"unknown_history_route\"}");
    const char *tail = req->uri + sizeof(prefix) - 1;
    size_t remaining = path_length - (sizeof(prefix) - 1);
    char uuid[37]{};
    const char *action = "";
    bool listing = remaining == 0, exporting = false;
    if (!listing) {
        if (*tail++ != '/') return agent_response(req, "404 Not Found", "{\"error\":\"unknown_history_route\"}");
        --remaining;
        if (remaining == 3 && !memcmp(tail, "new", 3)) action = "new";
        else {
            if (remaining < 36) return agent_response(req, "400 Bad Request", "{\"error\":\"invalid_conversation_id\"}");
            memcpy(uuid, tail, 36); tail += 36; remaining -= 36;
            if (!conversation_store::valid_uuid(uuid)) return agent_response(req, "400 Bad Request", "{\"error\":\"invalid_conversation_id\"}");
            if (remaining == 7 && !memcmp(tail, "/export", 7)) exporting = true;
            else if (remaining == 7 && !memcmp(tail, "/resume", 7)) action = "resume";
            else if (remaining == 6 && !memcmp(tail, "/close", 6)) action = "close";
            else if (remaining) return agent_response(req, "404 Not Found", "{\"error\":\"unknown_history_route\"}");
        }
    }
    OwnerQuery query;
    if (!owner_query(req, query)) return agent_response(req, "400 Bad Request", "{\"error\":\"invalid_history_query\"}");
    OwnerArchiveAuthority authority;
    if (!owner_cookie(req, authority.token)) return agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    if (req->method == HTTP_GET) {
        if (*action) return agent_response(req, "405 Method Not Allowed", "{\"error\":\"history_action_requires_post\"}");
        conversation_store::Control control{static_cast<uint64_t>(esp_timer_get_time()) + 8000000,
                                           OwnerArchiveAuthority::cancelled, &authority};
        if (listing) return owner_archive_response(req, conversation_store::list_conversations(query.after, query.count, control), false);
        conversation_store::Query page;
        page.conversation = uuid; page.after_id = query.after; page.count = query.count;
        page.literal = query.literal[0] ? query.literal : nullptr;
        page.utc_from_ms = query.utc_from; page.utc_to_ms = query.utc_to;
        page.export_kind = query.kind;
        return owner_archive_response(req, exporting ? conversation_store::export_page(page, control) :
                                       conversation_store::history(page, control), exporting);
    }
    if (exporting || listing || (req->method != HTTP_POST && req->method != HTTP_DELETE) ||
        (req->method == HTTP_POST && !*action) || (req->method == HTTP_DELETE && *action))
        return agent_response(req, "405 Method Not Allowed", "{\"error\":\"invalid_history_method\"}");
    if (req->content_len || req->method == HTTP_POST) {
        cJSON *body = owner_device_body(req);
        if (!body) return ESP_FAIL;
        const bool empty = body->child == nullptr;
        cJSON_Delete(body);
        if (!empty) return agent_response(req, "400 Bad Request", "{\"error\":\"empty_object_required\"}");
    }
    if (!web_auth_authorized(authority.token)) return agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    return agent_submit_response(req, ai_chat_owner_memory_action(req->method == HTTP_DELETE ? "delete" : action,
                                                                 uuid, authority.token));
}

static esp_err_t api_agent_readiness_handler(httpd_req_t *req)
{
    cJSON *body = owner_device_body(req);
    if (!body) return ESP_FAIL;
    const bool empty = body->child == nullptr;
    cJSON_Delete(body);
    if (!empty) return agent_response(req, "400 Bad Request", "{\"error\":\"empty_object_required\"}");
    return agent_submit_response(req, ai_chat_check_native_readiness(AI_CHAT_NATIVE));
}

static bool add_desk_note(cJSON *notes, const desk_note_t &note)
{
    cJSON *item = cJSON_CreateObject();
    if (!item || !cJSON_AddItemToArray(notes, item)) { cJSON_Delete(item); return false; }
    return cJSON_AddNumberToObject(item, "id", note.id) &&
        cJSON_AddStringToObject(item, "kind", note.task ? "task" : "note") &&
        cJSON_AddStringToObject(item, "text", note.text) &&
        cJSON_AddStringToObject(item, "dueDate", note.due_date) &&
        cJSON_AddBoolToObject(item, "completed", note.completed);
}

static bool add_desk_timer(cJSON *root)
{
    desk_timer_snapshot_t timer{};
    if (!desk_buddy_timer_snapshot(&timer) || timer.status == DESK_TIMER_IDLE) return cJSON_AddNullToObject(root, "timer");
    static const char *const statuses[] = {"idle", "running", "paused", "done"};
    static const char *const phases[] = {"timer", "focus", "break"};
    cJSON *item = cJSON_AddObjectToObject(root, "timer");
    return item && cJSON_AddStringToObject(item, "status", statuses[timer.status]) &&
        cJSON_AddStringToObject(item, "phase", phases[timer.phase]) &&
        cJSON_AddBoolToObject(item, "pomodoro", timer.pomodoro) &&
        cJSON_AddNumberToObject(item, "round", timer.round) &&
        cJSON_AddNumberToObject(item, "rounds", timer.rounds) &&
        cJSON_AddNumberToObject(item, "remainingSeconds", timer.remaining_seconds) &&
        cJSON_AddNumberToObject(item, "durationSeconds", timer.duration_seconds) &&
        cJSON_AddStringToObject(item, "label", timer.label);
}

static bool add_desk_weather(cJSON *root)
{
    std::unique_ptr<desk_weather_snapshot_t> weather(new (std::nothrow) desk_weather_snapshot_t{});
    if (!weather) return false;
    if (!desk_buddy_weather_snapshot(weather.get())) return cJSON_AddNullToObject(root, "weather");
    cJSON *item = cJSON_AddObjectToObject(root, "weather");
    bool okay = item && cJSON_AddStringToObject(item, "location", weather->location) &&
        cJSON_AddStringToObject(item, "condition", weather->condition) &&
        cJSON_AddNumberToObject(item, "weatherCode", weather->weather_code) &&
        cJSON_AddNumberToObject(item, "temperatureC", weather->temperature_c) &&
        cJSON_AddNumberToObject(item, "feelsLikeC", weather->feels_like_c) &&
        cJSON_AddNumberToObject(item, "highC", weather->high_c) &&
        cJSON_AddNumberToObject(item, "lowC", weather->low_c) &&
        cJSON_AddNumberToObject(item, "windKmh", weather->wind_kmh) &&
        cJSON_AddNumberToObject(item, "fetchedAt", static_cast<double>(weather->fetched_at)) &&
        cJSON_AddNumberToObject(item, "utcOffsetSeconds", weather->utc_offset_seconds);
    cJSON *forecast = okay ? cJSON_AddArrayToObject(item, "forecast") : nullptr;
    okay = forecast != nullptr;
    for (unsigned i = 0; okay && i < weather->forecast_count && i < DESK_FORECAST_HOURS; ++i) {
        const desk_forecast_hour_t &hour = weather->forecast[i];
        cJSON *entry = cJSON_CreateObject();
        if (!entry || !cJSON_AddItemToArray(forecast, entry)) { cJSON_Delete(entry); return false; }
        okay = cJSON_AddNumberToObject(entry, "time", static_cast<double>(hour.time)) &&
            cJSON_AddNumberToObject(entry, "temperatureC", hour.temperature_c) &&
            cJSON_AddNumberToObject(entry, "precipitationProbability", hour.precipitation_probability) &&
            cJSON_AddNumberToObject(entry, "weatherCode", hour.weather_code);
    }
    return okay;
}

/* Read-only view of the desk tools' local state (notes/tasks, timer, cached
 * weather). Changes still go through the agent's tools, never this route. */
static esp_err_t api_device_desk_get_handler(httpd_req_t *req)
{
    if (!owner_device_allowed(req, false)) return ESP_OK;
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_CreateObject(), cJSON_Delete);
    bool okay = root && cJSON_AddBoolToObject(root.get(), "notesReady", desk_buddy_notes_ready());
    cJSON *notes = okay ? cJSON_AddArrayToObject(root.get(), "notes") : nullptr;
    okay = notes != nullptr;
    desk_note_t note{};
    for (size_t i = 0; okay && desk_buddy_note_get(i, &note); ++i) okay = add_desk_note(notes, note);
    okay = okay && add_desk_timer(root.get()) && add_desk_weather(root.get());
    char *body = okay ? cJSON_PrintUnformatted(root.get()) : nullptr;
    if (!body) return agent_submit_response(req, AI_CHAT_NO_MEMORY);
    if (!owner_authenticated(req)) {
        cJSON_free(body);
        return agent_response(req, "401 Unauthorized", "{\"error\":\"login_required\"}");
    }
    const esp_err_t result = agent_response(req, "200 OK", body);
    cJSON_free(body);
    return result;
}

static esp_err_t api_agent_chat_handler(httpd_req_t *req)
{
    if (!legacy_window_allowed(req)) return ESP_OK;
    if (!agent_same_origin(req)) {
        agent_response(req, "403 Forbidden", "{\"error\":\"Cross-origin chat requests are not allowed.\"}");
        return ESP_FAIL;
    }
    cJSON *root = agent_read_json(req);
    if (!root) return ESP_FAIL; // Close the connection if a body was incomplete or rejected.
    cJSON *text = NULL;
    cJSON *speak = NULL;
    bool valid = true;
    for (cJSON *field = root->child; field; field = field->next) {
        if (strcmp(field->string, "text") == 0 && !text) text = field;
        else if (strcmp(field->string, "speak") == 0 && !speak) speak = field;
        else valid = false;
    }
    valid = valid && cJSON_IsString(text) && (!speak || cJSON_IsBool(speak));
    if (!valid) {
        cJSON_Delete(root);
        return agent_response(req, "400 Bad Request", "{\"error\":\"Expected {text: string, speak?: boolean}, without duplicate or unknown fields.\"}");
    }
    if (strlen(text->valuestring) > 8192) {
        cJSON_Delete(root);
        return agent_response(req, "413 Payload Too Large", "{\"error\":\"Message exceeds 8192 UTF-8 bytes.\"}");
    }
    if (cJSON_IsTrue(speak)) {
        cJSON_Delete(root);
        return agent_response(req, "403 Forbidden", "{\"error\":\"native_speech_required\"}");
    }
    if (!espwifi_provisioning_request_allowed(httpd_req_to_sockfd(req))) {
        cJSON_Delete(root);
        return agent_response(req, "403 Forbidden", "{\"error\":\"physical_local_access_required\"}");
    }
    const ai_chat_submit_result_t result = ai_chat_submit_text(AI_CHAT_LEGACY_HTTP, text->valuestring, AI_CHAT_SILENT, nullptr);
    cJSON_Delete(root);
    return agent_submit_response(req, result);
}

static esp_err_t api_agent_reset_handler(httpd_req_t *req)
{
    if (!legacy_window_allowed(req)) return ESP_OK;
    if (!agent_same_origin(req)) {
        agent_response(req, "403 Forbidden", "{\"error\":\"Cross-origin reset requests are not allowed.\"}");
        return ESP_FAIL;
    }
    if (req->content_len > 0) {
        cJSON *root = agent_read_json(req);
        if (!root) return ESP_FAIL;
        const bool empty = root->child == NULL;
        cJSON_Delete(root);
        if (!empty) return agent_response(req, "400 Bad Request", "{\"error\":\"New chat expects an empty JSON object or no body.\"}");
    }
    if (!legacy_window_allowed(req)) return ESP_OK;
    return agent_submit_response(req, ai_chat_reset(AI_CHAT_LEGACY_HTTP));
}

static esp_err_t api_agent_voice_handler(httpd_req_t *req)
{
    return agent_response(req, "403 Forbidden", "{\"error\":\"native_voice_required\"}");
}

static esp_err_t api_agent_options_handler(httpd_req_t *req)
{
    // No wildcard CORS/preflight policy is shared with agent endpoints.
    httpd_resp_set_hdr(req, "Allow", "GET, POST");
    return agent_response(req, "405 Method Not Allowed", "{\"error\":\"Agent requests must be same-origin.\"}");
}

/**
 * @brief Handler for serving embedded web files
 */
static esp_err_t www_handler(httpd_req_t *req)
{
    char path[128];
    const size_t path_length = strcspn(req->uri, "?");
    if (path_length >= sizeof(path)) return httpd_resp_send_404(req);
    memcpy(path, req->uri, path_length);
    path[path_length] = '\0';
    const char *uri = path;

    // Serve the SPA entry for primary routes, including direct Wi-Fi bookmarks.
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/wifi") == 0 ||
        strcmp(uri, "/wifi/sta") == 0 || strcmp(uri, "/wifi/sta/") == 0 ||
        strcmp(uri, "/settings") == 0 || strcmp(uri, "/settings/") == 0 ||
        strcmp(uri, "/login") == 0 || strcmp(uri, "/login/") == 0) {
        uri = "/index.html";
    }

    ESP_LOGD(TAG, "Serving: %s", uri);

    // Find and serve the file
    for (int i = 0; i < WWW_DATA_COUNT; i++) {
        if (strcmp(uri, www_data[i].uri) == 0) {
            // Set content type
            httpd_resp_set_type(req, www_data[i].content_type);

            // Set gzip encoding header
            httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

            // Set cache control for static assets
            if (strstr(uri, "/_app/immutable/") != NULL) {
                // The existing bundler strips some filename hashes; revalidate after flashing.
                httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
            } else if (strcmp(uri, "/index.html") == 0) {
                httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            } else {
                // Other files get shorter cache
                httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
            }

            // Send the file
            esp_err_t ret = httpd_resp_send(req, (const char*)www_data[i].content, www_data[i].len);

            if (ret == ESP_OK) {
                ESP_LOGD(TAG, "Served %s (%zu bytes)", uri, www_data[i].len);
            } else {
                ESP_LOGE(TAG, "Failed to serve %s: %s", uri, esp_err_to_name(ret));
            }

            return ret;
        }
    }

    // File not found
    ESP_LOGW(TAG, "File not found: %s", uri);
    httpd_resp_send_404(req);
    return ESP_FAIL;
}

/**
 * @brief Handler for /rest/system/status API endpoint
 */
static esp_err_t api_system_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Get system information
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // Add system information to JSON
    cJSON_AddStringToObject(root, "app_name", APP_NAME);
    cJSON_AddStringToObject(root, "app_version", APP_VERSION);
    cJSON_AddNumberToObject(root, "cpu_cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "min_free_heap", esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000); // Convert to seconds

    // Get chip model string
    const char *chip_model;
    switch(chip_info.model) {
        case CHIP_ESP32:   chip_model = "ESP32"; break;
        case CHIP_ESP32S2: chip_model = "ESP32-S2"; break;
        case CHIP_ESP32S3: chip_model = "ESP32-S3"; break;
        case CHIP_ESP32C3: chip_model = "ESP32-C3"; break;
        case CHIP_ESP32C2: chip_model = "ESP32-C2"; break;
        case CHIP_ESP32C6: chip_model = "ESP32-C6"; break;
        case CHIP_ESP32H2: chip_model = "ESP32-H2"; break;
        default:           chip_model = "Unknown"; break;
    }
    cJSON_AddStringToObject(root, "chip_model", chip_model);

    // Convert to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);

    free(json_str);
    return ret;
}

/**
 * @brief Handler for /rest/wifi/status API endpoint
 */
static esp_err_t api_wifi_status_handler(httpd_req_t *req)
{
    if (!legacy_window_allowed(req)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Get WiFi status
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    cJSON_AddNumberToObject(root, "mode", mode);

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddStringToObject(root, "ssid", (char*)ap_info.ssid);
            cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);
            cJSON_AddBoolToObject(root, "connected", true);

            // Get IP address
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    cJSON_AddStringToObject(root, "ip", ip_str);
                }
            }
        } else {
            cJSON_AddBoolToObject(root, "connected", false);
        }
    }

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        wifi_config_t wifi_config;
        if (esp_wifi_get_config(WIFI_IF_AP, &wifi_config) == ESP_OK) {
            cJSON_AddStringToObject(root, "ap_ssid", (char*)wifi_config.ap.ssid);
            cJSON_AddNumberToObject(root, "ap_channel", wifi_config.ap.channel);

            // Get AP IP address
            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    cJSON_AddStringToObject(root, "ap_ip", ip_str);
                }
            }
        }
    }

    // Convert to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);

    free(json_str);
    return ret;
}

/**
 * @brief Handler for /rest/features API endpoint
 */
static esp_err_t api_features_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Add firmware information
    cJSON_AddStringToObject(root, "firmware_name", APP_NAME);
    cJSON_AddStringToObject(root, "firmware_version", APP_VERSION);
    cJSON_AddStringToObject(root, "firmware_built_target", "esp32s3");

    // Add feature flags (all disabled for now, can be enabled as needed)
    cJSON_AddBoolToObject(root, "security", false);
    cJSON_AddBoolToObject(root, "mqtt", false);
    cJSON_AddBoolToObject(root, "ntp", false);
    cJSON_AddBoolToObject(root, "upload_firmware", false);
    cJSON_AddBoolToObject(root, "download_firmware", false);
    cJSON_AddBoolToObject(root, "sleep", false);
    cJSON_AddBoolToObject(root, "battery", false);
    cJSON_AddBoolToObject(root, "analytics", false);
    cJSON_AddBoolToObject(root, "coredump", false);
    cJSON_AddBoolToObject(root, "event_use_json", false);

    // Convert to string
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Send response
    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);

    free(json_str);
    return ret;
}

/**
 * @brief Handler for /rest/systemStatus API endpoint (simplified version)
 */
static esp_err_t api_system_status_simple_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "System status requested");

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Get chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    // Add all system information
    cJSON_AddStringToObject(root, "esp_platform", "ESP32-S3");
    cJSON_AddStringToObject(root, "firmware_version", APP_VERSION);

    // Safely get heap information
    size_t free_heap = esp_get_free_heap_size();
    size_t min_free_heap = esp_get_minimum_free_heap_size();
    size_t max_alloc = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    cJSON_AddNumberToObject(root, "free_heap", free_heap);
    cJSON_AddNumberToObject(root, "min_free_heap", min_free_heap);
    cJSON_AddNumberToObject(root, "max_alloc_heap", max_alloc);
    cJSON_AddNumberToObject(root, "cpu_freq_mhz", 240);
    cJSON_AddStringToObject(root, "cpu_type", "ESP32-S3");
    cJSON_AddNumberToObject(root, "cpu_rev", chip_info.revision);
    cJSON_AddNumberToObject(root, "cpu_cores", chip_info.cores);
    cJSON_AddNumberToObject(root, "uptime", (uint32_t)(esp_timer_get_time() / 1000000));

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        ESP_LOGE(TAG, "Failed to serialize JSON");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending system status: %zu bytes", strlen(json_str));

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);

    ESP_LOGI(TAG, "System status sent successfully");
    return ret;
}

/**
 * @brief Handler for /rest/wifiStatus API endpoint (simplified version)
 */
static esp_err_t api_wifi_status_simple_handler(httpd_req_t *req)
{
    if (!legacy_window_allowed(req)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            cJSON_AddNumberToObject(root, "status", 3); // WL_CONNECTED
            cJSON_AddStringToObject(root, "ssid", (char*)ap_info.ssid);
            cJSON_AddNumberToObject(root, "rssi", ap_info.rssi);

            esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (netif) {
                esp_netif_ip_info_t ip_info;
                if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                    char ip_str[16];
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                    cJSON_AddStringToObject(root, "local_ip", ip_str);
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.gw));
                    cJSON_AddStringToObject(root, "gateway_ip", ip_str);
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.netmask));
                    cJSON_AddStringToObject(root, "subnet_mask", ip_str);
                }
            }
        } else {
            cJSON_AddNumberToObject(root, "status", 6); // WL_DISCONNECTED
        }
    } else {
        cJSON_AddNumberToObject(root, "status", 6); // WL_DISCONNECTED
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}

/**
 * @brief Handler for /rest/apStatus API endpoint
 */
static esp_err_t api_ap_status_handler(httpd_req_t *req)
{
    if (!legacy_window_allowed(req)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);

    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) {
        cJSON_AddNumberToObject(root, "status", 1); // Active

        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                char ip_str[16];
                snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                cJSON_AddStringToObject(root, "ip_address", ip_str);
            }
        }

        wifi_sta_list_t sta_list;
        esp_wifi_ap_get_sta_list(&sta_list);
        cJSON_AddNumberToObject(root, "station_num", sta_list.num);
    } else {
        cJSON_AddNumberToObject(root, "status", 0); // Inactive
        cJSON_AddNumberToObject(root, "station_num", 0);
    }

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, json_str);
    free(json_str);
    return ret;
}

/**
 * @brief Generic stub handler for unimplemented endpoints
 */
static esp_err_t api_stub_handler(httpd_req_t *req)
{
    return agent_response(req, "501 Not Implemented", "{\"error\":\"not_implemented\"}");
}

/**
 * @brief Handler for GET /rest/wifiSettings
 */
static esp_err_t api_wifi_settings_get_handler(httpd_req_t *req)
{
    if (!legacy_window_allowed(req)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Get current WiFi configuration
    wifi_config_t wifi_config = {};
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) != ESP_OK) {
        cJSON_Delete(root);
        return agent_response(req, "503 Service Unavailable", "{\"error\":\"Wi-Fi configuration is unavailable.\"}");
    }
    char ssid_string[33] = {};
    memcpy(ssid_string, wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid));

    // Add basic WiFi settings
    cJSON_AddStringToObject(root, "hostname", "unravel-agent");
    cJSON_AddNumberToObject(root, "connection_mode", 1);

    // Create wifi_networks array
    cJSON *wifi_networks = cJSON_CreateArray();
    cJSON *network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "ssid", ssid_string);
    cJSON_AddStringToObject(network, "password", ""); // Don't send password for security
    cJSON_AddBoolToObject(network, "static_ip_config", false);
    cJSON_AddItemToArray(wifi_networks, network);
    cJSON_AddItemToObject(root, "wifi_networks", wifi_networks);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t ret = agent_response(req, "200 OK", json_str);
    free(json_str);
    return ret;
}

/**
 * @brief Handler for POST /rest/wifiSettings
 */
static esp_err_t api_wifi_settings_post_handler(httpd_req_t *req)
{
    if (!legacy_window_allowed(req)) return ESP_OK;
    if (!agent_same_origin(req)) {
        agent_response(req, "403 Forbidden", "{\"error\":\"Cross-origin Wi-Fi updates are not allowed.\"}");
        return ESP_FAIL;
    }
    cJSON *root = agent_read_json(req);
    if (!root) return ESP_FAIL;
    const cJSON *networks = cJSON_GetObjectItemCaseSensitive(root, "wifi_networks");
    const cJSON *network = cJSON_GetArrayItem(networks, 0);
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(network, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(network, "password");
    const cJSON *static_ip = cJSON_GetObjectItemCaseSensitive(network, "static_ip_config");
    if (!cJSON_IsArray(networks) || cJSON_GetArraySize(networks) != 1 ||
        !cJSON_IsString(ssid) || !cJSON_IsString(password) ||
        strlen(ssid->valuestring) == 0 || strlen(ssid->valuestring) > 32 ||
        strlen(password->valuestring) > 63 ||
        (strlen(password->valuestring) > 0 && strlen(password->valuestring) < 8) ||
        (static_ip && !cJSON_IsFalse(static_ip))) {
        cJSON_Delete(root);
        return agent_response(req, "400 Bad Request", "{\"error\":\"Provide one network, SSID of 1–32 bytes, and a blank or 8–63 byte password. Only automatic IP configuration is supported.\"}");
    }
    // GET deliberately omits the password; a blank password for the unchanged
    // network keeps the saved one (see espwifi_store_station).
    // Recheck after receiving/parsing the body: a slow upload cannot extend the window.
    if (!espwifi_provisioning_request_allowed(httpd_req_to_sockfd(req))) {
        cJSON_Delete(root);
        return agent_response(req, "403 Forbidden", "{\"error\":\"physical_local_access_required\"}");
    }
    const esp_err_t err = espwifi_store_station(ssid->valuestring, password->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return agent_response(req, "503 Service Unavailable", "{\"error\":\"Wi-Fi credentials could not be saved. Reconnect and try again.\"}");
    }
    // Return the actual settings shape, without the password, before the link
    // changes. Existing Wi-Fi event handling reconnects after disconnect.
    esp_err_t response = api_wifi_settings_get_handler(req);
    espwifi_reconnect();
    return response;
}

/**
 * @brief Handler for WiFi network scanning
 */
static esp_err_t api_wifi_scan_handler(httpd_req_t *req)
{
    if (!legacy_window_allowed(req)) return ESP_OK;
    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_list = NULL;

    // Start WiFi scan
    if (esp_wifi_scan_start(NULL, true) != ESP_OK ||
        esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) {
        return agent_response(req, "503 Service Unavailable", "{\"error\":\"Wi-Fi scan could not complete. Enter a network manually or scan again once the device is connected.\"}");
    }

    if (ap_count == 0) {
        return agent_response(req, "200 OK", "{\"networks\":[]}");
    }

    ap_list = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (ap_list == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (esp_wifi_scan_get_ap_records(&ap_count, ap_list) != ESP_OK) {
        free(ap_list);
        return agent_response(req, "503 Service Unavailable", "{\"error\":\"Wi-Fi scan results are unavailable. Scan again or enter your network manually.\"}");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();

    for (int i = 0; i < ap_count; i++) {
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", (char*)ap_list[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", ap_list[i].rssi);
        cJSON_AddNumberToObject(network, "channel", ap_list[i].primary);
        cJSON_AddNumberToObject(network, "encryption", ap_list[i].authmode);
        cJSON_AddItemToArray(networks, network);
    }

    cJSON_AddItemToObject(root, "networks", networks);
    free(ap_list);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json_str == NULL) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t ret = agent_response(req, "200 OK", json_str);
    free(json_str);
    return ret;
}

/**
 * @brief Handler for OPTIONS requests (CORS preflight)
 */
static esp_err_t api_options_handler(httpd_req_t *req)
{
    return agent_response(req, "405 Method Not Allowed", "{\"error\":\"cross_origin_access_not_supported\"}");
}

/**
 * @brief Handler for /rest/restart endpoint
 */
static esp_err_t api_restart_handler(httpd_req_t *req)
{
    return agent_response(req, "403 Forbidden", "{\"error\":\"physical_confirmation_required\"}");
}

/**
 * @brief Handler for /rest/factoryReset endpoint
 */
static esp_err_t api_factory_reset_handler(httpd_req_t *req)
{
    return agent_response(req, "403 Forbidden", "{\"error\":\"physical_confirmation_required\"}");
}

/**
 * @brief Start the web server
 */
httpd_handle_t web_server_start(void)
{
    if (web_auth_init() != ESP_OK)
        ESP_LOGW(TAG, "Owner authentication storage unavailable; settings remain locked");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // Modern browsers include client hints and the owner-session cookie.
    // HTTPD grows its scratch buffer to the received size, up to this bound.
    config.max_req_hdr_len = 4096;
    // Compile-time req.uri and parser limits must both cover encoded UTF-8 filters.
    static_assert(CONFIG_HTTPD_MAX_URI_LEN >= 2304, "History routes require CONFIG_HTTPD_MAX_URI_LEN >= 2304");
    // The 16-socket pool leaves six beyond HTTP's three internal and seven client sockets.
    config.max_open_sockets = 7;
    config.max_uri_handlers = 96; // 83 routes including the final static fallback, plus headroom.
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192; // Increase stack size for JSON operations
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 2;

    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    // Register API endpoints
    const httpd_uri_t agent_endpoints[] = {
        { .uri = "/rest/auth/status", .method = HTTP_GET, .handler = api_owner_status_handler, .user_ctx = NULL },
        { .uri = "/rest/auth/setup", .method = HTTP_POST, .handler = api_owner_setup_handler, .user_ctx = NULL },
        { .uri = "/rest/auth/login", .method = HTTP_POST, .handler = api_owner_login_handler, .user_ctx = NULL },
        { .uri = "/rest/auth/logout", .method = HTTP_POST, .handler = api_owner_logout_handler, .user_ctx = NULL },
        { .uri = "/rest/codex/status", .method = HTTP_GET, .handler = api_codex_status_handler, .user_ctx = NULL },
        { .uri = "/rest/codex/connect", .method = HTTP_POST, .handler = api_codex_connect_handler, .user_ctx = NULL },
        { .uri = "/rest/codex/cancel", .method = HTTP_POST, .handler = api_codex_cancel_handler, .user_ctx = NULL },
        { .uri = "/rest/codex/disconnect", .method = HTTP_POST, .handler = api_codex_disconnect_handler, .user_ctx = NULL },
        { .uri = "/rest/device", .method = HTTP_GET, .handler = api_device_get_handler, .user_ctx = NULL },
        { .uri = "/rest/device/control", .method = HTTP_POST, .handler = api_device_control_handler, .user_ctx = NULL },
        { .uri = "/rest/device/chat", .method = HTTP_POST, .handler = api_device_chat_handler, .user_ctx = NULL },
        { .uri = "/rest/device/reset", .method = HTTP_POST, .handler = api_device_reset_handler, .user_ctx = NULL },
        { .uri = "/rest/device/history*", .method = HTTP_GET, .handler = api_device_history_handler, .user_ctx = NULL },
        { .uri = "/rest/device/history*", .method = HTTP_POST, .handler = api_device_history_handler, .user_ctx = NULL },
        { .uri = "/rest/device/history*", .method = HTTP_DELETE, .handler = api_device_history_handler, .user_ctx = NULL },
        { .uri = "/rest/device/desk", .method = HTTP_GET, .handler = api_device_desk_get_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/readiness", .method = HTTP_POST, .handler = api_agent_readiness_handler, .user_ctx = NULL },
        { .uri = "/rest/agent", .method = HTTP_GET, .handler = api_agent_get_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/chat", .method = HTTP_POST, .handler = api_agent_chat_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/reset", .method = HTTP_POST, .handler = api_agent_reset_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/voice", .method = HTTP_POST, .handler = api_agent_voice_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/voice", .method = HTTP_OPTIONS, .handler = api_agent_options_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/settings", .method = HTTP_GET, .handler = api_agent_settings_get_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/settings", .method = HTTP_POST, .handler = api_agent_settings_post_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/settings", .method = HTTP_OPTIONS, .handler = api_agent_options_handler, .user_ctx = NULL },
        { .uri = "/rest/agent", .method = HTTP_OPTIONS, .handler = api_agent_options_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/chat", .method = HTTP_OPTIONS, .handler = api_agent_options_handler, .user_ctx = NULL },
        { .uri = "/rest/agent/reset", .method = HTTP_OPTIONS, .handler = api_agent_options_handler, .user_ctx = NULL }
    };
    for (const httpd_uri_t &endpoint : agent_endpoints) {
        if (httpd_register_uri_handler(server, &endpoint) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register agent endpoint");
            httpd_stop(server);
            return NULL;
        }
    }

    httpd_uri_t api_system_status = {
        .uri = "/rest/system/status",
        .method = HTTP_GET,
        .handler = api_system_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_system_status);

    httpd_uri_t api_wifi_status = {
        .uri = "/rest/wifi/status",
        .method = HTTP_GET,
        .handler = api_wifi_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_status);

    httpd_uri_t api_features = {
        .uri = "/rest/features",
        .method = HTTP_GET,
        .handler = api_features_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_features);

    httpd_uri_t api_features_options = {
        .uri = "/rest/features",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_features_options);

    // Register additional endpoints for Svelte UI compatibility
    httpd_uri_t api_system_status_simple = {
        .uri = "/rest/systemStatus",
        .method = HTTP_GET,
        .handler = api_system_status_simple_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_system_status_simple);

    httpd_uri_t api_system_status_options = {
        .uri = "/rest/systemStatus",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_system_status_options);

    httpd_uri_t api_wifi_status_simple = {
        .uri = "/rest/wifiStatus",
        .method = HTTP_GET,
        .handler = api_wifi_status_simple_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_status_simple);

    httpd_uri_t api_wifi_status_options = {
        .uri = "/rest/wifiStatus",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_status_options);

    httpd_uri_t api_ap_status = {
        .uri = "/rest/apStatus",
        .method = HTTP_GET,
        .handler = api_ap_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_ap_status);

    httpd_uri_t api_ap_status_options = {
        .uri = "/rest/apStatus",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_ap_status_options);

    // WiFi settings endpoints (GET and POST)
    httpd_uri_t api_wifi_settings_get = {
        .uri = "/rest/wifiSettings",
        .method = HTTP_GET,
        .handler = api_wifi_settings_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_settings_get);

    httpd_uri_t api_wifi_settings_post = {
        .uri = "/rest/wifiSettings",
        .method = HTTP_POST,
        .handler = api_wifi_settings_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_settings_post);

    // WiFi settings OPTIONS handler for CORS
    httpd_uri_t api_wifi_settings_options = {
        .uri = "/rest/wifiSettings",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_settings_options);

    // WiFi scan endpoints (both names for compatibility)
    httpd_uri_t api_wifi_scan = {
        .uri = "/rest/wifiScan",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_scan);

    httpd_uri_t api_wifi_scan_options = {
        .uri = "/rest/wifiScan",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_wifi_scan_options);

    httpd_uri_t api_list_networks = {
        .uri = "/rest/listNetworks",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_list_networks);

    httpd_uri_t api_list_networks_options = {
        .uri = "/rest/listNetworks",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_list_networks_options);

    httpd_uri_t api_scan_networks = {
        .uri = "/rest/scanNetworks",
        .method = HTTP_GET,
        .handler = api_wifi_scan_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_scan_networks);

    httpd_uri_t api_scan_networks_options = {
        .uri = "/rest/scanNetworks",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_scan_networks_options);

    httpd_uri_t api_ap_settings = {
        .uri = "/rest/apSettings",
        .method = HTTP_GET,
        .handler = api_stub_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_ap_settings);

    httpd_uri_t api_ap_settings_options = {
        .uri = "/rest/apSettings",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_ap_settings_options);

    httpd_uri_t api_light_state = {
        .uri = "/rest/lightState",
        .method = HTTP_GET,
        .handler = api_stub_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_light_state);

    httpd_uri_t api_light_state_options = {
        .uri = "/rest/lightState",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_light_state_options);

    // Restart endpoint
    httpd_uri_t api_restart = {
        .uri = "/rest/restart",
        .method = HTTP_POST,
        .handler = api_restart_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_restart);

    httpd_uri_t api_restart_options = {
        .uri = "/rest/restart",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_restart_options);

    // Factory reset endpoint
    httpd_uri_t api_factory_reset = {
        .uri = "/rest/factoryReset",
        .method = HTTP_POST,
        .handler = api_factory_reset_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_factory_reset);

    httpd_uri_t api_factory_reset_options = {
        .uri = "/rest/factoryReset",
        .method = HTTP_OPTIONS,
        .handler = api_options_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &api_factory_reset_options);

    // Stub endpoints for features not yet implemented
    const char* stub_endpoints[] = {
        "/rest/ntpStatus",
        "/rest/ntpSettings",
        "/rest/mqttStatus",
        "/rest/mqttSettings",
        "/rest/brokerSettings",
        "/rest/uploadFirmware",
        "/rest/downloadUpdate",
        "/rest/coreDump",
        "/rest/sleep"
    };

    for (int i = 0; i < sizeof(stub_endpoints) / sizeof(stub_endpoints[0]); i++) {
        httpd_uri_t stub_uri = {
            .uri = stub_endpoints[i],
            .method = HTTP_GET,
            .handler = api_stub_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stub_uri);

        // Also register POST handler for these endpoints
        httpd_uri_t stub_uri_post = {
            .uri = stub_endpoints[i],
            .method = HTTP_POST,
            .handler = api_stub_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stub_uri_post);

        // And OPTIONS for CORS
        httpd_uri_t stub_uri_options = {
            .uri = stub_endpoints[i],
            .method = HTTP_OPTIONS,
            .handler = api_options_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &stub_uri_options);
    }

    // Register wildcard handler for static files (must be last)
    httpd_uri_t uri_www = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = www_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &uri_www);

    ESP_LOGI(TAG, "Web server started successfully");
    ESP_LOGI(TAG, "Serving %d static files", WWW_DATA_COUNT);
    ESP_LOGI(TAG, "Access the web interface at: http://<device-ip>/");

    return server;
}

/**
 * @brief Stop the web server
 */
esp_err_t web_server_stop(httpd_handle_t server)
{
    if (server == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Stopping web server");
    return httpd_stop(server);
}
