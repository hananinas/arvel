#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace fixture {
[[noreturn]] void fail(const char *description, int line);
#define REQUIRE(condition, description) do { if (!(condition)) fixture::fail(description, __LINE__); } while (0)
constexpr int64_t initial_wall = 1800000000;
void initialize(const char *path);
void shutdown();
void wall(int64_t seconds);
int64_t wall();
void advance(int64_t microseconds);
void await(const std::function<bool()> &, const char *description);
uint64_t consumer_waits();
enum class Fault { none, stage, commit };
void fault(Fault);
void capacity(size_t bytes);
std::vector<unsigned char> record();
void corrupt_record();
enum class Grant { usercode, poll, authorization, refresh };
struct Reply {
    Grant grant;
    int status;
    std::string body;
    std::atomic<bool> held{false};
    std::atomic<bool> seen{false};
    // Hold at headers after a complete request body, or inside the bounded
    // transport's read loop; both must observe deadline and cancellation.
    bool hold_read = false;
    std::atomic<bool> closed{false};
};
std::shared_ptr<Reply> reply(Grant, int, std::string, bool held = false, bool hold_read = false);
void release(const std::shared_ptr<Reply> &);
size_t grants(Grant);
unsigned maximum_active_refresh();
void drained();
std::string jwt(const std::string &account, int64_t expiry);
std::string token_response(const std::string &account, int64_t expiry, const std::string &refresh = "synthetic-refresh-A");
std::string usercode_response();
std::string approval_response();
}
