#include "codex_auth.hpp"
#include "host_fixture.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
using fixture::Grant;
constexpr const char *account_a = "synthetic-account-A";
constexpr const char *account_b = "synthetic-account-B";
std::atomic<bool> not_cancelled{false};
std::string error;

CodexAuthStatus status(const char *expected, bool configured, bool busy) {
    const auto s = codex_auth_status();
    REQUIRE(!std::strcmp(s.state, expected) && s.configured == configured && s.busy == busy,
        "public status reports the resulting state, committed-account presence and pending-login flag");
    REQUIRE(s.expires_in <= 900, "public code lifetime never exceeds fifteen minutes");
    if (!busy) REQUIRE(!s.user_code[0] && s.expires_in == 0, "idle status does not leak a stale user code");
    // Synthetic credential sentinels must never appear in public error/codes.
    REQUIRE(!std::strstr(s.error, "synthetic-") && !std::strstr(s.error, "e30."), "public errors contain no token account or grant material");
    return s;
}
void idle() { fixture::await([] { return !codex_auth_status().busy; }, "asynchronous login reaches a terminal public state"); }
void usable(const std::string &account, int64_t expires) {
    CodexCredentials out;
    REQUIRE(codex_auth_credentials(out, not_cancelled, error), "consumer receives usable committed credentials");
    REQUIRE(out.account_id == account && out.access_token == fixture::jwt(account, expires), "consumer credentials exactly match the committed account and access token");
}
void unavailable() {
    CodexCredentials out; out.access_token = "synthetic-stale-output"; out.account_id = account_a;
    REQUIRE(!codex_auth_credentials(out, not_cancelled, error) && out.access_token.empty() && out.account_id.empty() && !error.empty(), "unavailable credentials fail truthfully and clear stale caller output");
}
uint32_t u32(const std::vector<unsigned char> &v, size_t offset) {
    REQUIRE(offset + 4 <= v.size(), "persisted header is bounded");
    uint32_t n = 0;
    for (unsigned i = 0; i < 4; ++i) n |= static_cast<uint32_t>(v[offset + i]) << (i * 8);
    return n;
}
void persisted(const std::string &account, int64_t expires, const std::string &refresh) {
    auto v = fixture::record();
    REQUIRE(v.size() >= 24 && !std::memcmp(v.data(), "CAX1", 4), "actual NVS file contains the production credential record");
    const size_t a = u32(v, 4), r = u32(v, 8), c = u32(v, 12);
    REQUIRE(v.size() == 24 + a + r + c, "actual persisted credential record has exact length");
    REQUIRE((u32(v, 16) | (static_cast<uint64_t>(u32(v, 20)) << 32)) == static_cast<uint64_t>(expires), "persisted expiry matches issued access token");
    REQUIRE(std::string(v.begin() + 24, v.begin() + 24 + a) == fixture::jwt(account, expires) &&
        std::string(v.begin() + 24 + a, v.begin() + 24 + a + r) == refresh &&
        std::string(v.begin() + 24 + a + r, v.end()) == account,
        "actual persisted bytes contain the complete expected access refresh and account tuple");
}
void script_login(std::string tokens) {
    fixture::reply(Grant::usercode, 200, fixture::usercode_response());
    fixture::reply(Grant::poll, 200, fixture::approval_response());
    fixture::reply(Grant::authorization, 200, std::move(tokens));
}
void login(const std::string &account = account_a) {
    script_login(fixture::token_response(account, fixture::wall() + 3600));
    REQUIRE(codex_auth_begin(error), "official device-code login starts through public API");
    idle();
    status("connected", true, false);
    usable(account, fixture::wall() + 3600);
    persisted(account, fixture::wall() + 3600, "synthetic-refresh-A");
}
std::shared_ptr<fixture::Reply> pending() {
    fixture::reply(Grant::usercode, 200, fixture::usercode_response());
    auto r = fixture::reply(Grant::poll, 403, "{}", true);
    REQUIRE(codex_auth_begin(error), "replacement login begins without removing saved account");
    fixture::await([r] { return r->seen.load(); }, "provider receives actual pending authorization poll");
    const auto s = status("awaiting_approval", true, true);
    REQUIRE(!std::strcmp(s.user_code, "TEST-CODE") && s.expires_in == 900, "consumer receives official user code with monotonic bounded lifetime");
    return r;
}
void closed(const std::shared_ptr<fixture::Reply> &r) {
    fixture::await([r] { return r->closed.load(); }, "cancelled or expired HTTP transport is cleaned up");
}
void empty_store() { REQUIRE(fixture::record().empty(), "committed disconnect leaves no persisted credentials"); }
void refresh_failure(const std::string &mode) {
    login();
    const auto saved = fixture::record();
    const auto revision = codex_auth_status().account_revision;
    fixture::wall(fixture::initial_wall + 3590);
    auto response = fixture::reply(Grant::refresh, mode == "refresh-provider" ? 401 : 200,
        fixture::token_response(account_a, fixture::wall() + 3600, "synthetic-refresh-B"));
    if (mode == "refresh-stage") fixture::fault(fixture::Fault::stage);
    if (mode == "refresh-commit") fixture::fault(fixture::Fault::commit);
    if (mode == "refresh-full") fixture::capacity(saved.size() + 128);
    CodexCredentials out;
    REQUIRE(!codex_auth_credentials(out, not_cancelled, error) && out.access_token.empty() && out.account_id.empty(), "failed refresh or persistence never publishes uncommitted provider credentials");
    const auto s = status("error", true, false);
    REQUIRE(s.error[0] && s.account_revision == revision && fixture::record() == saved, "refresh failure reports error while retaining committed credentials and account revision");
    REQUIRE(response->seen.load() && fixture::grants(Grant::refresh) == 1, "provider observes exactly one real refresh grant despite local failure");
    fixture::wall(fixture::initial_wall);
    usable(account_a, fixture::initial_wall + 3600);
    persisted(account_a, fixture::initial_wall + 3600, "synthetic-refresh-A");
    fixture::capacity(24576);
}
void replacement_failure(const std::string &mode) {
    login();
    const auto saved = fixture::record();
    const auto revision = codex_auth_status().account_revision;
    script_login(fixture::token_response(account_b, fixture::initial_wall + 3600, "synthetic-refresh-B"));
    if (mode == "login-stage") fixture::fault(fixture::Fault::stage);
    if (mode == "login-commit") fixture::fault(fixture::Fault::commit);
    if (mode == "login-full") fixture::capacity(saved.size() + 128);
    REQUIRE(codex_auth_begin(error), "replacement starts before storage fault is observed");
    idle();
    const auto s = status("error", true, false);
    REQUIRE(s.error[0] && s.account_revision == revision && fixture::record() == saved, "replacement storage failure preserves old committed account and reports an actionable error");
    usable(account_a, fixture::initial_wall + 3600);
    persisted(account_a, fixture::initial_wall + 3600, "synthetic-refresh-A");
    REQUIRE(fixture::grants(Grant::authorization) == 2, "provider granted replacement but local publication waited for commit");
    fixture::capacity(24576);
}
std::string invalid_tokens(const std::string &mode) {
    const auto token = fixture::jwt(account_a, fixture::wall() + 3600);
    const auto id = fixture::jwt(account_a, fixture::wall() + 3600);
    if (mode == "duplicate") return "{\"access_token\":\"" + token + "\",\"access_token\":\"" + token + "\",\"refresh_token\":\"synthetic-refresh-B\",\"id_token\":\"" + id + "\"}";
    if (mode == "nul") return "{\"access_token\":\"" + token + "\\u0000suffix\",\"refresh_token\":\"synthetic-refresh-B\",\"id_token\":\"" + id + "\"}";
    if (mode == "account") return "{\"access_token\":\"" + fixture::jwt(account_b, fixture::wall() + 3600) + "\",\"refresh_token\":\"synthetic-refresh-B\",\"id_token\":\"" + id + "\"}";
    if (mode == "refresh-account") return fixture::token_response(account_b, fixture::wall() + 3600);
    if (mode == "malformed") return "{\"access_token\":\"unterminated";
    if (mode == "escape") return "{\"access_token\":\"\\uD800\\u";
    if (mode == "number") return "{\"access_token\":1e9999999999999999999999999999999999999999}";
    if (mode == "trailing") return fixture::token_response(account_a, fixture::wall() + 3600) + " garbage";
    if (mode == "deep") return std::string(20, '[') + "0" + std::string(20, ']');
    if (mode == "raw-nul") return fixture::token_response(account_a, fixture::wall() + 3600) + std::string(1, '\0');
    REQUIRE(false, "known malformed response scenario");
    return {};
}
void rejected(const std::string &mode, bool refreshing) {
    login();
    const auto saved = fixture::record();
    const auto revision = codex_auth_status().account_revision;
    if (refreshing) {
        fixture::wall(fixture::initial_wall + 3590);
        fixture::reply(Grant::refresh, 200, invalid_tokens(mode));
        unavailable();
    } else {
        script_login(invalid_tokens(mode));
        REQUIRE(codex_auth_begin(error), "public device-code API accepts a new attempt before invalid response");
        idle();
    }
    const auto s = status("error", true, false);
    REQUIRE(s.error[0] && s.account_revision == revision && fixture::record() == saved, "untrusted malformed duplicate NUL or account-mismatched response cannot alter committed credentials");
    REQUIRE(fixture::grants(refreshing ? Grant::refresh : Grant::authorization) == (refreshing ? 1u : 2u), "rejection is exercised by public API with an actual observed provider grant");
    fixture::wall(fixture::initial_wall);
    usable(account_a, fixture::initial_wall + 3600);
    persisted(account_a, fixture::initial_wall + 3600, "synthetic-refresh-A");
}
void scenario(const std::string &name) {
    if (name == "login") {
        login();
        REQUIRE(codex_auth_status().account_revision != 0, "committed login publishes a nonzero account revision");
        REQUIRE(fixture::grants(Grant::usercode) == 1 && fixture::grants(Grant::poll) == 1 && fixture::grants(Grant::authorization) == 1 && fixture::grants(Grant::refresh) == 0, "successful login follows only the official device-code authorization flow");
    } else if (name == "reload" || name == "reload-refreshed") {
        REQUIRE(codex_auth_init(error), "fresh process loads the production committed NVS record");
        const auto s = status("connected", true, false);
        REQUIRE(s.account_revision != 0, "freshly loaded account has a revision");
        const auto expires = fixture::initial_wall + (name == "reload" ? 3600 : 7190);
        usable(account_a, expires);
        persisted(account_a, expires, name == "reload" ? "synthetic-refresh-A" : "synthetic-refresh-B");
        REQUIRE(fixture::grants(Grant::usercode) + fixture::grants(Grant::poll) + fixture::grants(Grant::authorization) + fixture::grants(Grant::refresh) == 0, "reloaded valid credentials require no provider access");
    } else if (name == "reload-empty") {
        REQUIRE(codex_auth_init(error), "fresh process loads committed disconnect without error");
        status("signed_out", false, false); empty_store(); unavailable();
        REQUIRE(fixture::grants(Grant::usercode) + fixture::grants(Grant::poll) + fixture::grants(Grant::authorization) + fixture::grants(Grant::refresh) == 0, "committed disconnect stays signed out after restart without provider access");
    } else if (name == "cancel-restart") {
        login(); const auto saved = fixture::record(); const auto revision = codex_auth_status().account_revision;
        auto old = pending(); usable(account_a, fixture::initial_wall + 3600);
        REQUIRE(!codex_auth_begin(error), "busy pending login rejects a competing begin");
        codex_auth_cancel(); status("connected", true, false);
        REQUIRE(fixture::record() == saved && codex_auth_status().account_revision == revision, "cancel retains exact committed record and media revision");
        auto next = pending(); closed(old);
        usable(account_a, fixture::initial_wall + 3600);
        codex_auth_cancel(); closed(next);
        status("connected", true, false);
        REQUIRE(fixture::record() == saved && codex_auth_status().account_revision == revision && fixture::grants(Grant::authorization) == 1 && fixture::grants(Grant::poll) == 3, "restart polls a new generation without accepting cancelled grants or replacing old account");
    } else if (name == "expiration") {
        login(); const auto saved = fixture::record(); auto r = pending();
        fixture::wall(fixture::initial_wall + 10000000);
        REQUIRE(codex_auth_status().expires_in == 900, "forward wall-clock jump cannot expire a device code");
        fixture::wall(fixture::initial_wall - 10000000);
        fixture::advance(899000000);
        REQUIRE(codex_auth_status().expires_in == 1 && codex_auth_status().busy, "backward wall-clock jump cannot extend the monotonic device deadline");
        fixture::advance(2000000);
        closed(r); // No status call here: actual watchdog must cancel blocked HTTP.
        const auto s = status("error", true, false);
        REQUIRE(s.user_code[0] == 0 && s.expires_in == 0 && fixture::record() == saved && fixture::grants(Grant::authorization) == 1, "expired pending code is cleared without a token grant or saved-account change");
        fixture::wall(fixture::initial_wall); usable(account_a, fixture::initial_wall + 3600);
    } else if (name == "logout-refresh" || name == "logout-authorization") {
        login(); const auto revision = codex_auth_status().account_revision;
        std::shared_ptr<fixture::Reply> r; std::thread consumer; bool result = true;
        if (name == "logout-refresh") {
            fixture::wall(fixture::initial_wall + 3590);
            r = fixture::reply(Grant::refresh, 200, fixture::token_response(account_a, fixture::wall() + 3600), true, true);
            consumer = std::thread([&] { CodexCredentials out; std::string e; result = codex_auth_credentials(out, not_cancelled, e); REQUIRE(out.access_token.empty() && out.account_id.empty(), "logout clears held refresh result before publication"); });
        } else {
            fixture::reply(Grant::usercode, 200, fixture::usercode_response());
            fixture::reply(Grant::poll, 200, fixture::approval_response());
            r = fixture::reply(Grant::authorization, 200, fixture::token_response(account_b, fixture::wall() + 3600), true, true);
            REQUIRE(codex_auth_begin(error), "replacement authorization begins");
        }
        fixture::await([r] { return r->seen.load(); }, "provider response is held after observing the real grant");
        REQUIRE(codex_auth_disconnect(error), "logout commits while provider response is held");
        fixture::release(r); closed(r);
        if (consumer.joinable()) { consumer.join(); REQUIRE(!result, "held refresh reports failure after logout"); }
        const auto s = status("signed_out", false, false);
        REQUIRE(s.account_revision != revision, "committed logout changes account revision");
        empty_store(); unavailable();
        REQUIRE(fixture::grants(name == "logout-refresh" ? Grant::refresh : Grant::authorization) == (name == "logout-refresh" ? 1u : 2u), "held provider grant cannot resurrect committed disconnect");
    } else if (name == "concurrent-refresh") {
        login(); const auto revision = codex_auth_status().account_revision;
        fixture::wall(fixture::initial_wall + 3590);
        auto r = fixture::reply(Grant::refresh, 200, fixture::token_response(account_a, fixture::wall() + 3600, "synthetic-refresh-B"), true);
        bool ok1 = false, ok2 = false; std::atomic<bool> started{false};
        auto request = [&](bool &ok) { CodexCredentials out; std::string e; ok = codex_auth_credentials(out, not_cancelled, e); REQUIRE(ok && out.access_token == fixture::jwt(account_a, fixture::wall() + 3600) && out.account_id == account_a, "concurrent consumer receives the committed refreshed tuple"); };
        std::thread first([&] { request(ok1); });
        fixture::await([r] { return r->seen.load(); }, "first refresh reaches held provider response");
        const auto waits = fixture::consumer_waits();
        std::thread second([&] { started.store(true); request(ok2); });
        fixture::await([&] { return started.load(); }, "second refresh consumer starts while first grant is held");
        fixture::await([&] { return fixture::consumer_waits() > waits; }, "second public consumer is waiting behind the held refresh");
        fixture::release(r); first.join(); second.join();
        REQUIRE(ok1 && ok2 && fixture::grants(Grant::refresh) == 1 && fixture::maximum_active_refresh() == 1, "concurrent refresh consumers share one serialized provider grant");
        REQUIRE(codex_auth_status().account_revision == revision, "token rotation does not invalidate account media revision");
        status("connected", true, false); persisted(account_a, fixture::wall() + 3600, "synthetic-refresh-B");
    } else if (name == "refresh-deadline" || name == "refresh-external-cancel") {
        login(); const auto saved = fixture::record(); fixture::wall(fixture::initial_wall + 3590);
        auto r = fixture::reply(Grant::refresh, 200, fixture::token_response(account_a, fixture::wall() + 3600), true, true);
        std::atomic<bool> cancel{false}; bool ok = true;
        std::thread consumer([&] { CodexCredentials out; std::string e; ok = codex_auth_credentials(out, cancel, e); REQUIRE(!ok && out.access_token.empty() && !e.empty(), "bounded or cancelled transport fails without publishing partial credentials"); });
        fixture::await([r] { return r->seen.load(); }, "actual refresh enters held bounded read loop");
        if (name == "refresh-deadline") fixture::advance(45000001); else cancel.store(true);
        closed(r); consumer.join();
        REQUIRE(!ok && fixture::record() == saved && fixture::grants(Grant::refresh) == 1, "transport deadline or watchdog external cancellation retains committed record");
        status(name == "refresh-deadline" ? "error" : "connected", true, false);
        fixture::wall(fixture::initial_wall); usable(account_a, fixture::initial_wall + 3600);
    } else if (name == "corrupt") {
        fixture::corrupt_record(); const auto saved = fixture::record();
        REQUIRE(!codex_auth_init(error), "corrupt record initialization truthfully fails");
        status("error", false, false); unavailable();
        REQUIRE(!codex_auth_begin(error), "corrupt saved account requires explicit recovery instead of silently overwriting");
        fixture::fault(fixture::Fault::commit);
        REQUIRE(!codex_auth_disconnect(error) && fixture::record() == saved, "failed disconnect commit does not pretend corrupt storage was removed");
        REQUIRE(codex_auth_disconnect(error), "public disconnect recovers corrupt record through committed erase");
        empty_store(); status("signed_out", false, false); unavailable();
        login();
    } else if (name == "disconnect-stage" || name == "disconnect-commit") {
        login(); const auto saved = fixture::record(); const auto revision = codex_auth_status().account_revision;
        fixture::fault(name == "disconnect-stage" ? fixture::Fault::stage : fixture::Fault::commit);
        REQUIRE(!codex_auth_disconnect(error), "failed erase or commit cannot pretend to sign out");
        const auto s = status("error", true, false);
        REQUIRE(s.error[0] && s.account_revision == revision && fixture::record() == saved, "failed logout retains committed credentials and revision");
        usable(account_a, fixture::initial_wall + 3600);
        REQUIRE(codex_auth_disconnect(error), "retry signs out after committed erase");
        empty_store(); status("signed_out", false, false); unavailable();
    } else if (name.compare(0, 6, "login-") == 0 && (name == "login-stage" || name == "login-commit" || name == "login-full")) {
        replacement_failure(name);
    } else if (name == "refresh-stage" || name == "refresh-commit" || name == "refresh-full" || name == "refresh-provider") {
        refresh_failure(name);
    } else if (name.compare(0, 7, "reject-") == 0) {
        rejected(name.substr(7), false);
    } else if (name.compare(0, 15, "refresh-reject-") == 0) {
        rejected(name.substr(15), true);
    } else {
        REQUIRE(false, "known isolated behavioral scenario");
    }
    fixture::drained();
}
}
int main(int argc, char **argv) {
    REQUIRE(argc == 3, "scenario and private temporary NVS path supplied");
    fixture::initialize(argv[2]);
    scenario(argv[1]);
    fixture::shutdown();
    std::printf("PASS %s: public state, credentials, persisted record and HTTP grants\n", argv[1]);
}
