#define _POSIX_C_SOURCE 200809L
#include "web_auth.h"
#include "host_fixture.h"
#include "mbedtls/sha256.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define US_SECOND 1000000LL
static const char *owner = "home.owner";
static const char *password = "an actual long passphrase";
static const char *replacement = "a different long passphrase";
static const char *program;
static const char *store;

static void restart_check(const char *token, bool authorized)
{
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        execl(program, program, "verify", store, token, authorized ? "1" : "0", (char *)NULL);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void *initialize_thread(void *unused)
{
    (void)unused;
    assert(web_auth_init() == ESP_OK);
    assert(!web_auth_configured());
    return NULL;
}

static void make_owner(bool remember, char token[WEB_AUTH_TOKEN_CAPACITY])
{
    char code[WEB_AUTH_CODE_CAPACITY];
    assert(web_auth_approval_start(code, sizeof(code)));
    assert(web_auth_approval_remaining() == 300);
    assert(web_auth_setup(owner, password, code, remember, token, WEB_AUTH_TOKEN_CAPACITY) == WEB_AUTH_OK);
    assert(web_auth_approval_remaining() == 0);
    assert(web_auth_authorized(token));
}

static void verify_no_plaintext(const char *token)
{
    FILE *file = fopen(store, "rb");
    assert(file);
    unsigned char bytes[512];
    size_t size = fread(bytes, 1, sizeof(bytes), file);
    assert(size > 0 && size < sizeof(bytes));
    fclose(file);
    const char *secrets[] = {password, replacement, token};
    for (unsigned s = 0; s < sizeof(secrets) / sizeof(secrets[0]); ++s) {
        size_t length = strlen(secrets[s]);
        for (size_t i = 0; i + length <= size; ++i)
            assert(memcmp(bytes + i, secrets[s], length) != 0);
    }
    unsigned char secret[32], hash[32];
    for (unsigned i = 0; i < 32; ++i) {
        char pair[] = {token[i * 2], token[i * 2 + 1], 0};
        secret[i] = (unsigned char)strtoul(pair, NULL, 16);
    }
    assert(mbedtls_sha256(secret, sizeof(secret), hash, 0) == 0);
    bool found_hash = false;
    for (size_t i = 0; i + sizeof(secret) <= size; ++i) {
        assert(memcmp(bytes + i, secret, sizeof(secret)) != 0);
        found_hash |= memcmp(bytes + i, hash, sizeof(hash)) == 0;
    }
    assert(found_hash);
}

static void approval_edges(const char *valid_token)
{
    char code[7], earlier[7], new_token[65];
    assert(web_auth_approval_start(earlier, sizeof(earlier)));
    do { assert(web_auth_approval_start(code, sizeof(code))); } while (!strcmp(code, earlier));
    assert(web_auth_setup(owner, replacement, earlier, true, new_token, sizeof(new_token)) == WEB_AUTH_DENIED);
    assert(web_auth_authorized(valid_token));
    assert(web_auth_approval_start(code, sizeof(code)));
    fixture_advance_time(300 * US_SECOND);
    assert(web_auth_setup(owner, replacement, code, true, new_token, sizeof(new_token)) == WEB_AUTH_DENIED);
    assert(!new_token[0] && !web_auth_approval_remaining());
    assert(web_auth_authorized(valid_token));
    assert(web_auth_approval_start(code, sizeof(code)));
    fixture_derivation_delay(300 * US_SECOND);
    assert(web_auth_setup(owner, replacement, code, true, new_token, sizeof(new_token)) == WEB_AUTH_DENIED);
    fixture_derivation_delay(0);
    assert(web_auth_authorized(valid_token));
    assert(!new_token[0]);
    assert(web_auth_approval_start(code, sizeof(code)));
    strcpy(earlier, code);
    earlier[0] = earlier[0] == '0' ? '1' : '0';
    for (int i = 0; i < 5; ++i)
        assert(web_auth_setup(owner, replacement, earlier, true, new_token, sizeof(new_token)) ==
               (i == 4 ? WEB_AUTH_RATE_LIMITED : WEB_AUTH_DENIED));
    assert(!web_auth_approval_remaining());
    assert(web_auth_setup(owner, replacement, code, true, new_token, sizeof(new_token)) == WEB_AUTH_DENIED);
    assert(web_auth_authorized(valid_token));
    assert(web_auth_approval_start(code, sizeof(code)));
    web_auth_approval_stop();
    assert(web_auth_setup(owner, replacement, code, true, new_token, sizeof(new_token)) == WEB_AUTH_DENIED);
    assert(!web_auth_approval_start(code, 6) && !code[0]);
}

static void throttle_edges(void)
{
    char token[65];
    uint64_t before = fixture_derivation_count();
    for (int i = 0; i < 5; ++i) {
        assert(web_auth_login(i % 2 ? owner : "someone.else", "wrong passphrase", true,
                              token, sizeof(token)) == (i == 4 ? WEB_AUTH_RATE_LIMITED : WEB_AUTH_DENIED));
        assert(!token[0]);
    }
    assert(fixture_derivation_count() == before + 5);
    before = fixture_derivation_count();
    assert(web_auth_login(owner, password, true, token, sizeof(token)) == WEB_AUTH_RATE_LIMITED);
    assert(fixture_derivation_count() == before);
    fixture_advance_time(60 * US_SECOND - 1);
    assert(web_auth_login(owner, password, true, token, sizeof(token)) == WEB_AUTH_RATE_LIMITED);
    fixture_advance_time(1);
    assert(web_auth_login(owner, password, false, token, sizeof(token)) == WEB_AUTH_OK);
    assert(web_auth_logout(token) == WEB_AUTH_OK);
}

static void core_scenario(void)
{
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) assert(pthread_create(&threads[i], NULL, initialize_thread, NULL) == 0);
    for (unsigned i = 0; i < 8; ++i) assert(pthread_join(threads[i], NULL) == 0);
    assert(web_auth_storage_available());
    char token[65];
    assert(web_auth_setup(owner, password, "123456", true, token, sizeof(token)) == WEB_AUTH_DENIED);
    assert(!web_auth_configured() && !token[0]);
    char sessions[4][65];
    make_owner(true, sessions[0]);
    char name[33];
    assert(web_auth_username(name, sizeof(name)) && !strcmp(name, owner));
    assert(!web_auth_username(name, 3) && !name[0]);
    assert(!web_auth_authorized("invalid"));
    assert(web_auth_login("bad name", password, true, token, sizeof(token)) == WEB_AUTH_INVALID);
    assert(web_auth_login(owner, "short", true, token, sizeof(token)) == WEB_AUTH_INVALID);
    assert(web_auth_login(owner, "has\ncontrol", true, token, sizeof(token)) == WEB_AUTH_INVALID);
    char uppercase[65];
    memcpy(uppercase, sessions[0], sizeof(uppercase));
    uppercase[0] = 'A';
    assert(!web_auth_authorized(uppercase));
    verify_no_plaintext(sessions[0]);
    restart_check(sessions[0], true);
    approval_edges(sessions[0]);
    throttle_edges();
    make_owner(true, sessions[0]);
    for (unsigned i = 1; i < 4; ++i)
        assert(web_auth_login(owner, password, true, sessions[i], sizeof(sessions[i])) == WEB_AUTH_OK);
    for (unsigned failure = FIXTURE_OPEN; failure <= FIXTURE_COMMIT; ++failure) {
        if (failure == FIXTURE_READ) continue;
        fixture_fail_next((enum fixture_failure)failure);
        assert(web_auth_login(owner, password, false, token, sizeof(token)) == WEB_AUTH_STORAGE_ERROR);
        assert(!token[0] && !web_auth_storage_available());
        for (unsigned i = 0; i < 4; ++i) assert(web_auth_authorized(sessions[i]));
        restart_check(sessions[0], true);
    }
    assert(web_auth_login(owner, password, false, token, sizeof(token)) == WEB_AUTH_OK);
    assert(!web_auth_authorized(sessions[0]));
    assert(web_auth_authorized(token));
    restart_check(sessions[0], false);
    restart_check(token, false);
    for (unsigned i = 1; i < 4; ++i) restart_check(sessions[i], true);
    fixture_advance_time(8 * 60 * 60 * US_SECOND - 1);
    assert(web_auth_authorized(token));
    fixture_advance_time(1);
    assert(!web_auth_authorized(token));
    assert(web_auth_authorized(sessions[1]));
    for (unsigned failure = FIXTURE_OPEN; failure <= FIXTURE_COMMIT; ++failure) {
        if (failure == FIXTURE_READ) continue;
        fixture_fail_next((enum fixture_failure)failure);
        assert(web_auth_logout(sessions[1]) == WEB_AUTH_STORAGE_ERROR);
        assert(web_auth_authorized(sessions[1]));
        restart_check(sessions[1], true);
    }
    assert(web_auth_logout(sessions[1]) == WEB_AUTH_OK);
    assert(!web_auth_authorized(sessions[1]));
    restart_check(sessions[1], false);
    assert(web_auth_logout(sessions[1]) == WEB_AUTH_OK);
    char code[7];
    for (unsigned failure = FIXTURE_OPEN; failure <= FIXTURE_COMMIT; ++failure) {
        if (failure == FIXTURE_READ) continue;
        assert(web_auth_approval_start(code, sizeof(code)));
        fixture_fail_next((enum fixture_failure)failure);
        assert(web_auth_setup("new.owner", replacement, code, true, token, sizeof(token)) == WEB_AUTH_STORAGE_ERROR);
        assert(!token[0]);
        assert(web_auth_username(name, sizeof(name)) && !strcmp(name, owner));
        assert(web_auth_authorized(sessions[2]) && web_auth_authorized(sessions[3]));
        restart_check(sessions[2], true);
        assert(web_auth_approval_remaining());
    }
    assert(web_auth_setup("new.owner", replacement, code, true, token, sizeof(token)) == WEB_AUTH_OK);
    assert(!web_auth_authorized(sessions[2]) && !web_auth_authorized(sessions[3]));
    assert(web_auth_authorized(token) && web_auth_storage_available());
    restart_check(token, true);
    restart_check(sessions[2], false);
    assert(web_auth_login(owner, password, false, sessions[0], sizeof(sessions[0])) == WEB_AUTH_DENIED);
    assert(web_auth_login("new.owner", replacement, false, sessions[0], sizeof(sessions[0])) == WEB_AUTH_OK);
    verify_no_plaintext(token);
}

static void damaged_scenario(const char *kind)
{
    if (!strcmp(kind, "unavailable")) {
        assert(web_auth_init() != ESP_OK);
        fixture_nvs_available(true);
    } else {
        FILE *file = fopen(store, "wb");
        assert(file);
        if (!strcmp(kind, "corrupt")) {
            unsigned char garbage[258] = {0};
            assert(fwrite(garbage, 1, sizeof(garbage), file) == sizeof(garbage));
        } else {
            assert(fwrite("bad", 1, 3, file) == 3);
        }
        assert(fclose(file) == 0);
        if (!strcmp(kind, "readfailure")) fixture_fail_next(FIXTURE_READ);
        assert(web_auth_init() != ESP_OK);
    }
    assert(!web_auth_storage_available() && !web_auth_configured());
    char token[65], code[7];
    assert(web_auth_login(owner, password, true, token, sizeof(token)) == WEB_AUTH_STORAGE_ERROR);
    assert(web_auth_setup(owner, password, "123456", true, token, sizeof(token)) == WEB_AUTH_DENIED);
    assert(web_auth_approval_start(code, sizeof(code)));
    fixture_fail_next(FIXTURE_SET);
    assert(web_auth_setup(owner, password, code, true, token, sizeof(token)) == WEB_AUTH_STORAGE_ERROR);
    assert(!web_auth_configured() && !token[0]);
    assert(web_auth_setup(owner, password, code, true, token, sizeof(token)) == WEB_AUTH_OK);
    assert(web_auth_init() == ESP_OK && web_auth_storage_available());
    restart_check(token, true);
}

int main(int argc, char **argv)
{
    assert(argc >= 3);
    program = argv[0];
    store = argv[2];
    fixture_init(store, strcmp(argv[1], "unavailable") != 0);
    if (!strcmp(argv[1], "verify")) {
        assert(argc == 5);
        assert(web_auth_init() == ESP_OK);
        assert(web_auth_authorized(argv[3]) == (strcmp(argv[4], "1") == 0));
        return 0;
    }
    unlink(store);
    if (!strcmp(argv[1], "core")) core_scenario();
    else if (!strcmp(argv[1], "physical-first")) {
        /* The physical callback lazily initializes without server startup. */
        char token[65];
        make_owner(false, token);
        restart_check(token, false);
    } else damaged_scenario(argv[1]);
    printf("web_auth actual-component scenario passed: %s\n", argv[1]);
    return 0;
}
