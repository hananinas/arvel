#pragma once
#include "desk_buddy.h"
#include <atomic>
#include <cstdint>
#include <string>

namespace unravel { struct ToolCallContext; }

/* Firmware I/O seam: production uses NVS/verified HTTPS; host scenarios use
 * isolated storage, clock and HTTP fixtures. No transport is owned by the UI. */
struct DeskPlatform {
    int64_t (*monotonic_us)();
    int64_t (*utc_now)();
    void *(*allocate)(size_t);
    int (*load_notes)(uint8_t *data, size_t *size); // 1 loaded, 0 absent, -1 error
    bool (*save_notes)(const uint8_t *data, size_t size);
    void (*search_configuration)(std::string &provider, bool &enabled, std::string &key);
    bool (*http)(const char *url, const char *api_key, const char *body,
                 size_t response_limit, uint32_t timeout_ms, std::string &response,
                 int &status, std::string &error, const unravel::ToolCallContext *call);
};

/* Initialize before callers; initialization does not erase invalid saved data.
 * Backend state is bounded and allocated in PSRAM on the device. */
bool desk_backend_initialize(const DeskPlatform &platform, std::string &error);
/* Safe from the existing ESP timer task: try-lock, no allocations or I/O. */
void desk_backend_tick();
/* Synchronous implementation for the registered native tools and queued native
 * controls. Flash/HTTPS operations require the existing/internal worker stack,
 * never a small LVGL stack or a PSRAM stack with caches disabled. */
/* The optional context is borrowed only until return. Remote calls enforce the
 * tighter of its absolute deadline and an eight-second total operation budget. */
bool desk_backend_execute(const char *tool, const cJSON *arguments, cJSON **reply,
                          const unravel::ToolCallContext *call = nullptr);
bool desk_backend_needs_internal_stack(const char *tool, const cJSON *arguments);
bool desk_backend_known_tool(const char *tool);

/* Production adapter is initialized by ai_chat_init; no permanent new worker.
 * Native queued I/O uses one fallibly created, joined 12 KiB internal pthread.
 * Rust tool callbacks already have that stack and execute directly. */
bool desk_platform_initialize(void (*configuration)(std::string &, bool &, std::string &),
                              const std::atomic<bool> &cancelled, std::string &error);
bool desk_platform_execute(const char *tool, const cJSON *arguments, cJSON **reply,
                           const unravel::ToolCallContext *call = nullptr);
