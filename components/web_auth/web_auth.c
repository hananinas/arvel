#include "web_auth.h"

#include <limits.h>
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define PASSWORD_ITERATIONS 100000U
#define SESSION_COUNT 4U
#define HASH_SIZE 32U
#define SALT_SIZE 16U
#define APPROVAL_US (300LL * 1000000LL)
#define SESSION_US (8LL * 60LL * 60LL * 1000000LL)
#define LOGIN_WINDOW_US (60LL * 1000000LL)
#define ATTEMPT_LIMIT 5U

/* Explicit byte layout, not a compiler-dependent struct. Only remembered
 * sessions enter NVS. The trailing SHA256 detects corruption, not tampering. */
enum {
    RECORD_VERSION = 1,
    OFF_VERSION = 4,
    OFF_ITERATIONS = 8,
    OFF_NEXT = 12,
    OFF_USERNAME = 13,
    OFF_SALT = OFF_USERNAME + WEB_AUTH_USERNAME_CAPACITY,
    OFF_PASSWORD = OFF_SALT + SALT_SIZE,
    OFF_SESSIONS = OFF_PASSWORD + HASH_SIZE,
    OFF_CHECKSUM = OFF_SESSIONS + SESSION_COUNT * (1 + HASH_SIZE),
    RECORD_SIZE = OFF_CHECKSUM + HASH_SIZE
};

typedef struct {
    uint8_t kind; /* 0 = free, 1 = temporary, 2 = remembered */
    uint8_t hash[HASH_SIZE];
    int64_t expires;
} session_t;

typedef struct {
    bool configured;
    char username[WEB_AUTH_USERNAME_CAPACITY];
    uint8_t salt[SALT_SIZE];
    uint8_t password_hash[HASH_SIZE];
    session_t sessions[SESSION_COUNT];
    uint8_t next;
} account_t;

static portMUX_TYPE init_guard = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static bool initialized;
static bool storage_available;
static esp_err_t init_result = ESP_ERR_INVALID_STATE;
static account_t account;
static uint8_t approval_hash[HASH_SIZE];
static int64_t approval_deadline;
static unsigned approval_failures;
static unsigned login_failures;
static int64_t login_window_start;
static int64_t login_cooldown_until;

static bool equal_bytes(const void *a, const void *b, size_t size)
{
    const volatile uint8_t *left = (const volatile uint8_t *)a;
    const volatile uint8_t *right = (const volatile uint8_t *)b;
    uint8_t different = 0;
    for (size_t i = 0; i < size; ++i) different |= left[i] ^ right[i];
    return different == 0;
}

static void clear_output(char *output, size_t capacity)
{
    if (output && capacity) mbedtls_platform_zeroize(output, capacity);
}

static size_t bounded_length(const char *text, size_t maximum)
{
    if (!text) return maximum + 1;
    size_t length = 0;
    while (length <= maximum && text[length]) ++length;
    return length;
}

static bool valid_username(const char *username)
{
    size_t length = bounded_length(username, WEB_AUTH_USERNAME_CAPACITY - 1);
    if (!length || length >= WEB_AUTH_USERNAME_CAPACITY) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)username[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

static bool valid_password(const char *password)
{
    size_t length = bounded_length(password, 128);
    if (length < 8 || length > 128) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)password[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

static void put_u32(uint8_t *output, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) output[i] = (uint8_t)(value >> (i * 8));
}

static uint32_t get_u32(const uint8_t *input)
{
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)input[i] << (i * 8);
    return value;
}

static bool pack_record(const account_t *source, uint8_t bytes[RECORD_SIZE])
{
    memset(bytes, 0, RECORD_SIZE);
    memcpy(bytes, "WOWN", 4);
    put_u32(bytes + OFF_VERSION, RECORD_VERSION);
    put_u32(bytes + OFF_ITERATIONS, PASSWORD_ITERATIONS);
    bytes[OFF_NEXT] = source->next;
    memcpy(bytes + OFF_USERNAME, source->username, WEB_AUTH_USERNAME_CAPACITY);
    memcpy(bytes + OFF_SALT, source->salt, SALT_SIZE);
    memcpy(bytes + OFF_PASSWORD, source->password_hash, HASH_SIZE);
    for (unsigned i = 0; i < SESSION_COUNT; ++i) {
        uint8_t *slot = bytes + OFF_SESSIONS + i * (1 + HASH_SIZE);
        if (source->sessions[i].kind == 2) {
            slot[0] = 1;
            memcpy(slot + 1, source->sessions[i].hash, HASH_SIZE);
        }
    }
    return mbedtls_sha256(bytes, OFF_CHECKSUM, bytes + OFF_CHECKSUM, 0) == 0;
}

static bool unpack_record(const uint8_t bytes[RECORD_SIZE], account_t *destination)
{
    uint8_t checksum[HASH_SIZE];
    if (mbedtls_sha256(bytes, OFF_CHECKSUM, checksum, 0) != 0) return false;
    bool valid = equal_bytes(checksum, bytes + OFF_CHECKSUM, HASH_SIZE);
    mbedtls_platform_zeroize(checksum, sizeof(checksum));
    if (!valid || memcmp(bytes, "WOWN", 4) != 0 ||
        get_u32(bytes + OFF_VERSION) != RECORD_VERSION ||
        get_u32(bytes + OFF_ITERATIONS) != PASSWORD_ITERATIONS ||
        bytes[OFF_NEXT] >= SESSION_COUNT) return false;
    /* Reject a non-canonical username field, including trailing garbage. */
    const char *username = (const char *)bytes + OFF_USERNAME;
    if (!memchr(username, 0, WEB_AUTH_USERNAME_CAPACITY) || !valid_username(username)) return false;
    size_t length = strlen(username);
    for (size_t i = length; i < WEB_AUTH_USERNAME_CAPACITY; ++i)
        if (username[i] != 0) return false;
    memset(destination, 0, sizeof(*destination));
    memcpy(destination->username, username, WEB_AUTH_USERNAME_CAPACITY);
    memcpy(destination->salt, bytes + OFF_SALT, SALT_SIZE);
    memcpy(destination->password_hash, bytes + OFF_PASSWORD, HASH_SIZE);
    destination->next = bytes[OFF_NEXT];
    for (unsigned i = 0; i < SESSION_COUNT; ++i) {
        const uint8_t *slot = bytes + OFF_SESSIONS + i * (1 + HASH_SIZE);
        if (slot[0] > 1) return false;
        if (slot[0]) {
            destination->sessions[i].kind = 2;
            memcpy(destination->sessions[i].hash, slot + 1, HASH_SIZE);
        } else {
            for (unsigned j = 1; j <= HASH_SIZE; ++j) if (slot[j]) return false;
        }
    }
    destination->configured = true;
    return true;
}

static void initialize_locked(void)
{
    if (initialized) return;
    initialized = true;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("web_owner", NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        storage_available = true;
        init_result = ESP_OK;
        return;
    }
    if (result != ESP_OK) {
        init_result = result;
        return;
    }
    uint8_t bytes[RECORD_SIZE] = {0};
    size_t length = 0;
    result = nvs_get_blob(handle, "record", NULL, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        storage_available = true;
        init_result = ESP_OK;
    } else if (result != ESP_OK) {
        init_result = result;
    } else if (length != RECORD_SIZE) {
        init_result = ESP_ERR_INVALID_STATE;
    } else {
        result = nvs_get_blob(handle, "record", bytes, &length);
        account_t loaded = {0};
        if (result == ESP_OK && length == RECORD_SIZE && unpack_record(bytes, &loaded)) {
            account = loaded;
            storage_available = true;
            init_result = ESP_OK;
        } else {
            init_result = result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
        }
        mbedtls_platform_zeroize(&loaded, sizeof(loaded));
    }
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    nvs_close(handle);
}

static bool lock_core(void)
{
    /* Static allocation and a short creation guard allow the physical UI and
     * server tasks to race their first call without duplicate initialization. */
    portENTER_CRITICAL(&init_guard);
    if (!mutex) mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    SemaphoreHandle_t handle = mutex;
    portEXIT_CRITICAL(&init_guard);
    if (!handle || xSemaphoreTake(handle, portMAX_DELAY) != pdTRUE) return false;
    initialize_locked();
    return true;
}

static void unlock_core(void)
{
    xSemaphoreGive(mutex);
}

static web_auth_result_t persist_account(const account_t *candidate)
{
    uint8_t bytes[RECORD_SIZE];
    if (!pack_record(candidate, bytes)) {
        mbedtls_platform_zeroize(bytes, sizeof(bytes));
        return WEB_AUTH_UNAVAILABLE;
    }
    nvs_handle_t handle;
    esp_err_t result = nvs_open("web_owner", NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, "record", bytes, sizeof(bytes));
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    mbedtls_platform_zeroize(bytes, sizeof(bytes));
    storage_available = result == ESP_OK;
    if (result != ESP_OK) return WEB_AUTH_STORAGE_ERROR;
    init_result = ESP_OK;
    return WEB_AUTH_OK;
}

static void stop_approval_locked(void)
{
    approval_deadline = 0;
    approval_failures = 0;
    mbedtls_platform_zeroize(approval_hash, sizeof(approval_hash));
}

static bool approval_open_locked(int64_t now)
{
    if (!approval_deadline || now >= approval_deadline) {
        stop_approval_locked();
        return false;
    }
    return true;
}

static bool password_hash(const char *password, const uint8_t salt[SALT_SIZE], uint8_t hash[HASH_SIZE])
{
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
            (const unsigned char *)password, strlen(password), salt, SALT_SIZE,
            PASSWORD_ITERATIONS, HASH_SIZE, hash) == 0;
}

static bool token_hash(const char *token, uint8_t hash[HASH_SIZE])
{
    if (bounded_length(token, WEB_AUTH_TOKEN_CAPACITY - 1) != WEB_AUTH_TOKEN_CAPACITY - 1) return false;
    uint8_t secret[HASH_SIZE];
    for (unsigned i = 0; i < HASH_SIZE; ++i) {
        unsigned char a = (unsigned char)token[i * 2];
        unsigned char b = (unsigned char)token[i * 2 + 1];
        if (!((a >= '0' && a <= '9') || (a >= 'a' && a <= 'f')) ||
            !((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f'))) {
            mbedtls_platform_zeroize(secret, sizeof(secret));
            return false;
        }
        unsigned high = a <= '9' ? a - '0' : a - 'a' + 10;
        unsigned low = b <= '9' ? b - '0' : b - 'a' + 10;
        secret[i] = (uint8_t)((high << 4) | low);
    }
    bool ok = mbedtls_sha256(secret, sizeof(secret), hash, 0) == 0;
    mbedtls_platform_zeroize(secret, sizeof(secret));
    return ok;
}

static bool new_token(char token[WEB_AUTH_TOKEN_CAPACITY], uint8_t hash[HASH_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t secret[HASH_SIZE];
    esp_fill_random(secret, sizeof(secret));
    bool ok = mbedtls_sha256(secret, sizeof(secret), hash, 0) == 0;
    if (ok) {
        for (unsigned i = 0; i < HASH_SIZE; ++i) {
            token[i * 2] = hex[secret[i] >> 4];
            token[i * 2 + 1] = hex[secret[i] & 15];
        }
        token[WEB_AUTH_TOKEN_CAPACITY - 1] = 0;
    }
    mbedtls_platform_zeroize(secret, sizeof(secret));
    return ok;
}

static bool session_live(const session_t *session, int64_t now)
{
    return session->kind == 2 || (session->kind == 1 && now < session->expires);
}

static unsigned choose_slot(const account_t *source, int64_t now)
{
    for (unsigned i = 0; i < SESSION_COUNT; ++i) {
        unsigned slot = (source->next + i) % SESSION_COUNT;
        if (!session_live(&source->sessions[slot], now)) return slot;
    }
    return source->next;
}

static void install_session(account_t *destination, unsigned slot,
                            const uint8_t hash[HASH_SIZE], bool remember, int64_t now)
{
    session_t *session = &destination->sessions[slot];
    memset(session, 0, sizeof(*session));
    session->kind = remember ? 2 : 1;
    memcpy(session->hash, hash, HASH_SIZE);
    session->expires = remember ? 0 : now + SESSION_US;
    destination->next = (uint8_t)((slot + 1) % SESSION_COUNT);
}

esp_err_t web_auth_init(void)
{
    if (!lock_core()) return ESP_ERR_NO_MEM;
    esp_err_t result = init_result;
    unlock_core();
    return result;
}

bool web_auth_configured(void)
{
    if (!lock_core()) return false;
    bool result = account.configured;
    unlock_core();
    return result;
}

bool web_auth_storage_available(void)
{
    if (!lock_core()) return false;
    bool result = storage_available;
    unlock_core();
    return result;
}

bool web_auth_approval_start(char *code, size_t capacity)
{
    clear_output(code, capacity);
    if (!lock_core()) return false;
    uint8_t previous_hash[HASH_SIZE];
    memcpy(previous_hash, approval_hash, sizeof(previous_hash));
    bool had_previous = approval_deadline != 0;
    stop_approval_locked();
    bool success = false;
    if (code && capacity >= WEB_AUTH_CODE_CAPACITY) {
        char fresh[WEB_AUTH_CODE_CAPACITY] = {0};
        /* Rejection sampling avoids modulo bias in six decimal digits. Also
         * reject a repeat of the cancelled code, not just its old deadline. */
        const uint32_t bound = UINT32_MAX - (UINT32_MAX % 1000000U);
        for (;;) {
            uint32_t random;
            do { random = esp_random(); } while (random >= bound);
            unsigned value = random % 1000000U;
            for (int i = 5; i >= 0; --i) {
                fresh[i] = (char)('0' + value % 10);
                value /= 10;
            }
            if (mbedtls_sha256((const unsigned char *)fresh, 6, approval_hash, 0) != 0) break;
            if (had_previous && equal_bytes(previous_hash, approval_hash, HASH_SIZE)) continue;
            approval_deadline = esp_timer_get_time() + APPROVAL_US;
            memcpy(code, fresh, sizeof(fresh));
            success = true;
            break;
        }
        mbedtls_platform_zeroize(fresh, sizeof(fresh));
    }
    mbedtls_platform_zeroize(previous_hash, sizeof(previous_hash));
    if (!success) stop_approval_locked();
    unlock_core();
    return success;
}

void web_auth_approval_stop(void)
{
    if (!lock_core()) return;
    stop_approval_locked();
    unlock_core();
}

uint32_t web_auth_approval_remaining(void)
{
    if (!lock_core()) return 0;
    int64_t now = esp_timer_get_time();
    uint32_t remaining = approval_open_locked(now) ?
        (uint32_t)((approval_deadline - now + 999999) / 1000000) : 0;
    unlock_core();
    return remaining;
}

web_auth_result_t web_auth_setup(const char *username, const char *password,
                                const char *code, bool remember, char *token, size_t capacity)
{
    clear_output(token, capacity);
    if (!token || capacity < WEB_AUTH_TOKEN_CAPACITY ||
        !valid_username(username) || !valid_password(password)) return WEB_AUTH_INVALID;
    if (!lock_core()) return WEB_AUTH_UNAVAILABLE;
    web_auth_result_t result = WEB_AUTH_DENIED;
    uint8_t submitted_hash[HASH_SIZE] = {0};
    char fresh_token[WEB_AUTH_TOKEN_CAPACITY] = {0};
    uint8_t session_hash[HASH_SIZE] = {0};
    account_t candidate = {0};
    if (!approval_open_locked(esp_timer_get_time())) goto done;
    bool code_format = bounded_length(code, 6) == 6;
    if (code_format) {
        for (unsigned i = 0; i < 6; ++i)
            if (code[i] < '0' || code[i] > '9') code_format = false;
    }
    if (code_format && mbedtls_sha256((const unsigned char *)code, 6, submitted_hash, 0) != 0) {
        result = WEB_AUTH_UNAVAILABLE;
        goto done;
    }
    if (!code_format || !equal_bytes(submitted_hash, approval_hash, HASH_SIZE)) {
        if (++approval_failures >= ATTEMPT_LIMIT) {
            stop_approval_locked();
            result = WEB_AUTH_RATE_LIMITED;
        }
        goto done;
    }
    candidate.configured = true;
    memcpy(candidate.username, username, strlen(username));
    esp_fill_random(candidate.salt, sizeof(candidate.salt));
    if (!password_hash(password, candidate.salt, candidate.password_hash) ||
        !new_token(fresh_token, session_hash)) {
        result = WEB_AUTH_UNAVAILABLE;
        goto done;
    }
    /* PBKDF2 can take time. An approval that expires during derivation cannot
     * replace the owner; likewise issue temporary sessions from current time. */
    int64_t now = esp_timer_get_time();
    if (!approval_open_locked(now)) goto done;
    install_session(&candidate, 0, session_hash, remember, now);
    result = persist_account(&candidate);
    if (result == WEB_AUTH_OK) {
        account = candidate;
        stop_approval_locked();
        login_failures = 0;
        login_window_start = 0;
        login_cooldown_until = 0;
        memcpy(token, fresh_token, WEB_AUTH_TOKEN_CAPACITY);
    }
done:
    mbedtls_platform_zeroize(submitted_hash, sizeof(submitted_hash));
    mbedtls_platform_zeroize(session_hash, sizeof(session_hash));
    mbedtls_platform_zeroize(fresh_token, sizeof(fresh_token));
    mbedtls_platform_zeroize(&candidate, sizeof(candidate));
    unlock_core();
    return result;
}

static web_auth_result_t failed_login(int64_t now)
{
    if (!login_failures || now - login_window_start >= LOGIN_WINDOW_US) {
        login_window_start = now;
        login_failures = 0;
    }
    if (++login_failures >= ATTEMPT_LIMIT) {
        login_cooldown_until = now + LOGIN_WINDOW_US;
        return WEB_AUTH_RATE_LIMITED;
    }
    return WEB_AUTH_DENIED;
}

web_auth_result_t web_auth_login(const char *username, const char *password,
                                bool remember, char *token, size_t capacity)
{
    clear_output(token, capacity);
    if (!token || capacity < WEB_AUTH_TOKEN_CAPACITY ||
        !valid_username(username) || !valid_password(password)) return WEB_AUTH_INVALID;
    if (!lock_core()) return WEB_AUTH_UNAVAILABLE;
    web_auth_result_t result;
    uint8_t derived[HASH_SIZE] = {0};
    uint8_t session_hash[HASH_SIZE] = {0};
    char fresh_token[WEB_AUTH_TOKEN_CAPACITY] = {0};
    char padded_username[WEB_AUTH_USERNAME_CAPACITY] = {0};
    account_t candidate = {0};
    int64_t now = esp_timer_get_time();
    if (!account.configured && init_result != ESP_OK) {
        result = WEB_AUTH_STORAGE_ERROR;
        goto done;
    }
    if (now < login_cooldown_until) {
        result = WEB_AUTH_RATE_LIMITED;
        goto done;
    }
    if (!account.configured) {
        result = failed_login(now);
        goto done;
    }
    /* Derive even for a wrong username, then compare fixed-length fields. */
    if (!password_hash(password, account.salt, derived)) {
        result = WEB_AUTH_UNAVAILABLE;
        goto done;
    }
    memcpy(padded_username, username, strlen(username));
    bool username_matches = equal_bytes(padded_username, account.username, sizeof(padded_username));
    bool password_matches = equal_bytes(derived, account.password_hash, HASH_SIZE);
    if (!username_matches || !password_matches) {
        result = failed_login(esp_timer_get_time());
        goto done;
    }
    if (!new_token(fresh_token, session_hash)) {
        result = WEB_AUTH_UNAVAILABLE;
        goto done;
    }
    now = esp_timer_get_time();
    unsigned slot = choose_slot(&account, now);
    bool needs_persistence = remember || account.sessions[slot].kind == 2;
    candidate = account;
    install_session(&candidate, slot, session_hash, remember, now);
    result = needs_persistence ? persist_account(&candidate) : WEB_AUTH_OK;
    if (result == WEB_AUTH_OK) {
        account = candidate;
        /* Successful logins do not replenish the shared failed-login budget. */
        memcpy(token, fresh_token, WEB_AUTH_TOKEN_CAPACITY);
    }
done:
    mbedtls_platform_zeroize(derived, sizeof(derived));
    mbedtls_platform_zeroize(session_hash, sizeof(session_hash));
    mbedtls_platform_zeroize(padded_username, sizeof(padded_username));
    mbedtls_platform_zeroize(fresh_token, sizeof(fresh_token));
    mbedtls_platform_zeroize(&candidate, sizeof(candidate));
    unlock_core();
    return result;
}

bool web_auth_authorized(const char *token)
{
    uint8_t hash[HASH_SIZE] = {0};
    if (!token_hash(token, hash)) return false;
    if (!lock_core()) {
        mbedtls_platform_zeroize(hash, sizeof(hash));
        return false;
    }
    int64_t now = esp_timer_get_time();
    bool result = false;
    for (unsigned i = 0; i < SESSION_COUNT; ++i) {
        bool matches = equal_bytes(hash, account.sessions[i].hash, HASH_SIZE);
        result |= account.configured && session_live(&account.sessions[i], now) && matches;
    }
    mbedtls_platform_zeroize(hash, sizeof(hash));
    unlock_core();
    return result;
}

web_auth_result_t web_auth_logout(const char *token)
{
    uint8_t hash[HASH_SIZE] = {0};
    if (!token_hash(token, hash)) return WEB_AUTH_INVALID;
    if (!lock_core()) {
        mbedtls_platform_zeroize(hash, sizeof(hash));
        return WEB_AUTH_UNAVAILABLE;
    }
    web_auth_result_t result = WEB_AUTH_OK;
    account_t candidate = account;
    bool changed = false;
    bool remembered = false;
    for (unsigned i = 0; i < SESSION_COUNT; ++i) {
        bool matches = equal_bytes(hash, account.sessions[i].hash, HASH_SIZE);
        if (account.sessions[i].kind && matches) {
            remembered |= account.sessions[i].kind == 2;
            mbedtls_platform_zeroize(&candidate.sessions[i], sizeof(candidate.sessions[i]));
            changed = true;
        }
    }
    if (remembered) result = persist_account(&candidate);
    if (changed && result == WEB_AUTH_OK) account = candidate;
    mbedtls_platform_zeroize(&candidate, sizeof(candidate));
    mbedtls_platform_zeroize(hash, sizeof(hash));
    unlock_core();
    return result;
}

bool web_auth_username(char *username, size_t capacity)
{
    clear_output(username, capacity);
    if (!username || !capacity || !lock_core()) return false;
    bool success = account.configured && capacity > strlen(account.username);
    if (success) memcpy(username, account.username, strlen(account.username) + 1);
    unlock_core();
    return success;
}
