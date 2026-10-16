#pragma once

#include <atomic>
#include <cstdint>
#include <string>

struct CodexAuthStatus {
    bool configured;
    bool busy;
    char state[24];
    char user_code[64];
    unsigned expires_in;
    char error[192];
    // Internal media-invalidation snapshot only; never export over HTTP.
    uint32_t account_revision;
};

struct CodexCredentials {
    std::string access_token;
    std::string account_id;
    CodexCredentials();
    ~CodexCredentials();
    CodexCredentials(const CodexCredentials &) = delete;
    CodexCredentials &operator=(const CodexCredentials &) = delete;
};

bool codex_auth_init(std::string &error);
CodexAuthStatus codex_auth_status();
bool codex_auth_begin(std::string &error);
void codex_auth_cancel();
bool codex_auth_disconnect(std::string &error);
bool codex_auth_credentials(CodexCredentials &out,
                            const std::atomic<bool> &cancelled,
                            std::string &error);
