#include "agent_runtime.hpp"
#include <cJSON.h>
#include <cstdlib>
#include <memory>
#include <new>
#include <cstring>
#include <utility>
#include <algorithm>
#include <chrono>

namespace unravel {
namespace {
constexpr size_t response_capacity = 128 * 1024;
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool save_session(cJSON *value, std::string &session)
{
    if (!cJSON_IsObject(value)) return false;
    char *encoded = cJSON_PrintUnformatted(value);
    if (!encoded) return false;
    session.assign(encoded);
    cJSON_free(encoded);
    return true;
}
}

uint64_t monotonic_time_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool ToolCallContext::is_cancelled() const
{
    return cancelled && cancelled(generation, cancellation_context);
}
bool ToolCallContext::is_expired() const
{
    return deadline_us && monotonic_time_us() >= deadline_us;
}
const char *tool_state_name(ToolState state)
{
    switch (state) {
    case ToolState::requested: return "requested";
    case ToolState::running: return "running";
    case ToolState::succeeded: return "succeeded";
    case ToolState::failed: return "failed";
    case ToolState::unknown: return "unknown";
    case ToolState::cancelled: return "cancelled";
    case ToolState::partial: return "partial";
    }
    return "unknown";
}
void Agent::set_call_context(const ToolCallContext &context) { call_context_ = context; }
void Agent::set_checkpoint(SessionCheckpoint callback, void *context)
{
    checkpoint_ = callback; checkpoint_context_ = context;
}
void Agent::set_tool_policy(BeforeTool before, AfterTool after, void *context)
{
    before_ = before; after_ = after; policy_context_ = context;
}
int32_t Agent::checkpoint_bridge(const uint8_t *json, size_t length, uint64_t offset, void *context)
{
    Agent *agent = static_cast<Agent *>(context);
    return agent->checkpoint_ && agent->checkpoint_(reinterpret_cast<const char *>(json),
        length, offset, agent->checkpoint_context_) ? 1 : 0;
}

Agent::Agent(unravel_model_callback_t model, void *context)
    : model_(model), context_(context)
{
}

void Agent::reset()
{
    std::string{}.swap(session_);
    message_offset_ = 0;
}

bool Agent::add_tool(Tool tool)
{
    if (tools_.size() >= UNRAVEL_MAX_TOOLS || tool.name.empty() ||
        tool.name.size() > 64 || tool.description.size() > 2048 ||
        tool.parameters_json.size() > 8192 || !tool.execute ||
        tool.name.find('\0') != std::string::npos || tool.description.find('\0') != std::string::npos ||
        tool.parameters_json.find('\0') != std::string::npos) return false;
    for (const Tool &registered : tools_) {
        if (registered.name == tool.name) return false;
    }
    Json schema(cJSON_ParseWithOpts(tool.parameters_json.c_str(), nullptr, true), cJSON_Delete);
    if (!cJSON_IsObject(schema.get())) return false;
    tools_.push_back(std::move(tool));
    return true;
}

void Agent::set_tool_observer(ToolObserver observer, void *context)
{
    observer_ = observer;
    observer_context_ = context;
}

int32_t Agent::model_bridge(const uint8_t *request, size_t length,
    uint8_t *response, size_t capacity, void *context)
{
    Agent *agent = static_cast<Agent *>(context);
    if (agent->call_context_.is_cancelled() || agent->call_context_.is_expired()) return -1;
    return agent->model_(request, length, response, capacity, agent->context_);
}
uint64_t Agent::clock_bridge(void *) { return monotonic_time_us(); }

namespace {
bool unsafe_json(const char *json, size_t length)
{
    if (std::memchr(json, 0, length)) return true;
    for (size_t i = 0; i < length; ++i) if (json[i] == '\\') {
        if (i + 5 < length && !std::memcmp(json + i + 1, "u0000", 5)) return true;
        ++i;
    }
    return false;
}
// Canonical key ordering gives policy hooks stable argument bytes independent
// of provider object order. Duplicate keys are rejected, never interpreted twice.
bool canonicalize(cJSON *value)
{
    if (cJSON_IsObject(value)) {
        std::vector<cJSON *> children;
        for (cJSON *child = value->child; child; child = child->next) children.push_back(child);
        std::sort(children.begin(), children.end(), [](const cJSON *a, const cJSON *b) {
            return std::strcmp(a->string, b->string) < 0;
        });
        for (size_t i = 0; i < children.size(); ++i) {
            if ((i && !std::strcmp(children[i-1]->string, children[i]->string)) ||
                !canonicalize(children[i])) return false;
        }
        value->child = children.empty() ? nullptr : children.front();
        for (size_t i = 0; i < children.size(); ++i) {
            children[i]->prev = i ? children[i-1] : children.back();
            children[i]->next = i + 1 < children.size() ? children[i+1] : nullptr;
        }
    } else if (cJSON_IsArray(value)) {
        for (cJSON *child = value->child; child; child = child->next)
            if (!canonicalize(child)) return false;
    }
    return true;
}
bool domain_failure(const std::string &output)
{
    Json json(cJSON_Parse(output.c_str()), cJSON_Delete);
    if (!cJSON_IsObject(json.get())) return false;
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(json.get(), "ok");
    cJSON *success = cJSON_GetObjectItemCaseSensitive(json.get(), "success");
    cJSON *error = cJSON_GetObjectItemCaseSensitive(json.get(), "error");
    return cJSON_IsFalse(ok) || cJSON_IsFalse(success) ||
        (error && !cJSON_IsNull(error) && !cJSON_IsFalse(error));
}
}

ToolResult Agent::execute_tool(const char *name, const char *arguments, const ToolCallContext &incoming)
{
    ToolResult result;
    ToolCallContext call = incoming;
    call.durable_started = false;
    if (!name || !arguments || !call.call_id || !*call.call_id ||
        !call.run_id || !*call.run_id || std::strlen(call.call_id) > 128 ||
        std::strlen(call.run_id) > 128 || std::strlen(arguments) > 8192 ||
        unsafe_json(arguments, std::strlen(arguments))) {
        result.error = "Invalid complete tool call"; return result;
    }
    const Tool *registered = nullptr;
    for (const Tool &tool : tools_) if (tool.name == name) { registered = &tool; break; }
    Json input(cJSON_ParseWithOpts(arguments, nullptr, true), cJSON_Delete);
    if (!registered || !cJSON_IsObject(input.get()) || !canonicalize(input.get())) {
        result.error = "Unknown tool or invalid arguments"; return result;
    }
    char *encoded = cJSON_PrintUnformatted(input.get());
    if (!encoded) { result.error = "Cannot encode arguments"; return result; }
    std::unique_ptr<char, decltype(&cJSON_free)> canonical(encoded, cJSON_free);
    if (call.is_cancelled()) { result.state = ToolState::cancelled; result.error = "Call cancelled or stale"; return result; }
    if (call.is_expired()) { result.error = "Deadline expired before dispatch"; return result; }
    if (registered->requires_durable && (!before_ || !after_)) {
        result.error = "This tool requires durable before/after policy acknowledgment"; return result;
    }
    if (before_) {
        const ToolDecision decision = before_(name, encoded, call, result, policy_context_);
        if (decision != ToolDecision::execute) {
            result.reused = decision == ToolDecision::reuse;
            if (decision == ToolDecision::refuse && result.state == ToolState::succeeded) result.state = ToolState::failed;
            if (observer_) observer_(name, encoded, call, result, observer_context_);
            return result;
        }
    }
    if (registered->requires_durable && !result.durable_acknowledged) {
        result.state = ToolState::failed;
        result.error = "Durable started transaction was not acknowledged";
        if (observer_) observer_(name, encoded, call, result, observer_context_);
        return result;
    }
    call.durable_started = before_ && result.durable_acknowledged;
    result.output.clear();
    result.error.clear();
    // Recheck after the synchronous acknowledgment, immediately before effects.
    if (call.is_cancelled() || call.is_expired()) {
        result.state = ToolState::cancelled; result.error = "Call stopped before dispatch";
    } else {
        result.state = ToolState::running;
        if (observer_) observer_(name, encoded, call, result, observer_context_);
        const bool ok = registered->execute(encoded, result.output, call, registered->context);
        if (!ok) result.error = "Tool handler reported failure";
        result.state = ok && !domain_failure(result.output) ? ToolState::succeeded : ToolState::failed;
        Json outcome(cJSON_Parse(result.output.c_str()), cJSON_Delete);
        cJSON *state = cJSON_GetObjectItemCaseSensitive(outcome.get(), "state");
        if (cJSON_IsString(state)) {
            if (!std::strcmp(state->valuestring, "unknown")) result.state = ToolState::unknown;
            else if (!std::strcmp(state->valuestring, "partial")) result.state = ToolState::partial;
            else if (!std::strcmp(state->valuestring, "cancelled")) result.state = ToolState::cancelled;
        }
        if (call.is_cancelled() || call.is_expired()) {
            result.state = ToolState::unknown; result.error = "Call interrupted after dispatch; effect unknown";
        }
        size_t wire_bytes = 0;
        for (unsigned char byte : result.output) wire_bytes += byte < 0x20 ? 6 : (byte == '"' || byte == '\\' ? 2 : 1);
        if (result.output.size() > 8192 || std::memchr(result.output.data(), 0, result.output.size()) ||
            wire_bytes > UNRAVEL_TOOL_MAX_RESPONSE_BYTES - 1024) {
            result.output.clear(); result.state = ToolState::unknown; result.error = "Tool result exceeds bounded text/wire contract";
        }
    }
    if (after_ && !after_(name, encoded, call, result, policy_context_)) {
        result.state = ToolState::unknown; result.error = "Terminal outcome could not be durably acknowledged";
        result.durable_acknowledged = false;
    }
    if (observer_) observer_(name, encoded, call, result, observer_context_);
    return result;
}

bool Agent::native_tools_json(std::string &output) const
{
    Json array(cJSON_CreateArray(), cJSON_Delete);
    if (!array) return false;
    for (const Tool &tool : tools_) {
        cJSON *item = cJSON_CreateObject();
        cJSON *schema = cJSON_Parse(tool.parameters_json.c_str());
        if (!item || !schema) { cJSON_Delete(item); cJSON_Delete(schema); return false; }
        cJSON_AddStringToObject(item, "type", "function");
        cJSON_AddStringToObject(item, "name", tool.name.c_str());
        cJSON_AddStringToObject(item, "description", tool.description.c_str());
        cJSON_AddItemToObject(item, "parameters", schema);
        cJSON_AddItemToArray(array.get(), item);
    }
    char *json = cJSON_PrintUnformatted(array.get());
    if (!json) return false;
    const bool fits = std::strlen(json) <= 24 * 1024;
    if (fits) output.assign(json);
    cJSON_free(json);
    return fits;
}

int32_t Agent::tool_bridge(const uint8_t *request, size_t length,
    uint8_t *response, size_t capacity, void *context)
{
    Agent *agent = static_cast<Agent *>(context);
    if (unsafe_json(reinterpret_cast<const char *>(request), length)) return -1;
    Json input(cJSON_ParseWithLength(reinterpret_cast<const char *>(request), length), cJSON_Delete);
    cJSON *name = cJSON_GetObjectItemCaseSensitive(input.get(), "name");
    cJSON *arguments = cJSON_GetObjectItemCaseSensitive(input.get(), "arguments");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(input.get(), "call_id");
    cJSON *run_id = cJSON_GetObjectItemCaseSensitive(input.get(), "run_id");
    cJSON *attempt = cJSON_GetObjectItemCaseSensitive(input.get(), "attempt");
    cJSON *absolute_deadline = cJSON_GetObjectItemCaseSensitive(input.get(), "deadline_us");
    if (!cJSON_IsString(name) || !cJSON_IsObject(arguments) || !cJSON_IsString(id)) return -1;
    ToolCallContext call = agent->call_context_;
    if (!cJSON_IsString(run_id)) return -1;
    call.run_id = run_id->valuestring;
    call.call_id = id->valuestring;
    call.attempt = cJSON_IsNumber(attempt) ? static_cast<uint32_t>(attempt->valuedouble) : 1;
    if (cJSON_IsNumber(absolute_deadline) && absolute_deadline->valuedouble > 0) {
        const uint64_t deadline = static_cast<uint64_t>(absolute_deadline->valuedouble);
        if (!call.deadline_us || deadline < call.deadline_us) call.deadline_us = deadline;
    }
    char *encoded = cJSON_PrintUnformatted(arguments);
    if (!encoded) return -1;
    ToolResult result = agent->execute_tool(name->valuestring, encoded, call);
    cJSON_free(encoded);
    Json reply(cJSON_CreateObject(), cJSON_Delete);
    if (!reply) return -1;
    std::string content;
    const char *projected = result.output.empty() ? result.error.c_str() : result.output.c_str();
    if (!result.error.empty() && !result.output.empty()) {
        Json evidence(cJSON_CreateObject(), cJSON_Delete);
        if (!evidence) return -1;
        cJSON_AddStringToObject(evidence.get(), "state", tool_state_name(result.state));
        cJSON_AddStringToObject(evidence.get(), "output", result.output.c_str());
        cJSON_AddStringToObject(evidence.get(), "error", result.error.c_str());
        char *text = cJSON_PrintUnformatted(evidence.get());
        if (!text) return -1;
        content.assign(text); cJSON_free(text);
        projected = content.c_str();
    }
    cJSON_AddStringToObject(reply.get(), "content", projected);
    cJSON_AddStringToObject(reply.get(), "state", tool_state_name(result.state));
    encoded = cJSON_PrintUnformatted(reply.get());
    if (!encoded) return -1;
    const size_t count = std::strlen(encoded);
    if (count <= capacity) std::memcpy(response, encoded, count);
    cJSON_free(encoded);
    return count <= capacity ? static_cast<int32_t>(count) : -1;
}

RunResult Agent::run(const char *prompt, const char *model, const char *system_prompt)
{
    RunResult result;
    if (!model_ || !prompt || !model || !system_prompt) {
        result.error = "Invalid agent configuration or insufficient memory";
        return result;
    }
    std::unique_ptr<uint8_t[]> response(new (std::nothrow) uint8_t[response_capacity]);
    if (!response) {
        result.error = "Invalid agent configuration or insufficient memory";
        return result;
    }
    Json request(cJSON_CreateObject(), cJSON_Delete);
    cJSON *params = request ? cJSON_AddObjectToObject(request.get(), "params") : nullptr;
    if (!params || !cJSON_AddStringToObject(request.get(), "jsonrpc", "2.0") ||
        !cJSON_AddNumberToObject(request.get(), "id", 1) ||
        !cJSON_AddStringToObject(request.get(), "method", "agent.run") ||
        !cJSON_AddStringToObject(params, "prompt", prompt) ||
        !cJSON_AddStringToObject(params, "model", model) ||
        !cJSON_AddStringToObject(params, "system_prompt", system_prompt)) {
        result.error = "Unable to allocate agent request";
        return result;
    }
    cJSON_AddNumberToObject(params, "message_offset", static_cast<double>(message_offset_));
    cJSON *execution = cJSON_AddObjectToObject(params, "execution");
    if (!execution) { result.error = "Unable to encode call context"; return result; }
    cJSON_AddStringToObject(execution, "conversation_id", call_context_.conversation_id);
    cJSON_AddStringToObject(execution, "provider_session", call_context_.provider_session);
    cJSON_AddStringToObject(execution, "run_id", call_context_.run_id);
    cJSON_AddNumberToObject(execution, "generation", static_cast<double>(call_context_.generation));
    cJSON_AddStringToObject(execution, "origin", call_context_.origin == ToolOrigin::native_audio ? "native_audio" :
        call_context_.origin == ToolOrigin::codex_live ? "codex_live" : call_context_.origin == ToolOrigin::screen ? "screen" : "text");
    if (!session_.empty()) {
        cJSON *session = cJSON_Parse(session_.c_str());
        if (!session || !cJSON_AddItemToObject(params, "session", session)) {
            cJSON_Delete(session);
            result.error = "Unable to restore agent session";
            return result;
        }
    }
    if (!tools_.empty()) {
        cJSON *tools = cJSON_AddArrayToObject(params, "tools");
        if (!tools) {
            result.error = "Unable to allocate tool definitions";
            return result;
        }
        for (const Tool &tool : tools_) {
            cJSON *definition = cJSON_CreateObject();
            cJSON *schema = cJSON_Parse(tool.parameters_json.c_str());
            if (!definition || !schema ||
                !cJSON_AddStringToObject(definition, "name", tool.name.c_str()) ||
                !cJSON_AddStringToObject(definition, "description", tool.description.c_str())) {
                cJSON_Delete(definition);
                cJSON_Delete(schema);
                result.error = "Unable to encode tool definition";
                return result;
            }
            cJSON_AddItemToObject(definition, "parameters", schema);
            cJSON_AddItemToArray(tools, definition);
        }
        cJSON *read_only = cJSON_AddArrayToObject(params, "read_only_tools");
        if (!read_only) { result.error = "Unable to allocate tool definitions"; return result; }
        for (const Tool &tool : tools_)
            if (tool.read_only) cJSON_AddItemToArray(read_only, cJSON_CreateString(tool.name.c_str()));
        cJSON *parallel = cJSON_AddArrayToObject(params, "parallel_tools");
        if (!parallel) { result.error = "Unable to allocate tool definitions"; return result; }
        for (const Tool &tool : tools_)
            if (tool.parallel_safe) cJSON_AddItemToArray(parallel, cJSON_CreateString(tool.name.c_str()));
    }
    char *encoded = cJSON_PrintUnformatted(request.get());
    if (!encoded) {
        result.error = "Unable to encode agent request";
        return result;
    }
    request.reset();
    const int32_t count = unravel_agent_rpc(reinterpret_cast<const uint8_t *>(encoded),
        std::char_traits<char>::length(encoded), response.get(), response_capacity,
        model_bridge, tool_bridge, checkpoint_ ? checkpoint_bridge : nullptr, clock_bridge, this);
    cJSON_free(encoded);
    if (count < 0 || static_cast<size_t>(count) > response_capacity) {
        result.error = "Rust agent RPC transport error " + std::to_string(count);
        return result;
    }
    Json reply(cJSON_ParseWithLength(reinterpret_cast<const char *>(response.get()), count), cJSON_Delete);
    response.reset();
    if (!reply) {
        result.error = "Invalid Rust agent RPC response";
        return result;
    }
    cJSON *error = cJSON_GetObjectItemCaseSensitive(reply.get(), "error");
    if (error) {
        cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
        cJSON *data = cJSON_GetObjectItemCaseSensitive(error, "data");
        if (data) save_session(cJSON_GetObjectItemCaseSensitive(data, "session"), session_);
        cJSON *offset = cJSON_GetObjectItemCaseSensitive(data, "message_offset");
        if (cJSON_IsNumber(offset)) message_offset_ = static_cast<uint64_t>(offset->valuedouble);
        result.error = cJSON_IsString(message) ? message->valuestring : "Agent run failed";
        return result;
    }
    cJSON *value = cJSON_GetObjectItemCaseSensitive(reply.get(), "result");
    cJSON *offset = cJSON_GetObjectItemCaseSensitive(value, "message_offset");
    if (cJSON_IsNumber(offset)) message_offset_ = static_cast<uint64_t>(offset->valuedouble);
    cJSON *output = cJSON_GetObjectItemCaseSensitive(value, "output");
    if (!cJSON_IsString(output) || !save_session(cJSON_GetObjectItemCaseSensitive(value, "session"), session_)) {
        result.error = "Incomplete Rust agent result";
        return result;
    }
    result.output = output->valuestring;
    result.ok = true;
    return result;
}

bool Agent::validate_session(const char *json, uint64_t offset, std::string &canonical, std::string &error)
{
    if (!json || std::strlen(json) > UNRAVEL_SESSION_MAX_BYTES || unsafe_json(json, std::strlen(json)) ||
        offset > 9007199254740991ULL) { error = "Invalid bounded session"; return false; }
    Json request(cJSON_CreateObject(), cJSON_Delete);
    cJSON *session = cJSON_ParseWithOpts(json, nullptr, true);
    if (!request || !session) { cJSON_Delete(session); error = "Invalid session JSON"; return false; }
    cJSON_AddStringToObject(request.get(), "jsonrpc", "2.0");
    cJSON_AddNumberToObject(request.get(), "id", 1);
    cJSON_AddStringToObject(request.get(), "method", "agent.session");
    cJSON *params = cJSON_AddObjectToObject(request.get(), "params");
    if (!params) { cJSON_Delete(session); error = "Allocation failure"; return false; }
    cJSON_AddItemToObject(params, "session", session);
    char *encoded = cJSON_PrintUnformatted(request.get());
    std::unique_ptr<uint8_t[]> response(new (std::nothrow) uint8_t[response_capacity]);
    if (!encoded || !response) { cJSON_free(encoded); error = "Allocation failure"; return false; }
    const int32_t count = unravel_agent_rpc(reinterpret_cast<const uint8_t *>(encoded), std::strlen(encoded),
        response.get(), response_capacity, nullptr, nullptr, nullptr, nullptr, nullptr);
    cJSON_free(encoded);
    if (count <= 0) { error = "Session validation transport failure"; return false; }
    Json reply(cJSON_ParseWithLength(reinterpret_cast<const char *>(response.get()), count), cJSON_Delete);
    cJSON *value = cJSON_GetObjectItemCaseSensitive(reply.get(), "result");
    if (!value || !save_session(cJSON_GetObjectItemCaseSensitive(value, "session"), canonical)) {
        error = "Session is invalid, oversized or has malformed tool groups"; return false;
    }
    return true;
}
bool Agent::load_session(const char *json, uint64_t offset, std::string &error)
{
    std::string canonical;
    if (!validate_session(json, offset, canonical, error)) return false;
    session_.swap(canonical); message_offset_ = offset; return true;
}
bool Agent::checkpoint_session(std::string &json, uint64_t &offset, std::string &error)
{
    if (session_.empty()) { error = "No session to checkpoint"; return false; }
    if (!validate_session(session_.c_str(), message_offset_, json, error)) return false;
    if (checkpoint_ && !checkpoint_(json.data(), json.size(), message_offset_, checkpoint_context_)) {
        error = "Archive acknowledgment failed"; return false;
    }
    offset = message_offset_; return true;
}

} // namespace unravel
