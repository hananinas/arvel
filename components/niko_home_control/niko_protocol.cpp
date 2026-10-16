#include "niko_protocol.hpp"
#include "cJSON.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace niko_home_control::protocol {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
Json parse(const char *s, size_t n) {
    if (!s || !n || n > max_frame || std::memchr(s, 0, n)) return {nullptr, cJSON_Delete};
    // Cap parser recursion and token allocation before invoking cJSON. Array
    // elements/commas are counted even when otherwise empty or malformed.
    unsigned depth = 0, tokens = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') { quoted = true; ++tokens; }
        else if (c == '{' || c == '[') { if (++depth > 12) return {nullptr, cJSON_Delete}; ++tokens; }
        else if (c == '}' || c == ']') { if (!depth) return {nullptr, cJSON_Delete}; --depth; }
        else if (c == ',' || c == ':') ++tokens;
        if (tokens > 4096) return {nullptr, cJSON_Delete};
    }
    if (quoted || depth) return {nullptr, cJSON_Delete};
    const char *end = nullptr;
    Json j(cJSON_ParseWithLengthOpts(s, n, &end, false), cJSON_Delete);
    if (!j || !end) return {nullptr, cJSON_Delete};
    while (end < s + n && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    if (end != s + n) j.reset();
    return j;
}
const cJSON *field(const cJSON *j, const char *key) { return cJSON_GetObjectItemCaseSensitive(j, key); }
std::string text(const cJSON *j, size_t max = 128) {
    return cJSON_IsString(j) && j->valuestring && std::strlen(j->valuestring) <= max ? j->valuestring : "";
}
std::string text(const cJSON *j, const char *key, size_t max = 128) { return text(field(j, key), max); }
bool flag(const cJSON *j) { return cJSON_IsTrue(j) || text(j, 5) == "true" || text(j, 5) == "True"; }
bool id_ok(const std::string &s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    }
    return true;
}
std::string encode(cJSON *j) {
    char *raw = cJSON_PrintUnformatted(j);
    std::string s = raw ? raw : "{\"ok\":false,\"state\":\"failed\",\"error\":\"allocation_failed\"}";
    cJSON_free(raw);
    return s;
}
std::string fail(const char *reason) {
    Json j(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddBoolToObject(j.get(), "ok", false);
    cJSON_AddStringToObject(j.get(), "state", "failed");
    cJSON_AddStringToObject(j.get(), "error", reason);
    return encode(j.get());
}
Property *ensure_property(Device &d, const char *name) {
    for (auto &p : d.properties) if (p.name == name) return &p;
    if (d.properties.size() >= max_properties) return nullptr;
    d.properties.push_back({}); d.properties.back().name = name; return &d.properties.back();
}
const Property *property(const Device &d, const char *name) {
    for (const auto &p : d.properties) if (p.name == name) return &p;
    return nullptr;
}
bool entries(Device &d, const cJSON *array, bool definitions, uint64_t seq, uint64_t now) {
    if (!array) return true;
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > int(max_properties)) return false;
    const cJSON *entry = nullptr;
    cJSON_ArrayForEach(entry, array) {
        if (!cJSON_IsObject(entry) || !entry->child || entry->child->next) return false;
        const cJSON *v = entry->child;
        if (!v->string || std::strlen(v->string) > 64) return false;
        Property *p = ensure_property(d, v->string); if (!p) return false;
        if (definitions) {
            if (!cJSON_IsObject(v)) return false;
            p->description = text(v, "Description", 96);
            p->status = flag(field(v, "HasStatus")); p->control = flag(field(v, "CanControl"));
        } else {
            if (!cJSON_IsString(v) || std::strlen(v->valuestring) > 128) return false;
            p->value = v->valuestring; p->sequence = seq; p->observed_us = now;
        }
    }
    return true;
}
cJSON *projection(const Device &d, const std::vector<std::string> *allow = nullptr) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", d.id.c_str()); cJSON_AddStringToObject(j, "name", d.name.c_str());
    cJSON_AddStringToObject(j, "type", d.type.c_str()); cJSON_AddStringToObject(j, "technology", d.technology.c_str());
    cJSON_AddStringToObject(j, "model", d.model.c_str()); cJSON_AddStringToObject(j, "locationId", d.location.c_str());
    cJSON_AddStringToObject(j, "locationName", d.location_name.c_str());
    cJSON_AddBoolToObject(j, "online", d.online); cJSON_AddBoolToObject(j, "safeLightingClass", d.lighting());
    if (allow) cJSON_AddBoolToObject(j, "ownerEnabled", std::find(allow->begin(), allow->end(), d.id) != allow->end());
    cJSON *props = cJSON_AddArrayToObject(j, "properties");
    for (const auto &p : d.properties) {
        cJSON *q = cJSON_CreateObject(); cJSON_AddItemToArray(props, q);
        cJSON_AddStringToObject(q, "name", p.name.c_str()); cJSON_AddStringToObject(q, "value", p.value.c_str());
        cJSON_AddStringToObject(q, "definition", p.description.c_str());
        cJSON_AddBoolToObject(q, "hasStatus", p.status); cJSON_AddBoolToObject(q, "canControl", p.control);
        cJSON_AddNumberToObject(q, "observedMonotonicUs", double(p.observed_us));
    }
    return j;
}
bool range_allows(const std::string &s, int value) {
    double lo = 0, hi = 0, step = 0; int used = 0;
    if (std::sscanf(s.c_str(), "Range(%lf,%lf,%lf)%n", &lo, &hi, &step, &used) != 3 || size_t(used) != s.size()) return false;
    if (!std::isfinite(lo) || !std::isfinite(hi) || !std::isfinite(step) || lo < 0 || hi > 100 || hi < lo || step <= 0) return false;
    double k = (value - lo) / step;
    return value >= lo && value <= hi && std::fabs(k - std::round(k)) < 0.000001;
}
bool value_matches(const char *name, const std::string &observed, const std::string &requested) {
    if (std::strcmp(name, "Brightness")) return observed == requested;
    double actual = 0; int used = 0;
    return std::sscanf(observed.c_str(), "%lf%n", &actual, &used) == 1 &&
        size_t(used) == observed.size() && std::isfinite(actual) &&
        actual == std::atoi(requested.c_str());
}
}
bool Device::lighting() const {
    // Niko v1.4 Annex A pp.41-42: dimmer is a dimmer action, relay (p.44)
    // is only a relay/circuit. Neither a name nor an allowlist proves a light.
    return type == "action" && technology == "nikohomecontrol" && model == "dimmer";
}
void Inventory::disconnect() { connected = false; ready = false; }
void Inventory::invalidate(const char *reason) { ready = false; error = reason; }
const Device *Inventory::find(const std::string &id) const { for (const auto &d : devices) if (d.id == id) return &d; return nullptr; }
bool Inventory::ingest(const char *topic, const char *json, size_t length, uint64_t now) {
    auto root = parse(json, length);
    if (!root || !cJSON_IsObject(root.get())) { invalidate("malformed_controller_message"); return false; }
    const std::string method = text(root.get(), "Method", 64);
    const bool device_rsp = !std::strcmp(topic, "hobby/control/devices/rsp");
    const bool device_evt = !std::strcmp(topic, "hobby/control/devices/evt");
    const bool location_rsp = !std::strcmp(topic, "hobby/control/locations/rsp");
    if ((device_rsp && method == "devices.list") || (device_evt && method == "devices.status") || (location_rsp && method == "locations.list")) {
        const cJSON *params = field(root.get(), "Params");
        if (!cJSON_IsArray(params) || cJSON_GetArraySize(params) != 1) { invalidate("invalid_controller_params"); return false; }
        const cJSON *list = field(cJSON_GetArrayItem(params, 0), location_rsp ? "Locations" : "Devices");
        if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > int(location_rsp ? max_locations : max_devices)) { invalidate("inventory_capacity_exceeded"); return false; }
        const uint64_t seq = ++sequence;
        if (location_rsp) {
            std::vector<Location> updated; updated.reserve(cJSON_GetArraySize(list));
            const cJSON *v = nullptr;
            cJSON_ArrayForEach(v, list) {
                Location l{text(v, "Uuid", 36), text(v, "Name", 96)};
                if (!id_ok(l.id)) { invalidate("invalid_location"); return false; }
                updated.push_back(std::move(l));
            }
            locations = std::move(updated); return true;
        }
        std::vector<Device> replacement;
        replacement.reserve(cJSON_GetArraySize(list));
        const cJSON *v = nullptr;
        cJSON_ArrayForEach(v, list) {
            std::string id = text(v, "Uuid", 36);
            if (!id_ok(id)) { invalidate("invalid_device_id"); return false; }
            if (std::any_of(replacement.begin(), replacement.end(), [&](const Device &other) { return other.id == id; })) { invalidate("duplicate_device_id"); return false; }
            Device d;
            if (device_evt) { const Device *old = find(id); if (!old) { invalidate("inventory_refresh_required"); return false; } d = *old; }
            d.id = id;
            if (device_rsp) {
                d.name = text(v, "Name", 96); d.type = text(v, "Type", 48); d.technology = text(v, "Technology", 48); d.model = text(v, "Model", 48);
                if (d.type.empty() || d.technology.empty() || d.model.empty()) { invalidate("invalid_device_descriptor"); return false; }
                const cJSON *parameters = field(v, "Parameters");
                if (parameters && (!cJSON_IsArray(parameters) || cJSON_GetArraySize(parameters) > 32)) { invalidate("parameter_capacity_exceeded"); return false; }
                const cJSON *p = nullptr;
                cJSON_ArrayForEach(p, parameters) {
                    if (field(p, "LocationId")) d.location = text(p, "LocationId", 36);
                    if (field(p, "LocationName")) d.location_name = text(p, "LocationName", 96);
                }
            }
            if (field(v, "Online")) d.online = flag(field(v, "Online"));
            if (!entries(d, field(v, "PropertyDefinitions"), true, seq, now) || !entries(d, field(v, "Properties"), false, seq, now)) { invalidate("invalid_property_definition_or_capacity"); return false; }
            replacement.push_back(std::move(d));
        }
        if (device_rsp) { devices = std::move(replacement); ready = connected; error.clear(); }
        else for (auto &updated : replacement)
            for (auto &existing : devices)
                if (existing.id == updated.id) { existing = std::move(updated); break; }
        last_observed_us = now; return true;
    }
    if (device_evt && (method == "devices.added" || method == "devices.removed" || method == "devices.changed" || method == "devices.param_changed" || method == "devices.displayname_changed")) { invalidate("inventory_refresh_required"); return true; }
    if (method.empty()) { invalidate("missing_controller_method"); return false; }
    // Only expose a static error category, never vendor bodies (may echo secrets).
    if (std::strstr(topic, "/err")) { error = method == "devices.control" ? "controller_control_error" : "controller_read_error"; return true; }
    return true;
}
bool Inventory::prepare(const char *args, const std::vector<std::string> &allow,
                        const unravel::ToolCallContext &call, Desired &d, std::string &wire, std::string &failure) const {
    auto reject = [&](const char *why) { failure = why; return false; };
    if (!call.durable_started) return reject("durable_storage_required");
    if (call.is_cancelled()) return reject("cancelled_before_publish");
    if (call.is_expired()) return reject("deadline_before_publish");
    if (!connected || !ready) return reject("fresh_inventory_required");
    if (!args || std::strlen(args) > 1024) return reject("invalid_arguments");
    auto j = parse(args, std::strlen(args)); if (!j || !cJSON_IsObject(j.get())) return reject("invalid_arguments");
    bool seen_id = false, seen_status = false, seen_brightness = false;
    const cJSON *v = nullptr;
    cJSON_ArrayForEach(v, j.get()) {
        if (!v->string) return reject("invalid_arguments");
        if (!std::strcmp(v->string, "device_id") && !seen_id) { seen_id = true; d.id = text(v, 36); }
        else if (!std::strcmp(v->string, "status") && !seen_status) { seen_status = true; d.status = text(v, 3); }
        else if (!std::strcmp(v->string, "brightness") && !seen_brightness) {
            seen_brightness = true;
            if (!cJSON_IsNumber(v) || !std::isfinite(v->valuedouble) || v->valuedouble < 0 || v->valuedouble > 100 || std::floor(v->valuedouble) != v->valuedouble) return reject("invalid_absolute_brightness");
            d.has_brightness = true; d.brightness = int(v->valuedouble);
        } else return reject("unknown_or_duplicate_argument");
    }
    if (!id_ok(d.id) || (!seen_status && !seen_brightness)) return reject("device_id_and_desired_property_required");
    const Device *target = find(d.id); if (!target) return reject("undiscovered_device");
    if (!target->lighting()) return reject("unsupported_actuator_class");
    if (std::find(allow.begin(), allow.end(), d.id) == allow.end()) return reject("owner_allowlist_required");
    if (!target->online) return reject("device_offline");
    if (seen_status) {
        const Property *p = property(*target, "Status");
        if ((d.status != "On" && d.status != "Off") || !p || !p->status || !p->control || (p->description != "Choice(On,Off)" && p->description != "Choice(On, Off)")) return reject("unsupported_status_definition");
    }
    if (seen_brightness) {
        const Property *p = property(*target, "Brightness");
        if (!p || !p->status || !p->control || !range_allows(p->description, d.brightness)) return reject("unsupported_brightness_definition");
    }
    Json root(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(root.get(), "Method", "devices.control");
    cJSON *params = cJSON_AddArrayToObject(root.get(), "Params"), *group = cJSON_CreateObject(); cJSON_AddItemToArray(params, group);
    cJSON *devices = cJSON_AddArrayToObject(group, "Devices"), *device = cJSON_CreateObject(); cJSON_AddItemToArray(devices, device);
    cJSON_AddStringToObject(device, "Uuid", d.id.c_str()); cJSON *props = cJSON_AddArrayToObject(device, "Properties");
    if (seen_status) { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "Status", d.status.c_str()); cJSON_AddItemToArray(props, p); }
    if (seen_brightness) { cJSON *p = cJSON_CreateObject(); cJSON_AddStringToObject(p, "Brightness", std::to_string(d.brightness).c_str()); cJSON_AddItemToArray(props, p); }
    wire = encode(root.get()); return true;
}
std::string Inventory::page(uint64_t after, size_t count, const std::string &filter, const std::string &location, bool observation_current) const {
    count = std::min<size_t>(count, 20);
    if (after > devices.size() || !count || filter.size() > 96 || location.size() > 36) {
        Json rejected(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddBoolToObject(rejected.get(), "ok", false);
        cJSON_AddBoolToObject(rejected.get(), "stale", !connected || !ready);
        cJSON_AddStringToObject(rejected.get(), "error", "invalid_or_obsolete_page_cursor");
        cJSON_AddArrayToObject(rejected.get(), "devices");
        cJSON_AddNullToObject(rejected.get(), "nextCursor");
        cJSON_AddNumberToObject(rejected.get(), "inventoryRevision", double(sequence));
        return encode(rejected.get());
    }
    Json j(cJSON_CreateObject(), cJSON_Delete); cJSON_AddBoolToObject(j.get(), "ok", true);
    cJSON_AddBoolToObject(j.get(), "stale", !observation_current || !connected || !ready); cJSON_AddStringToObject(j.get(), "error", error.c_str());
    cJSON *list = cJSON_AddArrayToObject(j.get(), "devices"); size_t i = size_t(after), emitted = 0;
    size_t encoded_size = encode(j.get()).size() + 96;
    // Limit encoded output by complete device, not by truncating JSON/text.
    for (; i < devices.size(); ++i) {
        const auto &d = devices[i];
        if ((!filter.empty() && d.name.find(filter) == std::string::npos) || (!location.empty() && d.location != location)) continue;
        cJSON *p = projection(d);
        std::string encoded = encode(p);
        if (encoded_size + encoded.size() + 1 > 8000) { cJSON_Delete(p); break; }
        encoded_size += encoded.size() + 1;
        cJSON_AddItemToArray(list, p);
        if (++emitted == count) { ++i; break; }
    }
    if (i < devices.size()) cJSON_AddNumberToObject(j.get(), "nextCursor", double(i)); else cJSON_AddNullToObject(j.get(), "nextCursor");
    cJSON_AddNumberToObject(j.get(), "inventoryRevision", double(sequence));
    return encode(j.get());
}
std::string Inventory::get(const char *args, bool observation_current) const {
    if (!args || std::strlen(args) > 1024) return fail("invalid_arguments");
    auto j = parse(args, std::strlen(args)); const cJSON *ids = field(j.get(), "device_ids");
    if (!cJSON_IsArray(ids) || cJSON_GetArraySize(ids) < 1 || cJSON_GetArraySize(ids) > 4) return fail("one_to_four_discovered_ids_required");
    Json out(cJSON_CreateObject(), cJSON_Delete); cJSON_AddBoolToObject(out.get(), "ok", true);
    cJSON_AddBoolToObject(out.get(), "stale", !observation_current || !connected || !ready); cJSON *list = cJSON_AddArrayToObject(out.get(), "devices");
    const cJSON *v = nullptr;
    cJSON_ArrayForEach(v, ids) { const Device *d = find(text(v, 36)); if (!d) return fail("undiscovered_device"); cJSON_AddItemToArray(list, projection(*d)); }
    std::string result = encode(out.get()); if (result.size() > 8192) return fail("result_capacity_exceeded_use_smaller_id_list"); return result;
}
bool Inventory::matched(const Desired &d, uint64_t baseline, bool observation_current) const {
    const Device *target = find(d.id); if (!observation_current || !target || !target->online || !connected || !ready) return false;
    auto match = [&](const char *name, const std::string &value) {
        const Property *p = property(*target, name);
        return p && p->sequence > baseline && p->observed_us > d.published_us &&
            value_matches(name, p->value, value);
    };
    return (d.status.empty() || match("Status", d.status)) && (!d.has_brightness || match("Brightness", std::to_string(d.brightness)));
}
std::string Inventory::outcome(const Desired &d, uint64_t baseline, bool published, bool cancelled, const char *reason, bool observation_current) const {
    const Device *target = find(d.id); bool any_match = false, all_match = observation_current && connected && ready;
    Json j(cJSON_CreateObject(), cJSON_Delete); cJSON_AddStringToObject(j.get(), "deviceId", d.id.c_str());
    cJSON *requested = cJSON_AddObjectToObject(j.get(), "requested"), *observed = cJSON_AddObjectToObject(j.get(), "observed");
    auto add = [&](const char *name, const std::string &value) {
        cJSON_AddStringToObject(requested, name, value.c_str()); const Property *p = target ? property(*target, name) : nullptr;
        cJSON *q = cJSON_AddObjectToObject(observed, name);
        bool fresh = observation_current && connected && ready && p && target->online && p->sequence > baseline && p->observed_us > d.published_us;
        cJSON_AddBoolToObject(q, "fresh", fresh); if (p) cJSON_AddStringToObject(q, "value", p->value.c_str()); else cJSON_AddNullToObject(q, "value");
        cJSON_AddNumberToObject(q, "observedMonotonicUs", p ? double(p->observed_us) : 0);
        bool matches = fresh && value_matches(name, p->value, value);
        any_match |= matches; all_match &= matches;
    };
    if (!d.status.empty()) add("Status", d.status);
    if (d.has_brightness) add("Brightness", std::to_string(d.brightness));
    const char *state = !published ? (cancelled ? "cancelled" : "failed") : cancelled ? "unknown" : all_match ? "succeeded" : any_match ? "partial" : "unknown";
    cJSON_AddStringToObject(j.get(), "state", state); cJSON_AddBoolToObject(j.get(), "ok", !std::strcmp(state, "succeeded"));
    cJSON_AddBoolToObject(j.get(), "published", published); cJSON_AddStringToObject(j.get(), "evidence", "Controller observation only; not causal or physical proof.");
    if (std::strcmp(state, "succeeded")) cJSON_AddStringToObject(j.get(), "error", reason);
    return encode(j.get());
}
void Fragments::reset() { used_ = total_ = 0; topic_.fill(0); }
bool Fragments::append(const char *topic, size_t topic_length, const char *data, size_t length, size_t offset, size_t total) {
    if (!total || total > max_frame || !data || length > total || offset > total - length) { reset(); return false; }
    if (offset == 0) {
        if (total_ && used_ != total_) { reset(); return false; }
        reset(); if (!topic || !topic_length || topic_length >= topic_.size()) return false;
        std::memcpy(topic_.data(), topic, topic_length); total_ = total;
    }
    if (offset != used_ || total != total_ || !total_) { reset(); return false; }
    std::memcpy(storage_ + used_, data, length); used_ += length;
    if (complete()) storage_[used_] = 0;
    return true;
}
bool token_expiry(const std::string &jwt, uint64_t &expiry) {
    const size_t dot = jwt.find('.'), second = dot == std::string::npos ? dot : jwt.find('.', dot + 1);
    if (dot == std::string::npos || second == std::string::npos || jwt.find('.', second + 1) != std::string::npos || second - dot > 2048) return false;
    std::string decoded; decoded.reserve(1536); unsigned acc = 0, bits = 0;
    for (size_t i = dot + 1; i < second; ++i) {
        char c = jwt[i]; int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '-' ? 62 : c == '_' ? 63 : -1;
        if (v < 0) return false;
        acc = (acc << 6) | unsigned(v); bits += 6;
        if (bits >= 8) { bits -= 8; decoded.push_back(char((acc >> bits) & 255)); }
    }
    auto j = parse(decoded.data(), decoded.size()); const cJSON *e = field(j.get(), "exp");
    if (!cJSON_IsNumber(e) || !std::isfinite(e->valuedouble) || e->valuedouble < 1 || e->valuedouble > 9007199254740991.0 || std::floor(e->valuedouble) != e->valuedouble) return false;
    expiry = uint64_t(e->valuedouble); return true;
}
bool config_valid(const Config &c, uint64_t utc, std::string &error) {
    if (!c.enabled) return true;
    auto hostname = [](const std::string &s) { return !s.empty() && s.size() <= 253 && std::all_of(s.begin(), s.end(), [](unsigned char v) { return (v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z') || (v >= '0' && v <= '9') || v == '.' || v == '-'; }); };
    if (!hostname(c.controller) || !hostname(c.tls_server_name)) { error = "controller_and_verified_tls_identity_required"; return false; }
    if (c.username.empty() || c.username.size() > 128 || c.jwt.empty() || c.jwt.size() > 2048 || c.ca_pem.size() > 8192 || c.ca_pem.find("-----BEGIN CERTIFICATE-----") == std::string::npos) { error = "username_jwt_and_owner_ca_required"; return false; }
    uint64_t expiry = 0;
    if (!token_expiry(c.jwt, expiry)) { error = "jwt_expiry_required"; return false; }
    if (utc < 1704067200) { error = "trusted_clock_required_for_tls_and_expiry"; return false; }
    if (utc >= expiry) { error = "jwt_expired_renew_in_niko_programming_software"; return false; }
    if (c.allowed_lights.size() > 16 || !std::all_of(c.allowed_lights.begin(), c.allowed_lights.end(), id_ok)) { error = "invalid_light_allowlist"; return false; }
    return true;
}
} // namespace niko_home_control::protocol
