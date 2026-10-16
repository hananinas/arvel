#define _POSIX_C_SOURCE 200809L
#include "host_fixture.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <unistd.h>

#define FIXTURE_RECORD_MAX 512
static char record_path[4096];
static bool nvs_available;
static enum fixture_failure next_failure;
static unsigned char pending[FIXTURE_RECORD_MAX];
static size_t pending_size;
static bool pending_valid;
static atomic_int_fast64_t monotonic_us;
static atomic_int_fast64_t derivation_delay;
static atomic_uint_fast64_t derivation_count;

void fixture_init(const char *path, bool available)
{
    assert(path && strlen(path) < sizeof(record_path));
    strcpy(record_path, path);
    nvs_available = available;
    next_failure = FIXTURE_NONE;
    pending_valid = false;
    atomic_store(&monotonic_us, 1000000);
    atomic_store(&derivation_delay, 0);
    atomic_store(&derivation_count, 0);
}

void fixture_fail_next(enum fixture_failure failure) { next_failure = failure; }
void fixture_nvs_available(bool available) { nvs_available = available; }
void fixture_advance_time(int64_t us) { atomic_fetch_add(&monotonic_us, us); }
void fixture_derivation_delay(int64_t us) { atomic_store(&derivation_delay, us); }
uint64_t fixture_derivation_count(void) { return atomic_load(&derivation_count); }
int64_t esp_timer_get_time(void) { return atomic_load(&monotonic_us); }

static bool failing(enum fixture_failure failure)
{
    if (next_failure != failure) return false;
    next_failure = FIXTURE_NONE;
    return true;
}

void esp_fill_random(void *output, size_t length)
{
    unsigned char *bytes = output;
    while (length) {
        ssize_t count = getrandom(bytes, length, 0);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        bytes += count;
        length -= (size_t)count;
    }
}

uint32_t esp_random(void)
{
    uint32_t value;
    esp_fill_random(&value, sizeof(value));
    return value;
}

int mbedtls_pkcs5_pbkdf2_hmac_ext(int md_type, const unsigned char *password,
    size_t length, const unsigned char *salt, size_t salt_length,
    unsigned iterations, uint32_t key_length, unsigned char *output)
{
    assert(md_type == MBEDTLS_MD_SHA256 && iterations >= 100000);
    assert(length <= INT_MAX && salt_length <= INT_MAX && iterations <= INT_MAX && key_length <= INT_MAX);
    atomic_fetch_add(&derivation_count, 1);
    int result = PKCS5_PBKDF2_HMAC((const char *)password, (int)length, salt,
        (int)salt_length, (int)iterations, EVP_sha256(), (int)key_length, output);
    fixture_advance_time(atomic_load(&derivation_delay));
    return result == 1 ? 0 : -1;
}

int mbedtls_sha256(const unsigned char *input, size_t length, unsigned char *output, int is224)
{
    unsigned int size = 0;
    assert(!is224);
    return EVP_Digest(input, length, output, &size, EVP_sha256(), NULL) == 1 && size == 32 ? 0 : -1;
}

void mbedtls_platform_zeroize(void *buffer, size_t size) { OPENSSL_cleanse(buffer, size); }

esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{
    assert(strcmp(name, "web_owner") == 0);
    if (!nvs_available) return ESP_ERR_NVS_NOT_INITIALIZED;
    if (failing(FIXTURE_OPEN)) return ESP_FAIL;
    if (mode == NVS_READONLY && access(record_path, F_OK) != 0) return ESP_ERR_NVS_NOT_FOUND;
    *handle = (unsigned)mode + 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *bytes, size_t *size)
{
    assert(handle && strcmp(key, "record") == 0);
    if (failing(FIXTURE_READ)) return ESP_FAIL;
    FILE *file = fopen(record_path, "rb");
    if (!file) return ESP_ERR_NVS_NOT_FOUND;
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length >= 0 && fseek(file, 0, SEEK_SET) == 0);
    esp_err_t result = ESP_OK;
    if (bytes) {
        if (*size < (size_t)length) result = ESP_ERR_NVS_INVALID_LENGTH;
        else if (fread(bytes, 1, (size_t)length, file) != (size_t)length) result = ESP_FAIL;
    }
    *size = (size_t)length;
    fclose(file);
    return result;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *bytes, size_t size)
{
    assert(handle == 2 && strcmp(key, "record") == 0 && size <= sizeof(pending));
    if (failing(FIXTURE_SET)) return ESP_FAIL;
    memcpy(pending, bytes, size);
    pending_size = size;
    pending_valid = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 2 && pending_valid);
    if (failing(FIXTURE_COMMIT)) return ESP_FAIL;
    char temporary[sizeof(record_path) + 8];
    assert(snprintf(temporary, sizeof(temporary), "%s.tmp", record_path) > 0);
    FILE *file = fopen(temporary, "wb");
    if (!file) return ESP_FAIL;
    bool ok = fwrite(pending, 1, pending_size, file) == pending_size;
    if (fflush(file) != 0 || fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok || rename(temporary, record_path) != 0) {
        unlink(temporary);
        return ESP_FAIL;
    }
    pending_valid = false;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle);
    OPENSSL_cleanse(pending, sizeof(pending));
    pending_valid = false;
    pending_size = 0;
}
