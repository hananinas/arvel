#include "device_tools.hpp"
#include "adc_bsp.h"
#include "ai_chat_bsp.h"
#include "arvel_art.h"
#include "buddy_settings.hpp"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
constexpr size_t MAX_NETWORKS = 12;

DeviceSettingsApplier apply_settings = nullptr;

// A tool argument backed by a device setting: its patch group and key.
struct Field {
    const char *arg;
    const char *group;  // "preferences" or "buddy"
    const char *key;
};
const Field DISPLAY_FIELDS[] = {
    {"brightness", "preferences", "brightness"}, {"dim_at_night", "preferences", "dimAtNight"},
    {"screen_sleep", "preferences", "screenSleep"}, {"scene", "buddy", "scene"}};
const Field SOUND_FIELDS[] = {
    {"volume", "preferences", "volume"}, {"voice", "preferences", "voiceStyle"},
    {"speaking_speed", "preferences", "speechRate"}, {"read_aloud", "buddy", "readAloud"},
    {"captions", "preferences", "captions"}};
const Field PROFILE_FIELDS[] = {
    {"personality", "preferences", "persona"}, {"user_name", "preferences", "userName"},
    {"timezone", "preferences", "timezone"}};

struct Spec;
using Runner = bool (*)(const Spec &spec, const cJSON *args, cJSON *out, const unravel::ToolCallContext &call);
struct Spec {
    const char *name;
    buddy_ui_tool_kind_t kind;
    const char *verb;
    const char *target;  // fixed on-screen target, or nullptr to summarize arguments
    const char *description;
    Runner run;
    const Field *fields;
    size_t field_count;
};

bool error(cJSON *out, const char *message)
{
    return cJSON_AddStringToObject(out, "error", message) != nullptr;
}

bool add_string_enum(cJSON *property, const char *const *names, size_t count)
{
    Json values(cJSON_CreateArray(), cJSON_Delete);
    if (!values) return false;
    for (size_t i = 0; i < count; ++i) {
        Json value(cJSON_CreateString(names[i]), cJSON_Delete);
        if (!value || !cJSON_AddItemToArray(values.get(), value.get())) return false;
        value.release();
    }
    if (!cJSON_AddItemToObject(property, "enum", values.get())) return false;
    values.release();
    return true;
}

// JSON schema property for one settings-backed argument.
cJSON *field_schema(const char *arg)
{
    cJSON *p = cJSON_CreateObject();
    if (!p) return nullptr;
    bool ok = true;
    auto describe = [p, &ok](const char *type, const char *text) {
        ok = cJSON_AddStringToObject(p, "type", type) && ok;
        ok = cJSON_AddStringToObject(p, "description", text) && ok;
    };
    if (!strcmp(arg, "brightness")) {
        describe("integer", "Screen brightness in percent, 10 to 100 in steps of 10.");
        ok = cJSON_AddNumberToObject(p, "minimum", 10) && ok;
        ok = cJSON_AddNumberToObject(p, "maximum", 100) && ok;
        ok = cJSON_AddNumberToObject(p, "multipleOf", 10) && ok;
    } else if (!strcmp(arg, "dim_at_night")) {
        describe("boolean", "Dim the screen to a third of its brightness from 22:00 to 07:00.");
    } else if (!strcmp(arg, "screen_sleep")) {
        describe("string", "Turn the screen off after this long without activity.");
        ok = add_string_enum(p, buddy_screen_sleep_names, 3) && ok;
    } else if (!strcmp(arg, "scene")) {
        describe("string", "Background scene the buddy sits in.");
        const char *ids[ARVEL_SCENE_COUNT];
        for (size_t i = 0; i < ARVEL_SCENE_COUNT; ++i) ids[i] = arvel_scenes[i].id;
        ok = add_string_enum(p, ids, ARVEL_SCENE_COUNT) && ok;
    } else if (!strcmp(arg, "volume")) {
        describe("integer", "Speaker volume in percent, 0 to 100 in steps of 10.");
        ok = cJSON_AddNumberToObject(p, "minimum", 0) && ok;
        ok = cJSON_AddNumberToObject(p, "maximum", 100) && ok;
        ok = cJSON_AddNumberToObject(p, "multipleOf", 10) && ok;
    } else if (!strcmp(arg, "voice")) {
        describe("string", "Speaking voice style.");
        ok = add_string_enum(p, buddy_voice_style_names, 3) && ok;
    } else if (!strcmp(arg, "speaking_speed")) {
        describe("number", "Speaking speed as a multiple of normal, 0.5 to 2.0 in steps of 0.1.");
        ok = cJSON_AddNumberToObject(p, "minimum", 0.5) && ok;
        ok = cJSON_AddNumberToObject(p, "maximum", 2.0) && ok;
    } else if (!strcmp(arg, "read_aloud")) {
        describe("boolean", "Read typed-chat replies aloud by default.");
    } else if (!strcmp(arg, "captions")) {
        describe("boolean", "Show live captions while speaking.");
    } else if (!strcmp(arg, "personality")) {
        describe("string", "The buddy's personality.");
        ok = add_string_enum(p, buddy_persona_names, 3) && ok;
    } else if (!strcmp(arg, "user_name")) {
        describe("string", "The user's name, used in greetings (at most 32 bytes; empty to clear).");
    } else if (!strcmp(arg, "timezone")) {
        describe("string", "Local time zone as a POSIX TZ string, e.g. CET-1CEST,M3.5.0,M10.5.0/3.");
    }
    if (!ok) { cJSON_Delete(p); return nullptr; }
    return p;
}

cJSON *object_schema(const Spec &spec)
{
    Json schema(cJSON_CreateObject(), cJSON_Delete);
    cJSON *props = schema ? cJSON_AddObjectToObject(schema.get(), "properties") : nullptr;
    if (!props || !cJSON_AddStringToObject(schema.get(), "type", "object")) return nullptr;
    for (size_t i = 0; i < spec.field_count; ++i) {
        Json property(field_schema(spec.fields[i].arg), cJSON_Delete);
        if (!property || !cJSON_AddItemToObject(props, spec.fields[i].arg, property.get())) return nullptr;
        property.release();
    }
    if (!cJSON_AddBoolToObject(schema.get(), "additionalProperties", false)) return nullptr;
    return schema.release();
}

// ---------------------------------------------------------------- runners

bool run_settings(const Spec &spec, const cJSON *args, cJSON *out, const unravel::ToolCallContext &call)
{
    Json patch(cJSON_CreateObject(), cJSON_Delete);
    Json applied(cJSON_CreateObject(), cJSON_Delete);
    if (!patch || !applied) return false;
    size_t count = 0;
    for (const cJSON *arg = args ? args->child : nullptr; arg; arg = arg->next) {
        const Field *field = nullptr;
        for (size_t i = 0; i < spec.field_count; ++i)
            if (arg->string && !strcmp(arg->string, spec.fields[i].arg)) field = &spec.fields[i];
        if (!field) return error(out, "Unknown argument. Use only the listed properties.");
        cJSON *group = cJSON_GetObjectItemCaseSensitive(patch.get(), field->group);
        if (!group) group = cJSON_AddObjectToObject(patch.get(), field->group);
        cJSON *value;
        if (!strcmp(field->arg, "speaking_speed")) {
            // The device stores tenths of normal speed.
            if (!cJSON_IsNumber(arg)) return error(out, "speaking_speed must be a number from 0.5 to 2.0.");
            value = cJSON_CreateNumber(std::lround(arg->valuedouble * 10));
        } else {
            value = cJSON_Duplicate(arg, true);
        }
        if (!group || !value) { cJSON_Delete(value); return false; }
        cJSON_AddItemToObject(group, field->key, value);
        cJSON_AddItemToObject(applied.get(), arg->string, cJSON_Duplicate(arg, true));
        ++count;
    }
    if (!count) return error(out, "Give at least one setting to change.");
    std::string detail;
    if (call.is_cancelled() || call.is_expired()) return error(out, "Setting change cancelled or expired.");
    if (!apply_settings(patch.get(), detail, call))
        return error(out, detail.empty() ? "The device rejected the change." : detail.c_str());
    cJSON_AddItemToObject(out, "applied", applied.release());
    return cJSON_AddBoolToObject(out, "ok", true) != nullptr;
}

// Battery voltage and estimated charge, or why it is unavailable.
cJSON *battery_json()
{
    cJSON *battery = cJSON_CreateObject();
    battery_reading_t reading;
    if (!battery || battery_read(&reading) != ESP_OK) {
        cJSON_AddStringToObject(battery, "error", "Battery sensing is unavailable on this device right now.");
        return battery;
    }
    cJSON_AddBoolToObject(battery, "present", reading.present);
    cJSON_AddNumberToObject(battery, "volts", reading.millivolts / 1000.0);
    if (reading.present) {
        cJSON_AddNumberToObject(battery, "estimatedPercent", reading.percent);
        cJSON_AddStringToObject(battery, "level", reading.percent >= 60 ? "good" : reading.percent >= 20 ? "medium" : "low");
    }
    cJSON_AddStringToObject(battery, "note", reading.present
        ? "Estimated from cell voltage. While charging over USB the voltage reads high, so the percentage can be "
          "overstated; the device cannot tell whether it is charging."
        : "No battery voltage detected; the device is probably running from USB without a battery.");
    if (!reading.calibrated) cJSON_AddBoolToObject(battery, "uncalibrated", true);
    return battery;
}

bool run_battery(const Spec &, const cJSON *args, cJSON *out, const unravel::ToolCallContext &)
{
    if (args && args->child) return error(out, "get_battery takes no arguments.");
    return cJSON_AddItemToObject(out, "battery", battery_json());
}

bool run_status(const Spec &, const cJSON *, cJSON *out, const unravel::ToolCallContext &)
{
    char *json = ai_chat_snapshot_json(AI_CHAT_NATIVE);
    Json snapshot(json ? cJSON_Parse(json) : nullptr, cJSON_Delete);
    free(json);
    if (!snapshot) return false;
    // Conversation and transient UI fields are not device status.
    for (const char *key : {"messages", "tools", "sources", "conversationId", "voiceTranscript",
                            "status", "lastError", "messageRevision", "voicePreview", "state", "busy"})
        cJSON_DeleteItemFromObject(snapshot.get(), key);
    cJSON *speed = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(snapshot.get(), "preferences"), "speechRate");
    if (cJSON_IsNumber(speed)) cJSON_SetNumberValue(speed, speed->valuedouble / 10);
    cJSON_AddItemToObject(out, "device", snapshot.release());

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    cJSON *system = cJSON_AddObjectToObject(out, "system");
    cJSON_AddStringToObject(system, "chip", chip.model == CHIP_ESP32S3 ? "ESP32-S3" : "ESP32");
    cJSON_AddNumberToObject(system, "cores", chip.cores);
    cJSON_AddNumberToObject(system, "uptimeSeconds", static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(system, "freeInternalKiB", heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024);
    cJSON_AddNumberToObject(system, "freePsramKiB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

    wifi_ap_record_t ap = {};
    cJSON *wifi = cJSON_AddObjectToObject(out, "wifi");
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) cJSON_AddNumberToObject(wifi, "rssi", ap.rssi);
    cJSON_AddStringToObject(wifi, "setupHotspot", "Unravel-Agent");
    cJSON_AddItemToObject(out, "battery", battery_json());
    return true;
}

bool run_scan(const Spec &, const cJSON *, cJSON *out, const unravel::ToolCallContext &call)
{
    uint16_t count = 0;
    if (call.is_cancelled() || call.is_expired()) return error(out, "Wi-Fi scan cancelled or expired.");
    if (esp_wifi_scan_start(nullptr, true) != ESP_OK || esp_wifi_scan_get_ap_num(&count) != ESP_OK)
        return error(out, "The Wi-Fi scan could not run. Try again in a moment.");
    std::vector<wifi_ap_record_t> records(count);
    if (count && esp_wifi_scan_get_ap_records(&count, records.data()) != ESP_OK)
        return error(out, "Wi-Fi scan results are unavailable. Try again.");
    records.resize(count);
    std::sort(records.begin(), records.end(),
              [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) { return a.rssi > b.rssi; });
    cJSON *networks = cJSON_AddArrayToObject(out, "networks");
    std::vector<std::string> seen;
    for (const auto &r : records) {
        std::string ssid(reinterpret_cast<const char *>(r.ssid), strnlen(reinterpret_cast<const char *>(r.ssid), sizeof(r.ssid)));
        if (ssid.empty() || std::find(seen.begin(), seen.end(), ssid) != seen.end()) continue;
        seen.push_back(ssid);
        cJSON *network = cJSON_CreateObject();
        cJSON_AddStringToObject(network, "ssid", ssid.c_str());
        cJSON_AddNumberToObject(network, "rssi", r.rssi);
        cJSON_AddStringToObject(network, "signal", r.rssi >= -60 ? "strong" : r.rssi >= -72 ? "good" : "weak");
        cJSON_AddBoolToObject(network, "secured", r.authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(networks, network);
        if (seen.size() == MAX_NETWORKS) break;
    }
    return true;
}

const Spec SPECS[] = {
    {"get_device_status", BUDDY_TOOL_READ, "Check", "device",
     "Read this device's current state: settings (brightness, volume, scene, screen sleep, voice, personality, "
     "time zone), Wi-Fi network, IP and signal, battery, firmware, local time, uptime and free memory.",
     run_status, nullptr, 0},
    {"get_battery", BUDDY_TOOL_READ, "Check", "battery",
     "Read the battery voltage and estimated charge percentage. The device cannot detect charging.",
     run_battery, nullptr, 0},
    {"set_display", BUDDY_TOOL_EDIT, "Set", nullptr,
     "Change the screen: brightness, night dimming, screen-sleep timeout or background scene. "
     "Applies and saves immediately. Pass only what the user asked to change.",
     run_settings, DISPLAY_FIELDS, sizeof(DISPLAY_FIELDS) / sizeof(DISPLAY_FIELDS[0])},
    {"set_sound", BUDDY_TOOL_EDIT, "Set", nullptr,
     "Change sound and speech: speaker volume, voice style, speaking speed, reading replies aloud and live captions. "
     "Applies and saves immediately. Pass only what the user asked to change.",
     run_settings, SOUND_FIELDS, sizeof(SOUND_FIELDS) / sizeof(SOUND_FIELDS[0])},
    {"set_profile", BUDDY_TOOL_EDIT, "Set", nullptr,
     "Change the buddy's personality, the user's name or the local time zone. Applies and saves immediately.",
     run_settings, PROFILE_FIELDS, sizeof(PROFILE_FIELDS) / sizeof(PROFILE_FIELDS[0])},
    {"scan_wifi", BUDDY_TOOL_SEARCH, "Scan", "Wi-Fi",
     "List nearby Wi-Fi networks with signal strength, strongest first.",
     run_scan, nullptr, 0},
};

const Spec *find(const char *name)
{
    for (const Spec &spec : SPECS)
        if (name && !strcmp(spec.name, name)) return &spec;
    return nullptr;
}

bool execute(const char *arguments_json, std::string &output, const unravel::ToolCallContext &call, void *context)
{
    const Spec &spec = *static_cast<const Spec *>(context);
    Json args(cJSON_Parse(arguments_json ? arguments_json : "{}"), cJSON_Delete);
    Json out(cJSON_CreateObject(), cJSON_Delete);
    if (!out) return false;
    // Rejections are ordinary results so the model can explain them to the user.
    const bool ok = call.is_cancelled() || call.is_expired()
        ? error(out.get(), "Device request cancelled or expired.")
        : cJSON_IsObject(args.get()) ? spec.run(spec, args.get(), out.get(), call)
                                    : error(out.get(), "Arguments must be a JSON object.");
    if (!ok) return false;
    char *text = cJSON_PrintUnformatted(out.get());
    if (!text) return false;
    output.assign(text);
    cJSON_free(text);
    ESP_LOGI("buddy", "Device tool %s completed", spec.name);
    return true;
}
} // namespace

bool device_tools_register(unravel::Agent &agent, DeviceSettingsApplier applier)
{
    apply_settings = applier;
    bool ok = true;
    for (const Spec &spec : SPECS) {
        Json schema(object_schema(spec), cJSON_Delete);
        char *text = schema ? cJSON_PrintUnformatted(schema.get()) : nullptr;
        if (!text) {
            ESP_LOGE("buddy", "Could not serialize device tool schema %s", spec.name);
            ok = false;
            continue;
        }
        unravel::Tool tool;
        tool.name = spec.name;
        tool.description = spec.description;
        tool.parameters_json = text;
        tool.execute = execute;
        tool.context = const_cast<Spec *>(&spec);
        cJSON_free(text);
        if (!agent.add_tool(std::move(tool))) {
            ESP_LOGE("buddy", "Could not register device tool %s", spec.name);
            ok = false;
        }
    }
    return ok;
}

bool device_tool_describe(const char *name, const cJSON *arguments, buddy_ui_tool_kind_t &kind,
                          std::string &verb, std::string &target)
{
    const Spec *spec = find(name);
    if (!spec) return false;
    kind = spec->kind;
    verb = spec->verb;
    target.clear();
    if (spec->target) { target = spec->target; return true; }
    // Summarize only settings-backed arguments, e.g. "brightness 40, scene night".
    for (const cJSON *arg = cJSON_IsObject(arguments) ? arguments->child : nullptr; arg; arg = arg->next) {
        bool known = false;
        for (size_t i = 0; i < spec->field_count; ++i)
            if (arg->string && !strcmp(arg->string, spec->fields[i].arg)) known = true;
        if (!known) continue;
        if (!target.empty()) target += ", ";
        std::string key(arg->string);
        std::replace(key.begin(), key.end(), '_', ' ');
        target += key + " ";
        if (cJSON_IsString(arg)) target += arg->valuestring;
        else if (cJSON_IsBool(arg)) target += cJSON_IsTrue(arg) ? "on" : "off";
        else if (cJSON_IsNumber(arg)) {
            char number[16];
            snprintf(number, sizeof(number), "%g", arg->valuedouble);
            target += number;
        }
    }
    return true;
}

