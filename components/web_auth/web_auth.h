#ifndef WEB_AUTH_H
#define WEB_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_AUTH_CODE_CAPACITY 7
#define WEB_AUTH_TOKEN_CAPACITY 65
#define WEB_AUTH_USERNAME_CAPACITY 33

typedef enum {
    WEB_AUTH_OK = 0,
    WEB_AUTH_INVALID,
    WEB_AUTH_DENIED,
    WEB_AUTH_RATE_LIMITED,
    WEB_AUTH_STORAGE_ERROR,
    WEB_AUTH_UNAVAILABLE
} web_auth_result_t;

/* All operations are serialized and lazily initialize the core. Initialization
 * errors never erase NVS; physically approved setup may repair a bad record. */
esp_err_t web_auth_init(void);
bool web_auth_configured(void);
bool web_auth_storage_available(void);

/* Call start ONLY from a physical device action. Never send its output to HTTP
 * or logs. A new window cancels the previous code; five bad codes close it. */
bool web_auth_approval_start(char *code, size_t capacity);
void web_auth_approval_stop(void);
uint32_t web_auth_approval_remaining(void);

/* Output tokens are cleared on failure. Remembered sessions have no calendar
 * expiry: they last until logout, physical recovery, or bounded slot eviction.
 * Temporary sessions expire eight monotonic hours after issuance. */
web_auth_result_t web_auth_setup(const char *username, const char *password,
                                const char *code, bool remember,
                                char *token, size_t capacity);
web_auth_result_t web_auth_login(const char *username, const char *password,
                                bool remember, char *token, size_t capacity);
bool web_auth_authorized(const char *token);
web_auth_result_t web_auth_logout(const char *token);
bool web_auth_username(char *username, size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
