#pragma once

#include "agent_runtime.h"
#include <string>
#include <vector>

namespace unravel {

enum class ToolState { requested, running, succeeded, failed, unknown, cancelled, partial };
enum class ToolOrigin { text, native_audio, codex_live, screen };
// Borrowed identity and cancellation state, valid for the synchronous operation.
// deadline_us uses monotonic_time_us(); zero means no caller deadline.
struct ToolCallContext {
    const char *conversation_id = "";
    const char *provider_session = "";
    const char *run_id = "";
    const char *call_id = "";
    ToolOrigin origin = ToolOrigin::text;
    uint64_t generation = 0;
    uint32_t attempt = 1;
    uint64_t deadline_us = 0;
    bool (*cancelled)(uint64_t generation, void *context) = nullptr;
    void *cancellation_context = nullptr;
    // Set only by Agent from the synchronous policy's committed-start ACK.
    // execute_tool always clears any caller-supplied value before admission.
    bool durable_started = false;
    bool is_cancelled() const;
    bool is_expired() const;
};
uint64_t monotonic_time_us();
const char *tool_state_name(ToolState state);
struct ToolResult {
    ToolState state = ToolState::failed;
    std::string output;
    std::string error;
    bool durable_acknowledged = false;
};
struct RunResult {
    bool ok = false;
    std::string output;
    std::string error;
};
struct Tool {
    std::string name;
    std::string description;
    std::string parameters_json;
    bool (*execute)(const char *arguments_json, std::string &output,
                    const ToolCallContext &call, void *context) = nullptr;
    void *context = nullptr;
    bool requires_durable = false;
};
using ToolObserver = void (*)(const char *name, const char *arguments_json,
    const ToolCallContext &call, const ToolResult &result, void *context);
enum class ToolDecision { execute, reuse, refuse };
// Policy is a synchronous transaction seam, never an observer. reuse/refuse
// supply the recorded outcome without dispatch; false after-ack means unknown.
using BeforeTool = ToolDecision (*)(const char *, const char *, const ToolCallContext &, ToolResult &, void *);
using AfterTool = bool (*)(const char *, const char *, const ToolCallContext &, const ToolResult &, void *);
using SessionCheckpoint = bool (*)(const char *session_json, size_t length, uint64_t message_offset, void *);

// One conversation per instance. Calls are synchronous, serial, and worker-only.
// All callback contexts remain valid until the operation returns. Not thread-safe.
class Agent {
public:
    explicit Agent(unravel_model_callback_t model, void *context = nullptr);
    Agent(const Agent &) = delete;
    Agent &operator=(const Agent &) = delete;
    Agent(Agent &&) = default;
    Agent &operator=(Agent &&) = default;
    RunResult run(const char *prompt, const char *model,
                  const char *system_prompt = "You are an assistant running on an ESP32 device display.");
    void reset();
    const std::string &session() const { return session_; }
    uint64_t message_offset() const { return message_offset_; }
    bool load_session(const char *json, uint64_t offset, std::string &error);
    bool checkpoint_session(std::string &json, uint64_t &offset, std::string &error);
    void set_checkpoint(SessionCheckpoint callback, void *context = nullptr);
    void set_call_context(const ToolCallContext &context);
    bool add_tool(Tool tool);
    bool native_tools_json(std::string &json) const;
    ToolResult execute_tool(const char *name, const char *arguments_json, const ToolCallContext &call);
    void set_tool_observer(ToolObserver observer, void *context = nullptr);
    void set_tool_policy(BeforeTool before, AfterTool after, void *context = nullptr);
private:
    unravel_model_callback_t model_;
    void *context_;
    std::string session_;
    uint64_t message_offset_ = 0;
    ToolCallContext call_context_;
    static int32_t model_bridge(const uint8_t *, size_t, uint8_t *, size_t, void *);
    static int32_t tool_bridge(const uint8_t *, size_t, uint8_t *, size_t, void *);
    static int32_t checkpoint_bridge(const uint8_t *, size_t, uint64_t, void *);
    static uint64_t clock_bridge(void *);
    bool validate_session(const char *, uint64_t, std::string &, std::string &);
    std::vector<Tool> tools_;
    ToolObserver observer_ = nullptr;
    void *observer_context_ = nullptr;
    BeforeTool before_ = nullptr;
    AfterTool after_ = nullptr;
    void *policy_context_ = nullptr;
    SessionCheckpoint checkpoint_ = nullptr;
    void *checkpoint_context_ = nullptr;
};
} // namespace unravel
