#include "niko_home_control.hpp"
#include "niko_protocol.hpp"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>

namespace niko_home_control {
namespace {
using protocol::Inventory;
const char *const topics[] = {
    "hobby/control/devices/rsp", "hobby/control/devices/evt", "hobby/control/devices/err",
    "hobby/control/locations/rsp", "hobby/control/locations/evt", "hobby/control/locations/err",
    // v1.4 location error examples use singular location, subscribe both forms.
    "hobby/control/location/err"
};
uint64_t clock_us() { return uint64_t(esp_timer_get_time()); }
std::string error_json(const char *error, const char *state = "failed") {
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "ok", false);
    cJSON_AddStringToObject(j, "state", state); cJSON_AddStringToObject(j, "error", error);
    char *s = cJSON_PrintUnformatted(j); std::string out = s ? s : "{}"; cJSON_free(s); cJSON_Delete(j); return out;
}
struct Frame { char topic[96]{}; uint64_t received_us = 0; size_t length = 0; char *data = nullptr; };
struct Event { enum Kind { connected, disconnected, subscribed, frame, error } kind; int value = 0; uint32_t epoch = 0; };
struct Request {
    const char *name, *args;
    const unravel::ToolCallContext *call;
    std::string output;
    StaticSemaphore_t semaphore_storage;
    SemaphoreHandle_t done = nullptr;
};
class Actor {
public:
    explicit Actor(const Config &c) : config(c) {}
    ~Actor() { shutdown(); release(); }
    bool start(std::string &error) {
        frames = static_cast<Frame *>(heap_caps_calloc(4, sizeof(Frame), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        assembly = static_cast<char *>(heap_caps_malloc(protocol::max_frame + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!frames || !assembly) { error = "niko_frame_allocation_failed"; return false; }
        for (int i = 0; i < 4; ++i) {
            frames[i].data = static_cast<char *>(heap_caps_malloc(protocol::max_frame + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (!frames[i].data) { error = "niko_frame_allocation_failed"; return false; }
        }
        events = xQueueCreate(16, sizeof(Event)); free_frames = xQueueCreate(4, sizeof(int)); requests = xQueueCreate(2, sizeof(Request *));
        exited = xSemaphoreCreateBinary();
        if (!events || !free_frames || !requests || !exited) { error = "niko_queue_allocation_failed"; return false; }
        for (int i = 0; i < 4; ++i) xQueueSend(free_frames, &i, 0);
        fragments.reset(new protocol::Fragments(assembly));
        esp_mqtt_client_config_t c{};
        c.broker.address.hostname = config.controller.c_str();
        c.broker.address.port = 8884;
        c.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
        c.broker.verification.certificate = config.ca_pem.c_str();
        c.broker.verification.common_name = config.tls_server_name.c_str();
        c.broker.verification.skip_cert_common_name_check = false;
        c.credentials.username = config.username.c_str();
        c.credentials.authentication.password = config.jwt.c_str();
        c.session.disable_clean_session = false;
        c.network.timeout_ms = 1500;
        c.network.reconnect_timeout_ms = 5000;
        // Only the actor may reconnect. A connection cannot be replaced between
        // inventory admission and a command's synchronous publication.
        c.network.disable_auto_reconnect = true;
        c.buffer.size = 4096;
        c.buffer.out_size = 1024;
        c.task.stack_size = 6144;
        client = esp_mqtt_client_init(&c);
        if (!client || esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, callback, this) != ESP_OK) { error = "niko_mqtt_initialization_failed"; return false; }
        if (xTaskCreate(run_entry, "niko_actor", 8192, this, 4, &task) != pdPASS) { error = "niko_actor_start_failed"; return false; }
        return true;
    }
    void shutdown() {
        { std::lock_guard<std::mutex> lock(admission); stopping.store(true); }
        if (task) { xSemaphoreTake(exited, portMAX_DELAY); task = nullptr; }
        if (client) { esp_mqtt_client_destroy(client); client = nullptr; }
    }
    std::string status() {
        std::lock_guard<std::mutex> lock(mutex);
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "enabled", true);
        cJSON_AddBoolToObject(j, "configured", true);
        const bool current = connection_current();
        cJSON_AddBoolToObject(j, "authenticated", current && inventory.connected);
        cJSON_AddBoolToObject(j, "connected", current && inventory.connected);
        cJSON_AddBoolToObject(j, "inventoryReady", current && inventory.ready);
        cJSON_AddBoolToObject(j, "stale", !current || !inventory.connected || !inventory.ready);
        cJSON_AddBoolToObject(j, "exercised", control_exercised);
        cJSON_AddNumberToObject(j, "lastObservedMonotonicUs", double(inventory.last_observed_us));
        cJSON_AddNumberToObject(j, "deviceCount", double(inventory.devices.size()));
        cJSON_AddNumberToObject(j, "allowlistCount", double(config.allowed_lights.size()));
        cJSON_AddStringToObject(j, "writeScope", "Owner-enabled Niko dimmer actions only; relays, sockets and scenes read-only");
        uint64_t exp = 0; protocol::token_expiry(config.jwt, exp);
        uint64_t utc = uint64_t(std::time(nullptr));
        cJSON_AddNumberToObject(j, "tokenExpiresUtcSeconds", double(exp));
        cJSON_AddBoolToObject(j, "tokenExpired", utc >= exp);
        cJSON_AddStringToObject(j, "error", inventory.error.c_str());
        char *s = cJSON_PrintUnformatted(j); std::string result = s ? s : "{}"; cJSON_free(s); cJSON_Delete(j); return result;
    }
    std::string page(uint64_t after, size_t count) { std::lock_guard<std::mutex> lock(mutex); return inventory.page(after, count, {}, {}, connection_current()); }
    std::string execute(const char *name, const char *args, const unravel::ToolCallContext &call) {
        Request request{name, args, &call, {}, {}, nullptr};
        request.done = xSemaphoreCreateBinaryStatic(&request.semaphore_storage);
        Request *pointer = &request;
        // Admission and stop are ordered so the actor always replies before
        // the borrowed identity/request leaves the caller's stack.
        {
            std::lock_guard<std::mutex> lock(admission);
            if (stopping.load()) return error_json("niko_stopped");
            if (!request.done || xQueueSend(requests, &pointer, 0) != pdTRUE) return error_json("niko_request_pressure");
        }
        xSemaphoreTake(request.done, portMAX_DELAY);
        return std::move(request.output);
    }
private:
    Config config;
    Inventory inventory;
    bool control_exercised = false; // Matching controller evidence, not physical proof.
    std::mutex mutex;
    std::mutex admission;
    std::atomic<uint32_t> connection_epoch{0};
    uint32_t inventory_epoch = 0;
    bool reconnect_pending = false;
    uint64_t reconnect_after_us = 0;
    uint64_t assembly_received_us = 0;
    std::atomic<bool> stopping{false}, pressure{false}, transport_connected{false};
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t exited = nullptr;
    QueueHandle_t events = nullptr, free_frames = nullptr, requests = nullptr;
    esp_mqtt_client_handle_t client = nullptr;
    Frame *frames = nullptr;
    char *assembly = nullptr;
    std::unique_ptr<protocol::Fragments> fragments;
    int subscriptions[7]{};
    unsigned subscribed_mask = 0;
    bool mqtt_started = false;
    // Inventory is owned by this actor; external snapshots hold mutex.
    bool connection_current() const {
        return inventory_epoch == connection_epoch.load() && transport_connected.load() && !pressure.load();
    }
    void release() {
        if (frames) { for (int i = 0; i < 4; ++i) heap_caps_free(frames[i].data); heap_caps_free(frames); frames = nullptr; }
        heap_caps_free(assembly); assembly = nullptr;
        if (events) vQueueDelete(events);
        if (free_frames) vQueueDelete(free_frames);
        if (requests) vQueueDelete(requests);
        if (exited) vSemaphoreDelete(exited);
        events = free_frames = requests = nullptr; exited = nullptr;
    }
    void enqueue(Event::Kind kind, int value = 0) {
        Event e{kind, value, connection_epoch.load()}; if (xQueueSend(events, &e, 0) != pdTRUE) pressure.store(true);
    }
    static void callback(void *arg, esp_event_base_t, int32_t event_id, void *data) {
        auto &self = *static_cast<Actor *>(arg);
        if (self.stopping.load() || self.pressure.load()) return;
        auto *event = static_cast<esp_mqtt_event_handle_t>(data);
        switch (event_id) {
        case MQTT_EVENT_CONNECTED: ++self.connection_epoch; self.fragments->reset(); self.transport_connected.store(true); self.enqueue(Event::connected); break;
        case MQTT_EVENT_DISCONNECTED: ++self.connection_epoch; self.transport_connected.store(false); self.fragments->reset(); self.enqueue(Event::disconnected); break;
        case MQTT_EVENT_SUBSCRIBED: self.enqueue(Event::subscribed, event->msg_id); break;
        case MQTT_EVENT_ERROR: ++self.connection_epoch; self.transport_connected.store(false); self.fragments->reset(); self.enqueue(Event::error); break;
        case MQTT_EVENT_DATA: {
            if (event->current_data_offset == 0) self.assembly_received_us = clock_us();
            if (event->retain || event->data_len < 0 || event->current_data_offset < 0 || event->total_data_len <= 0 || event->topic_len < 0 || !self.fragments->append(event->topic, size_t(event->topic_len), event->data, size_t(event->data_len), size_t(event->current_data_offset), size_t(event->total_data_len))) { self.pressure.store(true); break; }
            if (!self.fragments->complete()) break;
            int index = -1;
            if (xQueueReceive(self.free_frames, &index, 0) != pdTRUE) { self.pressure.store(true); break; }
            Frame &f = self.frames[index]; f.length = self.fragments->size(); f.received_us = self.assembly_received_us;
            std::strcpy(f.topic, self.fragments->topic()); std::memcpy(f.data, self.fragments->data(), f.length + 1);
            self.enqueue(Event::frame, index); self.fragments->reset(); break;
        }
        default: break;
        }
    }
    int publish_read(const char *topic, const char *body) {
        if (!transport_connected.load() || stopping.load()) return -1;
        return esp_mqtt_client_publish(client, topic, body, int(std::strlen(body)), 0, 0);
    }
    void refresh() {
        if (subscribed_mask != 127) return;
        if (publish_read("hobby/control/devices/cmd", "{\"Method\":\"devices.list\"}") < 0 || publish_read("hobby/control/locations/cmd", "{\"Method\":\"locations.list\"}") < 0) inventory.invalidate("discovery_publish_failed");
    }
    void process(const Event &event) {
        std::lock_guard<std::mutex> lock(mutex);
        if (event.epoch != connection_epoch.load()) {
            if (event.kind == Event::frame) xQueueSend(free_frames, &event.value, 0);
            return;
        }
        if (event.kind == Event::connected) {
            inventory.disconnect(); inventory_epoch = event.epoch; reconnect_pending = false;
            inventory.connected = true; inventory.error.clear(); subscribed_mask = 0;
            for (size_t i = 0; i < 7; ++i) {
                subscriptions[i] = esp_mqtt_client_subscribe(client, topics[i], 0);
                if (subscriptions[i] < 0) { inventory.invalidate("subscription_failed"); pressure.store(true); }
            }
        } else if (event.kind == Event::disconnected || event.kind == Event::error) {
            inventory.disconnect(); inventory.error = event.kind == Event::error ? "mqtt_tls_or_authentication_error" : "controller_disconnected"; subscribed_mask = 0;
            reconnect_pending = true; reconnect_after_us = clock_us() + 5000000;
        } else if (event.kind == Event::subscribed) {
            for (size_t i = 0; i < 7; ++i) if (subscriptions[i] == event.value) subscribed_mask |= 1u << i;
            if (subscribed_mask == 127) refresh();
        } else if (event.kind == Event::frame) {
            Frame &f = frames[event.value];
            bool valid = inventory.ingest(f.topic, f.data, f.length, f.received_us);
            if (!valid && inventory.error != "inventory_refresh_required") pressure.store(true);
            if (inventory.error == "inventory_refresh_required") refresh();
            if (!std::strcmp(f.topic, "hobby/control/locations/evt")) refresh();
            xQueueSend(free_frames, &event.value, 0);
        }
    }
    void drain() { Event event{}; while (xQueueReceive(events, &event, 0) == pdTRUE) process(event); }
    bool unhealthy() {
        if (!pressure.load()) return false;
        std::lock_guard<std::mutex> lock(mutex); inventory.disconnect(); inventory.error = "mqtt_frame_message_or_inventory_pressure_stop_and_reconfigure"; return true;
    }
    void control(Request &request) {
        uint64_t deadline = clock_us() + 8000000;
        if (request.call->deadline_us && request.call->deadline_us < deadline) deadline = request.call->deadline_us;
        protocol::Desired desired; std::string wire, error; uint64_t baseline = 0; uint32_t epoch = 0;
        drain();
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!connection_current() || subscribed_mask != 127) { request.output = error_json("fresh_connection_inventory_required"); return; }
            if (!inventory.prepare(request.args, config.allowed_lights, *request.call, desired, wire, error)) { request.output = error_json(error.c_str(), request.call->is_cancelled() ? "cancelled" : "failed"); return; }
            baseline = inventory.sequence;
            epoch = inventory_epoch;
        }
        // No publish/outbox call on disconnected state. QoS0/non-retained writes
        // have no replay path; reconnect only issues read-only discovery.
        // Last authority/durable/expiry checks are directly adjacent to publish.
        uint64_t expiry = 0; protocol::token_expiry(config.jwt, expiry);
        if (!request.call->durable_started || request.call->is_cancelled() || request.call->is_expired() || stopping.load() || !connection_current() || connection_epoch.load() != epoch || uint64_t(std::time(nullptr)) < 1704067200 || uint64_t(std::time(nullptr)) >= expiry || clock_us() >= deadline) { request.output = error_json("authority_connection_or_deadline_changed_before_publish", request.call->is_cancelled() ? "cancelled" : "failed"); return; }
        const int sent = esp_mqtt_client_publish(client, "hobby/control/devices/cmd", wire.c_str(), int(wire.size()), 0, 0);
        // Exclude observations received before OR during synchronous publication.
        const uint64_t published_us = clock_us();
        desired.published_us = published_us;
        // Even a negative return can follow a partial socket write. Once handed
        // to MQTT there is no safe assertion that the controller did not act.
        bool cancelled = false; const char *reason = sent < 0 ? "publish_result_uncertain" : "fresh_observation_timeout";
        bool snapshot_requested = false;
        while (sent >= 0 && clock_us() < deadline) {
            if (request.call->is_cancelled() || request.call->is_expired() || stopping.load()) { cancelled = true; reason = "cancelled_after_publish_outcome_uncertain"; break; }
            if (pressure.load() || !transport_connected.load() || connection_epoch.load() != epoch) { reason = "connection_lost_after_publish_outcome_uncertain"; break; }
            Event event{}; if (xQueueReceive(events, &event, pdMS_TO_TICKS(20)) == pdTRUE) process(event);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (inventory.matched(desired, baseline, connection_current() && connection_epoch.load() == epoch)) break;
            }
            if (!snapshot_requested && clock_us() - published_us >= 4000000 && subscribed_mask == 127) {
                snapshot_requested = true;
                publish_read("hobby/control/devices/cmd", "{\"Method\":\"devices.list\"}");
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        if (request.call->is_cancelled() || request.call->is_expired() || stopping.load()) {
            cancelled = true; reason = "cancelled_after_publish_outcome_uncertain";
        }
        bool current = sent >= 0 && connection_current() && connection_epoch.load() == epoch;
        if (!current && sent >= 0) reason = "connection_lost_after_publish_outcome_uncertain";
        request.output = inventory.outcome(desired, baseline, true, cancelled, reason, current);
        // Classify against the epoch after encoding as well; never let queued
        // old-connection evidence turn a broken publication into success.
        current = current && connection_current() && connection_epoch.load() == epoch;
        if (!current) request.output = inventory.outcome(desired, baseline, true, cancelled, sent < 0 ? "publish_result_uncertain" : "connection_lost_after_publish_outcome_uncertain", false);
        if (current && !cancelled && inventory.matched(desired, baseline)) control_exercised = true;
    }
    void handle(Request &request) {
        if (stopping.load() || pressure.load()) request.output = error_json("niko_stopped_or_message_pressure");
        else if (request.call->is_cancelled() || request.call->is_expired()) request.output = error_json("call_cancelled_or_expired", "cancelled");
        else if (!std::strcmp(request.name, "niko_control")) control(request);
        else {
            std::lock_guard<std::mutex> lock(mutex);
            if (!std::strcmp(request.name, "niko_get_state")) request.output = inventory.get(request.args, connection_current());
            else {
                const char *args = request.args ? request.args : "{}";
                cJSON *j = std::strlen(args) <= 1024 ? cJSON_Parse(args) : nullptr;
                const cJSON *cursor = cJSON_GetObjectItemCaseSensitive(j, "cursor"), *count = cJSON_GetObjectItemCaseSensitive(j, "count");
                const cJSON *filter = cJSON_GetObjectItemCaseSensitive(j, "filter"), *location = cJSON_GetObjectItemCaseSensitive(j, "location");
                bool valid = cJSON_IsObject(j) && (!cursor || (cJSON_IsNumber(cursor) && cursor->valuedouble >= 0 && cursor->valuedouble <= protocol::max_devices && cursor->valuedouble == cursor->valueint)) && (!count || (cJSON_IsNumber(count) && count->valuedouble >= 1 && count->valuedouble <= 20 && count->valuedouble == count->valueint)) && (!filter || (cJSON_IsString(filter) && std::strlen(filter->valuestring) <= 96)) && (!location || (cJSON_IsString(location) && std::strlen(location->valuestring) <= 36));
                request.output = valid ? inventory.page(cursor ? uint64_t(cursor->valueint) : 0, count ? size_t(count->valueint) : 20, filter ? filter->valuestring : "", location ? location->valuestring : "", connection_current()) : error_json("invalid_page_arguments");
                cJSON_Delete(j);
            }
        }
        xSemaphoreGive(request.done);
    }
    static void run_entry(void *arg) { static_cast<Actor *>(arg)->run(); }
    void run() {
        if (esp_mqtt_client_start(client) == ESP_OK) mqtt_started = true;
        else { std::lock_guard<std::mutex> lock(mutex); inventory.error = "mqtt_start_failed"; stopping.store(true); }
        while (!stopping.load()) {
            if (unhealthy()) {
                if (mqtt_started) { esp_mqtt_client_stop(client); mqtt_started = false; }
            } else {
                uint64_t exp = 0; protocol::token_expiry(config.jwt, exp);
                if (uint64_t(std::time(nullptr)) >= exp) {
                    std::lock_guard<std::mutex> lock(mutex); inventory.disconnect(); inventory.error = "jwt_expired_renew_in_niko_programming_software"; pressure.store(true);
                } else { Event event{}; if (xQueueReceive(events, &event, pdMS_TO_TICKS(20)) == pdTRUE) process(event); }
            }
            Request *request = nullptr; if (xQueueReceive(requests, &request, 0) == pdTRUE) handle(*request);
            if (reconnect_pending && !transport_connected.load() && !pressure.load() && !stopping.load() && clock_us() >= reconnect_after_us) {
                // Commands have finished; replacement connections begin read-only.
                reconnect_after_us = clock_us() + 5000000;
                if (esp_mqtt_client_reconnect(client) == ESP_OK) reconnect_pending = false;
            }
            if (pressure.load()) vTaskDelay(pdMS_TO_TICKS(20));
        }
        { std::lock_guard<std::mutex> lock(admission); stopping.store(true); }
        transport_connected.store(false);
        if (mqtt_started) { esp_mqtt_client_stop(client); mqtt_started = false; }
        Request *request = nullptr;
        while (xQueueReceive(requests, &request, 0) == pdTRUE) { request->output = error_json("niko_stopped", "cancelled"); xSemaphoreGive(request->done); }
        { std::lock_guard<std::mutex> lock(mutex); inventory.disconnect(); }
        xSemaphoreGive(exited); vTaskDelete(nullptr);
    }
};
std::mutex lifecycle;
std::mutex reconfiguration;
std::shared_ptr<Actor> active;
std::string configuration_error;
void stop_actor() {
    std::shared_ptr<Actor> old;
    { std::lock_guard<std::mutex> lock(lifecycle); old = std::move(active); }
    if (old) old->shutdown();
}
bool execute_tool(const char *args, std::string &output, const unravel::ToolCallContext &call, void *context) {
    std::shared_ptr<Actor> actor;
    { std::lock_guard<std::mutex> lock(lifecycle); actor = active; }
    output = actor ? actor->execute(static_cast<const char *>(context), args, call) : error_json("niko_not_enabled");
    return true; // Domain state lives in the result; SDK normalizes failed/unknown.
}
}
bool configure(const Config &config, std::string &error) {
    std::lock_guard<std::mutex> serial(reconfiguration);
    if (!protocol::config_valid(config, uint64_t(std::time(nullptr)), error)) return false;
    stop_actor();
    if (!config.enabled) {
        std::lock_guard<std::mutex> lock(lifecycle);
        configuration_error.clear(); error.clear(); return true;
    }
    auto next = std::make_shared<Actor>(config);
    if (!next->start(error)) { std::lock_guard<std::mutex> lock(lifecycle); configuration_error = error; return false; }
    { std::lock_guard<std::mutex> lock(lifecycle); active = std::move(next); configuration_error.clear(); }
    error.clear(); return true;
}
void stop() {
    std::lock_guard<std::mutex> serial(reconfiguration);
    stop_actor();
}
std::string status_json() {
    std::shared_ptr<Actor> actor; std::string error;
    { std::lock_guard<std::mutex> lock(lifecycle); actor = active; error = configuration_error; }
    if (actor) return actor->status();
    cJSON *j = cJSON_CreateObject(); cJSON_AddBoolToObject(j, "enabled", false); cJSON_AddBoolToObject(j, "configured", false); cJSON_AddBoolToObject(j, "authenticated", false); cJSON_AddBoolToObject(j, "connected", false); cJSON_AddBoolToObject(j, "inventoryReady", false); cJSON_AddBoolToObject(j, "exercised", false); cJSON_AddStringToObject(j, "error", error.c_str());
    cJSON_AddBoolToObject(j, "stale", true);
    cJSON_AddNumberToObject(j, "lastObservedMonotonicUs", 0);
    cJSON_AddNumberToObject(j, "deviceCount", 0);
    cJSON_AddNumberToObject(j, "allowlistCount", 0);
    cJSON_AddNumberToObject(j, "tokenExpiresUtcSeconds", 0);
    cJSON_AddBoolToObject(j, "tokenExpired", false);
    cJSON_AddStringToObject(j, "writeScope", "Owner-enabled Niko dimmer actions only; relays, sockets and scenes read-only");
    char *s = cJSON_PrintUnformatted(j); std::string result = s ? s : "{}"; cJSON_free(s); cJSON_Delete(j); return result;
}
std::string inventory_json(uint64_t after, size_t count) {
    std::shared_ptr<Actor> actor; { std::lock_guard<std::mutex> lock(lifecycle); actor = active; }
    return actor ? actor->page(after, count) :
        "{\"ok\":false,\"stale\":true,\"error\":\"niko_not_enabled\",\"devices\":[],\"nextCursor\":null,\"inventoryRevision\":0}";
}
bool register_tools(unravel::Agent &agent) {
    unravel::Tool list;
    list.name = "niko_list_devices"; list.description = "Read bounded Niko inventory. Stale/offline is labeled. Only dimmer actions may be owner-enabled for writes; relay/socket/scene classes are read-only. Select discovered IDs; do not guess names.";
    list.parameters_json = R"({"type":"object","properties":{"filter":{"type":"string","maxLength":96},"location":{"type":"string","maxLength":36},"cursor":{"type":"integer","minimum":0,"maximum":64},"count":{"type":"integer","minimum":1,"maximum":20}},"additionalProperties":false})";
    list.execute = execute_tool; list.context = const_cast<char *>("niko_list_devices");
    if (!agent.add_tool(std::move(list))) return false;
    unravel::Tool state;
    state.name = "niko_get_state"; state.description = "Read last controller-observed state for one to four discovered device UUIDs. Offline state is stale, not current physical truth.";
    state.parameters_json = R"({"type":"object","properties":{"device_ids":{"type":"array","minItems":1,"maxItems":4,"items":{"type":"string","maxLength":36}}},"required":["device_ids"],"additionalProperties":false})";
    state.execute = execute_tool; state.context = const_cast<char *>("niko_get_state");
    if (!agent.add_tool(std::move(state))) return false;
    unravel::Tool control;
    control.name = "niko_control"; control.description = "Set one explicitly owner-enabled, discovered Niko dimmer action's Status On/Off and/or absolute brightness percentage. Requires durable call ledger. Brightness 0 does not turn Status Off. Waits for fresh controller evidence; partial/unknown is never success. No toggle, relay, socket, motor, scene or replay.";
    control.parameters_json = R"({"type":"object","properties":{"device_id":{"type":"string","maxLength":36},"status":{"type":"string","enum":["On","Off"]},"brightness":{"type":"integer","minimum":0,"maximum":100}},"required":["device_id"],"additionalProperties":false})";
    control.execute = execute_tool; control.context = const_cast<char *>("niko_control"); control.requires_durable = true;
    return agent.add_tool(std::move(control));
}
} // namespace niko_home_control
