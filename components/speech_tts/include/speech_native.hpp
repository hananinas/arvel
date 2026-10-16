#pragma once

#include "speech_live.hpp"
#include <atomic>
#include <cstdint>
#include <string>

// Includes escaping of bounded transcript/tool strings; excludes trailing NUL.
inline constexpr size_t speech_native_max_event_bytes = 65536;

// Queue-only callback; bytes remain owned by the socket task for this invocation.
// Start schedules verified connection/configuration; only a ready event means the
// server acknowledged the requested PCM formats and tool definitions.
bool speech_native_start(const char *endpoint, const char *api_key,
                         const char *instructions, const char *tools_json,
                         speech_live_event_callback_t callback, void *context,
                         std::string &error);
bool speech_native_active();
bool speech_native_muted();
bool speech_native_set_muted(bool muted, std::string &error);
// Nonblocking cancellation. wait_stopped joins all media/socket ownership before
// the caller may release its callback context. Never call wait from the callback.
void speech_native_stop();
bool speech_native_wait_stopped(uint32_t timeout_ms);
bool speech_native_tool_result(const char *provider_session, const char *call_id,
                               const char *result_json, std::string &error);
// Authenticated, verified same-origin HTTPS health only: no capture or inference.
// Success is server readiness, not proof of an exercised speech/tool deployment.
bool speech_native_check_readiness(const char *endpoint, const char *api_key,
                                   std::atomic<bool> &cancel, std::string &error);
