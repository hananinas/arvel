#include "niko_protocol.hpp"
#include "cJSON.h"
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>

// Only host clock/cancellation plumbing is supplied here; all parsing,
// admission, wire construction and outcome classification are production code.
namespace unravel {
uint64_t monotonic_time_us() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
bool ToolCallContext::is_cancelled() const { return cancelled && cancelled(generation, cancellation_context); }
bool ToolCallContext::is_expired() const { return deadline_us && monotonic_time_us() >= deadline_us; }
}
using namespace niko_home_control;
using namespace niko_home_control::protocol;
const char *id = "00000000-0000-4000-8000-000000000001";
std::string snapshot(const char *model = "dimmer", const char *control = "true", const char *online = "True") {
    return std::string(R"json({"Method":"devices.list","Params":[{"Devices":[{"Uuid":")json") + id +
        R"json(","Name":"Synthetic fixture","Type":"action","Technology":"nikohomecontrol","Model":")json" + model +
        R"json(","Online":")json" + online +
        R"json(","Parameters":[{"LocationId":"00000000-0000-4000-8000-000000000002"},{"LocationName":"Synthetic room"}],"Properties":[{"Status":"Off"},{"Brightness":"20"}],"PropertyDefinitions":[{"Status":{"Description":"Choice(On,Off)","HasStatus":"true","CanControl":")json" + control +
        R"json("}},{"Brightness":{"Description":"Range(0.00,100.00,1.00)","HasStatus":"true","CanControl":"true"}}]}]}]})json";
}
std::string event(const char *properties) {
    return std::string(R"({"Method":"devices.status","Params":[{"Devices":[{"Uuid":")") + id + R"(","Properties":)" + properties + R"(}]}]})";
}
std::string args(const char *tail) { return std::string("{\"device_id\":\"") + id + "\"," + tail + "}"; }
bool ingest(Inventory &i, const std::string &s, bool list, uint64_t time) { return i.ingest(list ? "hobby/control/devices/rsp" : "hobby/control/devices/evt", s.data(), s.size(), time); }
std::string result_state(const std::string &result) {
    cJSON *j = cJSON_Parse(result.c_str()); assert(j);
    cJSON *s = cJSON_GetObjectItemCaseSensitive(j, "state"); assert(cJSON_IsString(s));
    std::string out = s->valuestring; cJSON_Delete(j); return out;
}
bool cancel(uint64_t, void *p) { return *static_cast<bool *>(p); }
int main() {
    Inventory inventory; inventory.connected = true;
    assert(ingest(inventory, snapshot(), true, 10)); assert(inventory.ready);
    unravel::ToolCallContext call; Desired desired; std::string wire, failure;
    const std::string request = args("\"status\":\"On\",\"brightness\":0");
    std::vector<std::string> allow{id};
    assert(!inventory.prepare(request.c_str(), allow, call, desired, wire, failure)); assert(failure == "durable_storage_required");
    call.durable_started = true;
    assert(!inventory.prepare(request.c_str(), {}, call, desired, wire, failure)); assert(failure == "owner_allowlist_required");
    assert(inventory.prepare(request.c_str(), allow, call, desired, wire, failure));
    cJSON *command = cJSON_Parse(wire.c_str()); assert(command);
    assert(!std::strcmp(cJSON_GetObjectItemCaseSensitive(command, "Method")->valuestring, "devices.control"));
    cJSON *device = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(command, "Params"), 0), "Devices"), 0);
    cJSON *properties = cJSON_GetObjectItemCaseSensitive(device, "Properties"); assert(cJSON_GetArraySize(properties) == 2);
    assert(!std::strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(properties, 0), "Status")->valuestring, "On"));
    assert(!std::strcmp(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(properties, 1), "Brightness")->valuestring, "0"));
    cJSON_Delete(command); // zero brightness did not rewrite requested On to Off.
    uint64_t baseline = inventory.sequence; desired.published_us = 100;
    assert(!inventory.matched(desired, baseline)); // MQTT acceptance is no observation.
    assert(result_state(inventory.outcome(desired, baseline, true, false, "timeout")) == "unknown");
    assert(ingest(inventory, event("[{\"Status\":\"On\"},{\"Brightness\":\"0\"}]"), false, 90));
    assert(!inventory.matched(desired, baseline)); // queued pre-publication frame is not fresh.
    assert(ingest(inventory, event("[{\"Status\":\"On\"}]"), false, 110));
    assert(result_state(inventory.outcome(desired, baseline, true, false, "timeout")) == "partial");
    assert(ingest(inventory, event("[{\"Brightness\":\"0.00\"}]"), false, 120));
    assert(inventory.matched(desired, baseline));
    assert(result_state(inventory.outcome(desired, baseline, true, false, "")) == "succeeded");
    assert(result_state(inventory.outcome(desired, baseline, true, true, "cancelled")) == "unknown");
    // The cached inventory can still match after a callback changes connection
    // generation. That observation is not fresh evidence for this publication.
    assert(!inventory.matched(desired, baseline, false));
    assert(result_state(inventory.outcome(desired, baseline, true, false, "connection changed", false)) == "unknown");
    cJSON *old_connection = cJSON_Parse(inventory.outcome(desired, baseline, true, false, "connection changed", false).c_str());
    cJSON *observed = cJSON_GetObjectItemCaseSensitive(old_connection, "observed");
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(observed, "Status"), "fresh")));
    cJSON_Delete(old_connection);
    bool cancelled = true; call.cancelled = cancel; call.cancellation_context = &cancelled;
    desired = {}; assert(!inventory.prepare(request.c_str(), allow, call, desired, wire, failure));
    assert(failure == "cancelled_before_publish"); call.cancelled = nullptr;
    call.deadline_us = 1; assert(!inventory.prepare(request.c_str(), allow, call, desired, wire, failure)); call.deadline_us = 0;
    for (const char *model : {"relay", "smartplug", "mood", "motor", "unknown"}) {
        assert(ingest(inventory, snapshot(model), true, 130)); desired = {};
        assert(!inventory.prepare(request.c_str(), allow, call, desired, wire, failure)); assert(failure == "unsupported_actuator_class");
    }
    assert(ingest(inventory, snapshot("dimmer", "false"), true, 140)); desired = {};
    assert(!inventory.prepare(request.c_str(), allow, call, desired, wire, failure)); assert(failure == "unsupported_status_definition");
    assert(ingest(inventory, snapshot("dimmer", "true", "False"), true, 150)); desired = {};
    assert(!inventory.prepare(request.c_str(), allow, call, desired, wire, failure)); assert(failure == "device_offline");
    assert(ingest(inventory, snapshot(), true, 160));
    for (const char *bad : {"\"brightness\":1.5", "\"brightness\":101", "\"status\":\"Toggle\"", "\"status\":\"On\",\"status\":\"Off\"", "\"topic\":\"other\""}) {
        desired = {}; assert(!inventory.prepare(args(bad).c_str(), allow, call, desired, wire, failure));
    }
    inventory.disconnect(); desired = {};
    assert(!inventory.prepare(request.c_str(), allow, call, desired, wire, failure));
    inventory.connected = true; // reconnect does not restore authority or a pending command.
    assert(!inventory.prepare(request.c_str(), allow, call, desired, wire, failure));
    assert(ingest(inventory, snapshot(), true, 170));
    assert(inventory.prepare(request.c_str(), allow, call, desired, wire, failure));
    std::string page = inventory.page(0, 1); assert(page.size() <= 8192);
    cJSON *p = cJSON_Parse(page.c_str()); assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(p, "devices")) == 1); cJSON_Delete(p);
    const std::string changed = R"({"Method":"devices.changed","Params":[{"Devices":[]}]})";
    assert(ingest(inventory, changed, false, 180)); assert(!inventory.ready);
    assert(!ingest(inventory, "{", true, 190)); assert(!inventory.ready);
    std::string deep(20, '['); deep += std::string(20, ']'); assert(!ingest(inventory, deep, true, 200));
    // Use a valid array of 65 objects to trigger the device bound, not a JSON
    // syntax failure.
    std::string oversized = "{\"Method\":\"devices.list\",\"Params\":[{\"Devices\":[";
    for (size_t i = 0; i < max_devices + 1; ++i) { if (i) oversized += ','; oversized += "{}"; }
    oversized += "]}]}";
    assert(!ingest(inventory, oversized, true, 210));
    assert(inventory.error == "inventory_capacity_exceeded");
    assert(ingest(inventory, snapshot(), true, 220));
    std::string too_many_properties = "[";
    for (size_t i = 0; i < max_properties + 1; ++i) {
        if (i) too_many_properties += ',';
        too_many_properties += "{\"synthetic" + std::to_string(i) + "\":\"value\"}";
    }
    too_many_properties += ']';
    assert(!ingest(inventory, event(too_many_properties.c_str()), false, 230));
    assert(!inventory.ready);
    assert(inventory.find(id)->properties.size() == 2); // failed event is atomic.
    std::array<char, max_frame + 1> buffer{}; Fragments fragments(buffer.data());
    const char *topic = "hobby/control/devices/rsp"; std::string list = snapshot();
    assert(fragments.append(topic, std::strlen(topic), list.data(), 7, 0, list.size())); assert(!fragments.complete());
    assert(fragments.append(nullptr, 0, list.data() + 7, list.size() - 7, 7, list.size())); assert(fragments.complete());
    assert(!std::memcmp(fragments.data(), list.data(), list.size()));
    fragments.reset(); assert(!fragments.append(topic, std::strlen(topic), "x", 1, 1, 2));
    assert(!fragments.append(topic, std::strlen(topic), "x", 1, 0, max_frame + 1));
    assert(fragments.append(topic, std::strlen(topic), "x", 1, 0, 3));
    assert(!fragments.append(nullptr, 0, "y", 1, 2, 3));
    uint64_t expiry = 0;
    const std::string jwt = "synthetic.eyJleHAiOjIwMDAwMDAwMDB9.signature";
    assert(token_expiry(jwt, expiry)); assert(expiry == 2000000000);
    assert(!token_expiry("invalid", expiry));
    Config config; config.enabled = true; config.controller = "synthetic.local"; config.tls_server_name = "synthetic.local";
    config.username = "synthetic"; config.jwt = jwt; config.ca_pem = "-----BEGIN CERTIFICATE-----\nsynthetic\n-----END CERTIFICATE-----";
    assert(config_valid(config, 1800000000, failure)); // TLS itself verifies actual PEM/chain, not this metadata check.
    assert(!config_valid(config, 2000000000, failure));
    assert(!config_valid(config, 0, failure));
    config.tls_server_name.clear(); assert(!config_valid(config, 1800000000, failure));
    std::cout << "Niko production protocol/admission/outcome scenarios passed\n";
}
