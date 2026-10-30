#include "nebius_sandbox.hpp"
#include "cJSON.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>

namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
const char *id = "00000000-0000-4000-8000-000000000001";
const char *image_id = "00000000-0000-4000-8000-000000000002";
const char *valid = R"json({"language":"python","code":"print(42)"})json";
const cJSON *field(const cJSON *j, const char *key) { return cJSON_GetObjectItemCaseSensitive(j, key); }
bool eq(const cJSON *j, const char *value) { return cJSON_IsString(j) && !std::strcmp(j->valuestring, value); }
Json json(const std::string &s) { Json j(cJSON_ParseWithOpts(s.c_str(), nullptr, true), cJSON_Delete); assert(j); return j; }
std::string encode(cJSON *j) { char *s = cJSON_PrintUnformatted(j); assert(s); std::string out(s); cJSON_free(s); return out; }
std::string args(const char *action = "status") {
    return std::string("{\"operation_id\":\"") + id + "\",\"action\":\"" + action + "\"}";
}
std::string response(const char *status, const char *state = R"({"exit_code":0,"signal":null,"timed_out":false})",
                     const char *output = R"({"value":"42\n","encoding":"ascii","truncated":false})") {
    return std::string("{\"uuid\":\"") + id + "\",\"kind\":\"instance\",\"status\":\"" + status +
        "\",\"metadata\":{\"stdin\":\"must-not-leak\",\"result\":{\"state\":" + state + ",\"stdout\":" + output +
        R"(,"stderr":{"value":"","encoding":"ascii","truncated":false}}},"result_image_uuid":")" + image_id + "\"}";
}
struct Fixture {
    int calls = 0, before_calls = 0, after_calls = 0;
    bool delivered = true, acknowledge = true, after_ack = true, stop = false, stop_in_request = false, stop_in_policy = false;
    int status = 201;
    std::string reply = std::string("{\"uuid\":\"") + id + "\"}";
    std::string method, path, body;
    unravel::ToolState observed = unravel::ToolState::requested;
};
bool cancelled(uint64_t, void *p) { return static_cast<Fixture *>(p)->stop; }
bool request(const char *method, const char *path, const char *body, std::string &reply,
             int &status, const unravel::ToolCallContext &call, void *p) {
    auto &f = *static_cast<Fixture *>(p);
    assert(call.durable_started && !call.is_cancelled() && !call.is_expired());
    ++f.calls; f.method = method; f.path = path; f.body = body ? body : "";
    status = f.status; reply = f.reply;
    if (f.stop_in_request) f.stop = true;
    return f.delivered;
}
unravel::ToolDecision before(const char *, const char *, const unravel::ToolCallContext &, unravel::ToolResult &r, void *p) {
    auto &f = *static_cast<Fixture *>(p); ++f.before_calls;
    r.durable_acknowledged = f.acknowledge;
    if (f.stop_in_policy) f.stop = true;
    return unravel::ToolDecision::execute;
}
bool after(const char *, const char *, const unravel::ToolCallContext &, const unravel::ToolResult &r, void *p) {
    auto &f = *static_cast<Fixture *>(p); ++f.after_calls; f.observed = r.state; return f.after_ack;
}
struct Harness {
    Fixture f;
    nebius_sandbox::Tools tools;
    unravel::Agent agent;
    unravel::ToolCallContext call;
    explicit Harness(bool network = false) : tools({"tag:python:3.12-slim", network}, request, &f), agent(nullptr) {
        assert(tools.register_tools(agent));
        agent.set_tool_policy(before, after, &f);
        call.run_id = "sandbox-test-run"; call.call_id = "sandbox-test-call";
        call.cancelled = cancelled; call.cancellation_context = &f;
    }
    unravel::ToolResult run(const std::string &s = valid) { return agent.execute_tool("sandbox_run_code", s.c_str(), call); }
    unravel::ToolResult operation(const char *action = "status") { return agent.execute_tool("sandbox_operation", args(action).c_str(), call); }
};
void invalid_arguments() {
    Harness h;
    for (const char *bad : {"{", "[]", "{}", R"({"language":"ruby","code":"1"})", R"({"language":1,"code":"1"})",
            R"({"language":"python","code":null})", R"({"language":"python","code":" \n\t"})",
            R"({"language":"python","code":"1","network":1})", R"({"language":"python","code":"1","save_filesystem":"true"})",
            R"({"language":"python","code":"1","timeout_seconds":0})", R"({"language":"python","code":"1","timeout_seconds":301})",
            R"({"language":"python","code":"1","timeout_seconds":1.5})",
            R"({"language":"python","code":"1","image_uuid":"tag:python:3.12-slim"})",
            R"({"language":"python","code":"1","image_uuid":"../../elsewhere"})", R"({"language":"python","code":"1","image_uuid":null})",
            R"({"language":"python","code":"1","env":{"TOKEN":"secret"}})", R"({"language":"python","code":"1","code":"2"})",
            R"({"language":"python","code":"a\u0000b"})", R"({"language":"python","code":"1"} trailing)"}) {
        const auto r = h.run(bad); assert(r.state != unravel::ToolState::succeeded && h.f.calls == 0);
    }
    std::string oversized = "{\"language\":\"python\",\"code\":\"" + std::string(4097, 'x') + "\"}";
    assert(h.run(oversized).state == unravel::ToolState::failed && h.f.calls == 0);
    std::string bad_utf8 = "{\"language\":\"python\",\"code\":\""; bad_utf8 += char(0xc0); bad_utf8 += char(0x80); bad_utf8 += "\"}";
    assert(h.run(bad_utf8).state == unravel::ToolState::failed && h.f.calls == 0);
    for (const char *bad : {"{}", R"({"operation_id":"bad","action":"status"})", R"({"operation_id":"00000000-0000-4000-8000-000000000001","action":"poll"})",
                          R"({"operation_id":"00000000-0000-4000-8000-000000000001","action":"cancel","retry":true})"}) {
        assert(h.agent.execute_tool("sandbox_operation", bad, h.call).state == unravel::ToolState::failed && h.f.calls == 0);
    }
}
void submission() {
    Harness h;
    auto r = h.run(); auto accepted = json(r.output);
    assert(r.state == unravel::ToolState::succeeded && cJSON_IsTrue(field(accepted.get(), "ok")));
    assert(eq(field(accepted.get(), "status"), "submitted") && eq(field(accepted.get(), "operation_id"), id));
    assert(!field(accepted.get(), "exit_code") && !field(accepted.get(), "state"));
    assert(h.f.calls == 1);
    auto body = json(h.f.body);
    assert(cJSON_IsFalse(field(body.get(), "shell")) && cJSON_IsFalse(field(body.get(), "preserve_env")) && cJSON_IsTrue(field(body.get(), "disposable")));
    assert(cJSON_IsFalse(field(field(body.get(), "networking"), "enabled")));
    assert(eq(field(field(body.get(), "stdin"), "value"), "cHJpbnQoNDIp"));
    assert(eq(field(field(body.get(), "stdin"), "encoding"), "base64") && cJSON_IsTrue(field(field(body.get(), "stdin"), "close")));
    assert(h.run(R"({"language":"shell","code":"echo ok","network":true})").state == unravel::ToolState::failed && h.f.calls == 1);
    Harness permitted(true);
    std::string continuation = std::string(R"({"language":"shell","code":"echo ok","network":true,"save_filesystem":true,"timeout_seconds":300,"image_uuid":")") + image_id + "\"}";
    assert(permitted.run(continuation).state == unravel::ToolState::succeeded);
    body = json(permitted.f.body);
    assert(eq(field(body.get(), "image"), image_id) && cJSON_IsFalse(field(body.get(), "disposable")));
    assert(cJSON_IsTrue(field(field(body.get(), "networking"), "enabled")) && field(body.get(), "timeout")->valueint == 300);
    assert(permitted.run().state == unravel::ToolState::succeeded);
    body = json(permitted.f.body); assert(cJSON_IsFalse(field(field(body.get(), "networking"), "enabled")));
    assert(h.run(R"({"language":"python","code":"é"})").state == unravel::ToolState::succeeded);
    body = json(h.f.body); assert(eq(field(field(body.get(), "stdin"), "value"), "w6k="));
    std::string boundary = "{\"language\":\"python\",\"code\":\"" + std::string(4096, 'x') + "\"}";
    assert(h.run(boundary).state == unravel::ToolState::succeeded);
    h.f.delivered = false;
    r = h.run(); assert(r.state == unravel::ToolState::unknown && eq(field(json(r.output).get(), "operation_id"), id));
    assert(r.output.find("do_not_blindly_resubmit") != std::string::npos);
    h.f.delivered = true;
    for (const char *bad : {"{}", "not json", R"({"uuid":"bad"})", R"({"uuid":"00000000-0000-4000-8000-000000000001","uuid":"00000000-0000-4000-8000-000000000002"})"}) {
        h.f.reply = bad; assert(h.run().state == unravel::ToolState::unknown);
    }
    h.f.reply = "secret-provider-body";
    for (int status : {400, 401, 403, 409, 429}) {
        h.f.status = status; r = h.run(); assert(r.state == unravel::ToolState::failed && r.output.find("secret") == std::string::npos);
    }
    for (int status : {200, 202, 408, 500, 503}) { h.f.status = status; assert(h.run().state == unravel::ToolState::unknown); }
}
void status_projection() {
    Harness h; h.f.status = 200;
    for (const char *status : {"PENDING", "ASSIGNED", "EXECUTING"}) {
        h.f.reply = response(status); auto r = h.operation(); auto j = json(r.output);
        assert(r.state == unravel::ToolState::succeeded && eq(field(j.get(), "state"), "pending"));
        assert(eq(field(j.get(), "status"), status) && !field(j.get(), "exit_code") && !field(j.get(), "result_image_uuid"));
        assert(h.f.method == "GET" && h.f.path == std::string("/operations/") + id && h.f.body.empty());
    }
    h.f.reply = response("SUCCESS"); auto r = h.operation(); auto j = json(r.output);
    assert(r.state == unravel::ToolState::succeeded && eq(field(j.get(), "state"), "succeeded"));
    assert(field(j.get(), "exit_code")->valueint == 0 && eq(field(j.get(), "result_image_uuid"), image_id));
    assert(eq(field(field(j.get(), "stdout"), "value"), "42\n") && r.output.find("must-not-leak") == std::string::npos);
    h.f.reply = response("SUCCESS", R"({"exit_code":0})", R"({"value":"","encoding":"ascii"})");
    r = h.operation(); assert(r.state == unravel::ToolState::succeeded); j = json(r.output);
    assert(!field(j.get(), "signal") && !field(j.get(), "timed_out") && !field(field(j.get(), "stdout"), "truncated"));
    // Nebius reports signal -1 for a process that was not signalled.
    h.f.reply = response("SUCCESS", R"({"exit_code":0,"signal":-1,"timed_out":false,"pid":8,"core_dump":false})");
    r = h.operation(); j = json(r.output);
    assert(r.state == unravel::ToolState::succeeded && eq(field(j.get(), "state"), "succeeded"));
    h.f.reply = std::string("{\"uuid\":\"") + id + R"(","kind":"instance","status":"SUCCESS","metadata":{"result":{"state":{"exit_code":0}}}})";
    r = h.operation(); j = json(r.output);
    assert(r.state == unravel::ToolState::succeeded && !field(j.get(), "stdout") && !field(j.get(), "stderr"));
    for (const char *state : {R"({"exit_code":2})", R"({"exit_code":null,"signal":9})", R"({"exit_code":0,"signal":15})", R"({"exit_code":0,"timed_out":true})"}) {
        h.f.reply = response("SUCCESS", state); r = h.operation(); j = json(r.output);
        assert(r.state == unravel::ToolState::failed && cJSON_IsFalse(field(j.get(), "ok")) && field(j.get(), "stdout"));
    }
    for (const char *status : {"FAILED", "CANCELLED"}) {
        h.f.reply = response(status); r = h.operation(); j = json(r.output);
        assert(r.state == (!std::strcmp(status, "FAILED") ? unravel::ToolState::failed : unravel::ToolState::cancelled));
        assert(!field(j.get(), "result_image_uuid") && cJSON_IsFalse(field(j.get(), "ok")) && field(j.get(), "stdout"));
    }
    h.f.reply = response("SUCCESS", R"({"exit_code":0})", R"({"value":"AP8=","encoding":"base64","truncated":true})");
    r = h.operation(); j = json(r.output);
    assert(r.state == unravel::ToolState::succeeded && eq(field(field(j.get(), "stdout"), "value"), "AP8="));
    assert(eq(field(field(j.get(), "stdout"), "encoding"), "base64") && cJSON_IsTrue(field(field(j.get(), "stdout"), "truncated")));
    for (const char *bad_state : {"{}", R"({"exit_code":null})", R"({"exit_code":"0"})", R"({"exit_code":0.5})", R"({"exit_code":0,"signal":"9"})", R"({"exit_code":0,"timed_out":0})"}) {
        h.f.reply = response("SUCCESS", bad_state); assert(h.operation().state == unravel::ToolState::unknown);
    }
    for (const char *bad_stream : {"null", R"({"value":"x","encoding":"rot13"})", R"({"value":2,"encoding":"ascii"})", R"({"value":"x","encoding":"ascii","truncated":1})", R"({"value":"A===","encoding":"base64"})"}) {
        h.f.reply = response("SUCCESS", R"({"exit_code":0})", bad_stream); assert(h.operation().state == unravel::ToolState::unknown);
    }
    for (const char *bad : {"{}", "not json", R"({"uuid":"00000000-0000-4000-8000-000000000002","kind":"instance","status":"SUCCESS"})",
        R"({"uuid":"00000000-0000-4000-8000-000000000001","kind":"image","status":"PENDING"})",
        R"({"uuid":"00000000-0000-4000-8000-000000000001","kind":"instance","status":"NEW_STATUS"})",
        R"({"uuid":"00000000-0000-4000-8000-000000000001","kind":"instance","status":"SUCCESS"})"}) {
        h.f.reply = bad; assert(h.operation().state == unravel::ToolState::unknown);
    }
    auto oversized = json(response("SUCCESS"));
    cJSON *out = const_cast<cJSON *>(field(field(field(oversized.get(), "metadata"), "result"), "stdout"));
    cJSON_ReplaceItemInObjectCaseSensitive(out, "value", cJSON_CreateString(std::string(513, 'x').c_str()));
    h.f.reply = encode(oversized.get()); assert(h.operation().state == unravel::ToolState::unknown);
    // Worst-case JSON escaping must survive Agent's 8192-byte text and 32KiB nested-wire contract.
    std::string controls(512, '\1');
    cJSON_ReplaceItemInObjectCaseSensitive(out, "value", cJSON_CreateString(controls.c_str()));
    cJSON *err = const_cast<cJSON *>(field(field(field(oversized.get(), "metadata"), "result"), "stderr"));
    cJSON_ReplaceItemInObjectCaseSensitive(err, "value", cJSON_CreateString(controls.c_str()));
    h.f.reply = encode(oversized.get()); r = h.operation(); assert(r.state == unravel::ToolState::succeeded && r.output.size() <= 8192);
    h.f.delivered = false; assert(h.operation().state == unravel::ToolState::unknown);
    h.f.delivered = true; h.f.status = 404; assert(h.operation().state == unravel::ToolState::unknown);
}
void cancellation_and_durability() {
    Harness h; h.f.status = 202; h.f.reply.clear();
    auto r = h.operation("cancel"); auto j = json(r.output);
    assert(r.state == unravel::ToolState::succeeded && cJSON_IsTrue(field(j.get(), "cancellation_requested")));
    assert(!field(j.get(), "status") && !field(j.get(), "state") && h.f.method == "DELETE" && h.f.body.empty());
    h.f.status = 409; assert(h.operation("cancel").state == unravel::ToolState::failed);
    h.f.status = 500; assert(h.operation("cancel").state == unravel::ToolState::unknown);
    h.f.status = 408; assert(h.operation("cancel").state == unravel::ToolState::unknown);
    h.f.delivered = false; r = h.operation("cancel"); assert(r.state == unravel::ToolState::unknown && r.output.find("do_not_blindly_retry") != std::string::npos);
    Harness gated;
    gated.agent.set_tool_policy(nullptr, nullptr);
    gated.call.durable_started = true; // Caller cannot forge committed admission.
    assert(gated.run().state == unravel::ToolState::failed && gated.f.calls == 0);
    assert(gated.operation("cancel").state == unravel::ToolState::failed && gated.f.calls == 0);
    gated.agent.set_tool_policy(before, after, &gated.f); gated.f.acknowledge = false;
    assert(gated.run().state == unravel::ToolState::failed && gated.f.calls == 0);
    gated.f.acknowledge = true; gated.f.stop = true;
    assert(gated.run().state == unravel::ToolState::cancelled && gated.f.calls == 0);
    gated.f.stop = false; gated.call.deadline_us = 1;
    assert(gated.run().state == unravel::ToolState::failed && gated.f.calls == 0);
    gated.call.deadline_us = 0; gated.f.stop_in_policy = true;
    assert(gated.run().state == unravel::ToolState::cancelled && gated.f.calls == 0);
    gated.f.stop = false; gated.f.stop_in_policy = false; gated.f.stop_in_request = true;
    assert(gated.run().state == unravel::ToolState::unknown && gated.f.calls == 1 && gated.f.method == "POST");
    gated.f.stop = false; gated.f.stop_in_request = false; gated.f.after_ack = false;
    assert(gated.run().state == unravel::ToolState::unknown && gated.f.calls == 2);
}
struct Provider { int calls = 0; bool observed_submission = false; };
int32_t model(const uint8_t *request_bytes, size_t length, uint8_t *output, size_t capacity, void *context) {
    auto &p = *static_cast<Provider *>(context);
    std::string wire;
    if (++p.calls == 1) {
        wire = R"({"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"sandbox-native-call","type":"function","function":{"name":"sandbox_run_code","arguments":"{\"language\":\"python\",\"code\":\"print(42)\"}"}}]},"finish_reason":"tool_calls"}]})";
    } else {
        auto body = json(std::string(reinterpret_cast<const char *>(request_bytes), length));
        const cJSON *message = nullptr;
        cJSON_ArrayForEach(message, field(body.get(), "messages")) {
            if (eq(field(message, "role"), "tool")) {
                const cJSON *content = field(message, "content"); assert(cJSON_IsString(content));
                p.observed_submission = std::strstr(content->valuestring, "submitted") && std::strstr(content->valuestring, id) &&
                    !std::strstr(content->valuestring, "exit_code");
            }
        }
        wire = R"({"choices":[{"message":{"role":"assistant","content":"Submitted; execution still needs a status query."},"finish_reason":"stop"}]})";
    }
    assert(wire.size() <= capacity); std::memcpy(output, wire.data(), wire.size()); return static_cast<int32_t>(wire.size());
}
void model_followup() {
    Fixture f; Provider p; nebius_sandbox::Tools tools({}, request, &f); unravel::Agent agent(model, &p);
    assert(tools.register_tools(agent)); agent.set_tool_policy(before, after, &f);
    unravel::ToolCallContext call; call.run_id = "sandbox-model-run"; agent.set_call_context(call);
    auto r = agent.run("Run print(42) remotely", "fixture");
    assert(r.ok && p.calls == 2 && p.observed_submission && f.calls == 1 && f.after_calls == 1);
    assert(r.output == "Submitted; execution still needs a status query.");
}
}
int main() {
    invalid_arguments(); submission(); status_projection(); cancellation_and_durability(); model_followup();
    std::cout << "Nebius production Tools + Agent protocol/admission/model-follow-up scenarios passed\n";
}
