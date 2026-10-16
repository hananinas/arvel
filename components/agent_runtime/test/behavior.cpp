#include "agent_runtime.hpp"
#include "cJSON.h"
#include <atomic>
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <map>

static std::atomic<bool> fail_next_array{false};

// Fail one fallible array allocation, independent of workspace size.
void *operator new[](std::size_t bytes, const std::nothrow_t &) noexcept
{
    if(fail_next_array.exchange(false)) return nullptr;
    try { return ::operator new[](bytes); }
    catch(...) { return nullptr; }
}

struct Provider {
    std::vector<std::vector<std::string>> user_turns;
};

static int32_t model(const uint8_t *request, size_t length, uint8_t *response,
                     size_t capacity, void *opaque)
{
    auto &provider = *static_cast<Provider *>(opaque);
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> body(
        cJSON_ParseWithLength(reinterpret_cast<const char *>(request), length), cJSON_Delete);
    assert(body);
    std::vector<std::string> users;
    cJSON *messages = cJSON_GetObjectItemCaseSensitive(body.get(), "messages");
    assert(cJSON_IsArray(messages));
    cJSON *message;
    cJSON_ArrayForEach(message, messages) {
        cJSON *role = cJSON_GetObjectItemCaseSensitive(message, "role");
        cJSON *content = cJSON_GetObjectItemCaseSensitive(message, "content");
        if(cJSON_IsString(role) && !std::strcmp(role->valuestring, "user")) {
            assert(cJSON_IsString(content));
            users.emplace_back(content->valuestring);
        }
    }
    provider.user_turns.push_back(std::move(users));
    const char *wire = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Completed\"},\"finish_reason\":\"stop\"}]}";
    const size_t count = std::strlen(wire);
    assert(count <= capacity);
    std::memcpy(response, wire, count);
    return static_cast<int32_t>(count);
}

struct ExecutionFixture {
    int effects = 0;
    bool cancelled = false;
    bool ack = true;
    bool cancel_during_effect = false;
    bool cancel_during_ack = false;
    uint64_t actual_deadline = 0;
    uint64_t actual_generation = 0;
    bool before_ack = false;
    bool actual_durable_started = false;
    std::string response = "{\"ok\":true}";
    std::string actual_call;
    std::map<std::string, std::pair<std::string, unravel::ToolResult>> ledger;
};

static bool effect(const char *, std::string &output, const unravel::ToolCallContext &call, void *opaque)
{
    auto &fixture = *static_cast<ExecutionFixture *>(opaque);
    ++fixture.effects;
    fixture.actual_call = call.call_id;
    fixture.actual_deadline = call.deadline_us;
    fixture.actual_generation = call.generation;
    fixture.actual_durable_started = call.durable_started;
    if (fixture.cancel_during_effect) fixture.cancelled = true;
    assert(call.attempt == 1 && call.run_id && *call.run_id);
    output = fixture.response;
    return true;
}
static bool cancelled(uint64_t generation, void *opaque)
{
    return generation != 7 || static_cast<ExecutionFixture *>(opaque)->cancelled;
}
static unravel::ToolDecision before(const char *, const char *args, const unravel::ToolCallContext &call,
                                    unravel::ToolResult &recorded, void *opaque)
{
    auto &ledger = static_cast<ExecutionFixture *>(opaque)->ledger;
    const auto found = ledger.find(call.call_id);
    if (found != ledger.end()) {
        recorded = found->second.second;
        if (found->second.first != args) {
            recorded.state = unravel::ToolState::failed;
            recorded.error = "identity conflict";
            return unravel::ToolDecision::refuse;
        }
        return recorded.state == unravel::ToolState::unknown ?
            unravel::ToolDecision::refuse : unravel::ToolDecision::reuse;
    }
    unravel::ToolResult started;
    started.state = unravel::ToolState::unknown;
    started.error = "started, no committed terminal result";
    ledger.emplace(call.call_id, std::make_pair(std::string(args), started));
    assert(!call.durable_started);
    recorded.durable_acknowledged = static_cast<ExecutionFixture *>(opaque)->before_ack;
    if (static_cast<ExecutionFixture *>(opaque)->cancel_during_ack)
        static_cast<ExecutionFixture *>(opaque)->cancelled = true;
    return unravel::ToolDecision::execute;
}
static bool after(const char *, const char *args, const unravel::ToolCallContext &call,
                  const unravel::ToolResult &result, void *opaque)
{
    auto &fixture = *static_cast<ExecutionFixture *>(opaque);
    if (!fixture.ack) return false;
    fixture.ledger[call.call_id] = {args, result};
    return true;
}
static void execution_behavior()
{
    ExecutionFixture fixture;
    unravel::Agent agent(nullptr);
    assert(agent.add_tool({"effect","validated action","{\"type\":\"object\"}",effect,&fixture}));
    agent.set_tool_policy(before, after, &fixture);
    unravel::ToolCallContext call;
    call.conversation_id = "conversation";
    call.provider_session = "provider-session";
    call.run_id = "run";
    call.call_id = "actual-provider-id";
    call.generation = 7;
    call.cancelled = cancelled;
    call.cancellation_context = &fixture;
    auto result = agent.execute_tool("effect","{\"z\":1,\"a\":2}",call);
    assert(result.state == unravel::ToolState::succeeded && fixture.effects == 1);
    assert(fixture.actual_call == call.call_id);
    result = agent.execute_tool("effect","{\"a\":2,\"z\":1}",call);
    assert(result.state == unravel::ToolState::succeeded && fixture.effects == 1);
    result = agent.execute_tool("effect","{\"a\":3,\"z\":1}",call);
    assert(result.state == unravel::ToolState::failed && fixture.effects == 1);
    call.call_id = "interrupted";
    fixture.ack = false;
    result = agent.execute_tool("effect","{}",call);
    assert(result.state == unravel::ToolState::unknown && fixture.effects == 2);
    fixture.ack = true;
    result = agent.execute_tool("effect","{}",call);
    assert(result.state == unravel::ToolState::unknown && fixture.effects == 2);
    call.call_id = "domain-failure";
    fixture.response = "{\"ok\":false,\"error\":\"controller refused\"}";
    result = agent.execute_tool("effect","{}",call);
    assert(result.state == unravel::ToolState::failed && result.output == fixture.response);
    assert(fixture.effects == 3);
    call.call_id = "stale";
    call.generation = 6;
    result = agent.execute_tool("effect","{}",call);
    assert(result.state == unravel::ToolState::cancelled && fixture.effects == 3);
    call.generation = 7;
    fixture.cancelled = true;
    result = agent.execute_tool("effect","{}",call);
    assert(result.state == unravel::ToolState::cancelled && fixture.effects == 3);
    fixture.cancelled = false;
    call.deadline_us = unravel::monotonic_time_us() - 1;
    result = agent.execute_tool("effect","{}",call);
    assert(result.state != unravel::ToolState::succeeded && fixture.effects == 3);
    call.deadline_us = 0;
    result = agent.execute_tool("effect","{\"duplicate\":1,\"duplicate\":2}",call);
    assert(result.state == unravel::ToolState::failed && fixture.effects == 3);
    result = agent.execute_tool("effect","{} trailing",call);
    assert(result.state == unravel::ToolState::failed && fixture.effects == 3);
    call.call_id = "cancelled-after-start";
    fixture.cancel_during_ack = true;
    result = agent.execute_tool("effect","{}",call);
    assert(result.state == unravel::ToolState::cancelled && fixture.effects == 3);
    fixture.cancel_during_ack = false;
    fixture.cancelled = false;
    call.call_id = "cancelled-during-effect";
    fixture.cancel_during_effect = true;
    result = agent.execute_tool("effect","{}",call);
    assert(result.state == unravel::ToolState::unknown && fixture.effects == 4);
    fixture.cancel_during_effect = false;
    fixture.cancelled = false;
    result = agent.execute_tool("effect","{}",call);
    assert(result.state == unravel::ToolState::unknown && fixture.effects == 4);
    std::string tools;
    assert(agent.native_tools_json(tools));
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> definitions(cJSON_Parse(tools.c_str()), cJSON_Delete);
    cJSON *definition = cJSON_GetArrayItem(definitions.get(),0);
    assert(!std::strcmp(cJSON_GetObjectItemCaseSensitive(definition,"type")->valuestring,"function"));
    assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(definition,"parameters")));
    for (unsigned i = 1; i < UNRAVEL_MAX_TOOLS; ++i)
        assert(agent.add_tool({"read" + std::to_string(i),"read","{}",effect,&fixture}));
    assert(!agent.add_tool({"seventeenth","read","{}",effect,&fixture}));
}

struct ToolProvider {
    int requests = 0;
    bool fail_domain = true;
    bool observed_error = false;
};
static int32_t tool_model(const uint8_t *request, size_t length, uint8_t *response,
                          size_t capacity, void *opaque)
{
    auto &provider = *static_cast<ToolProvider *>(opaque);
    std::string wire;
    if (++provider.requests == 1) {
        wire = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"real-call-123\",\"type\":\"function\",\"function\":{\"name\":\"effect\",\"arguments\":\"{}\"}}]},\"finish_reason\":\"tool_calls\"}]}";
    } else {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> body(cJSON_ParseWithLength(reinterpret_cast<const char *>(request),length),cJSON_Delete);
        cJSON *messages = cJSON_GetObjectItemCaseSensitive(body.get(),"messages");
        cJSON *message;
        cJSON_ArrayForEach(message,messages) {
            cJSON *role = cJSON_GetObjectItemCaseSensitive(message,"role");
            if (cJSON_IsString(role) && !std::strcmp(role->valuestring,"tool")) {
                cJSON *content = cJSON_GetObjectItemCaseSensitive(message,"content");
                assert(cJSON_IsString(content));
                provider.observed_error = std::strstr(content->valuestring,"controller refused") &&
                    std::strstr(content->valuestring,"is_error");
            }
        }
        wire = "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Action refused\"},\"finish_reason\":\"stop\"}]}";
    }
    assert(wire.size() <= capacity);
    std::memcpy(response,wire.data(),wire.size());
    return static_cast<int32_t>(wire.size());
}
static bool deny_archive(const char *,size_t,uint64_t,void *) { return false; }
static void bridge_behavior()
{
    ToolProvider provider;
    ExecutionFixture fixture;
    fixture.response = "{\"ok\":false,\"error\":\"controller refused\"}";
    unravel::Agent agent(tool_model,&provider);
    assert(agent.add_tool({"effect","action","{\"type\":\"object\"}",effect,&fixture}));
    unravel::ToolCallContext call;
    call.run_id = "owned-run"; call.generation = 7;
    agent.set_call_context(call);
    const auto result = agent.run("do action","fixture");
    assert(result.ok && provider.requests == 2 && provider.observed_error);
    assert(fixture.effects == 1 && fixture.actual_call == "real-call-123");
    assert(fixture.actual_generation == 7 && fixture.actual_deadline > 0);
    std::string canonical,error;
    uint64_t offset;
    assert(agent.checkpoint_session(canonical,offset,error));
    unravel::Agent restored(nullptr);
    assert(restored.load_session(canonical.c_str(),offset,error));
    assert(!restored.load_session("{\"id\":\"s\",\"messages\":[{\"role\":\"tool\",\"call_id\":\"orphan\",\"name\":\"effect\",\"content\":\"ok\",\"is_error\":false}]}",0,error));
    agent.set_checkpoint(deny_archive);
    const int requests_before = provider.requests;
    const auto rejected = agent.run("must not dispatch","fixture");
    assert(!rejected.ok && provider.requests == requests_before && fixture.effects == 1);
    ToolProvider uncertain_provider;
    ExecutionFixture uncertain_fixture;
    uncertain_fixture.ack = false;
    unravel::Agent uncertain(tool_model,&uncertain_provider);
    assert(uncertain.add_tool({"effect","action","{}",effect,&uncertain_fixture}));
    uncertain.set_call_context(call);
    uncertain.set_tool_policy(before,after,&uncertain_fixture);
    auto unknown = uncertain.run("effect","fixture");
    assert(!unknown.ok && uncertain_fixture.effects == 1 && uncertain_provider.requests == 1);
    assert(uncertain.session().find("tool_unknown") != std::string::npos);
    uncertain_fixture.ack = true;
    unknown = uncertain.run("resume without replay","fixture");
    assert(!unknown.ok && uncertain_fixture.effects == 1 && uncertain_provider.requests == 1);
}

static void durable_admission_behavior()
{
    ExecutionFixture fixture;
    unravel::Agent agent(nullptr);
    assert(agent.add_tool({"required","write","{}",effect,&fixture,true}));
    assert(agent.add_tool({"local","local","{}",effect,&fixture}));
    unravel::ToolCallContext call;
    call.call_id = "missing-policy"; call.run_id = "run"; call.generation = 7;
    call.durable_started = true; // A forged caller value never authorizes effects.
    auto result = agent.execute_tool("required","{}",call);
    assert(result.state == unravel::ToolState::failed && fixture.effects == 0);
    result = agent.execute_tool("local","{}",call);
    assert(result.state == unravel::ToolState::succeeded && fixture.effects == 1);
    assert(!fixture.actual_durable_started);
    agent.set_tool_policy(before,after,&fixture);
    call.call_id = "ephemeral-is-not-durable";
    result = agent.execute_tool("required","{}",call);
    assert(result.state == unravel::ToolState::failed && fixture.effects == 1);
    fixture.before_ack = true;
    call.call_id = "committed-start";
    result = agent.execute_tool("required","{}",call);
    assert(result.state == unravel::ToolState::succeeded && fixture.effects == 2);
    assert(result.durable_acknowledged && fixture.actual_durable_started);
    fixture.ack = false;
    call.call_id = "uncommitted-terminal";
    result = agent.execute_tool("required","{}",call);
    assert(result.state == unravel::ToolState::unknown && !result.durable_acknowledged);
    assert(!result.error.empty() && fixture.effects == 3);
    fixture.ack = true;
    result = agent.execute_tool("required","{}",call);
    assert(result.state == unravel::ToolState::unknown && fixture.effects == 3);
}

int main()
{
    execution_behavior();
    bridge_behavior();
    durable_admission_behavior();
    Provider provider;
    unravel::Agent agent(model, &provider);
    auto first = agent.run("first", "fixture");
    assert(first.ok && first.output == "Completed");
    const std::string saved = agent.session();

    fail_next_array = true;
    auto failed = agent.run("must-not-enter-history", "fixture");
    assert(!failed.ok);
    assert(agent.session() == saved);

    auto recovered = agent.run("recovered", "fixture");
    assert(recovered.ok && recovered.output == "Completed");
    agent.reset();
    auto reset = agent.run("reset", "fixture");
    assert(reset.ok && reset.output == "Completed");
    const std::vector<std::vector<std::string>> expected = {{"first"}, {"first", "recovered"}, {"reset"}};
    assert(provider.user_turns == expected);
    std::cout << "PASS: workspace allocation failure preserves conversation; retry and reset recover through real Rust RPC\n";
}
