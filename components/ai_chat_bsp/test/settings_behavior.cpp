#include "buddy_settings.hpp"
#include "arvel_art.h"
#include "nvs.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <vector>

// The generated art symbols are not linked into the serializer host executable.
extern "C" const arvel_scene_t arvel_scenes[ARVEL_SCENE_COUNT] = {
    {"meadow", "Meadow", false, nullptr, 0, nullptr, 0},
    {"rain", "Rain", false, nullptr, 0, nullptr, 0},
    {"a", "", false, nullptr, 0, nullptr, 0}, {"b", "", false, nullptr, 0, nullptr, 0},
    {"c", "", false, nullptr, 0, nullptr, 0}, {"d", "", false, nullptr, 0, nullptr, 0},
    {"e", "", false, nullptr, 0, nullptr, 0}, {"f", "", false, nullptr, 0, nullptr, 0},
    {"g", "", false, nullptr, 0, nullptr, 0}, {"h", "", false, nullptr, 0, nullptr, 0},
    {"i", "", false, nullptr, 0, nullptr, 0}, {"j", "", false, nullptr, 0, nullptr, 0}
};
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
using Blobs = std::map<std::string, std::vector<uint8_t>>;
Blobs committed, staged;
bool fail_commit = false;
size_t available_entries = 2048, blob_writes = 0;
const char *id = "0000000a-0000-4000-8000-000000000001";
const char *ca = "-----BEGIN CERTIFICATE-----\nZml4dHVyZQ==\n-----END CERTIFICATE-----\n";
BuddySettings configured() {
    auto s = buddy_settings_defaults();
    s.niko.enabled = true; s.niko.controller = "192.0.2.10"; s.niko.tls_server_name = "controller.example.org";
    s.niko.username = "owner-fixture"; s.niko.jwt = "token-fixture"; s.niko.ca_pem = ca; s.niko.allowed_lights = {id};
    s.live.endpoint = "wss://voice.example.org/v1/realtime"; s.live.api_key = "live-fixture";
    return s;
}
bool patch(const BuddySettings &current, const char *text, BuddySettings &out) {
    Json json(cJSON_Parse(text), cJSON_Delete); assert(json);
    std::string error;
    const bool result = buddy_settings_patch(json.get(), current, out, error);
    assert(result == error.empty());
    return result;
}
void patches() {
    const auto s = configured(); BuddySettings next = buddy_settings_defaults();
    for (const char *text : {
        R"({"niko":{"controller":"192.0.2.11"}})",
        R"({"niko":{"tlsServerName":"other.example.org","jwt":"fresh"}})",
        R"({"niko":{"enabled":false,"controller":"192.0.2.11"}})",
        R"({"niko":{"username":""}})", R"({"niko":{"allowedLights":["named-socket"]}})",
        R"({"niko":{"allowedLights":["0000000a-0000-4000-8000-000000000001","0000000a-0000-4000-8000-000000000001"]}})",
        R"({"niko":{"allowedLights":["0000000A-0000-4000-8000-000000000001"]}})",
        R"({"niko":{"mqttTopic":"arbitrary"}})", R"({"niko":{"enabled":true,"enabled":false}})",
        R"({"niko":{},"niko":{}})", R"({"niko":{"controller":"mqtts://controller.example.org:8884"}})",
        R"({"niko":{"tlsServerName":"[::1]"}})", R"({"niko":{"jwt":"bad\nheader"}})",
        R"({"niko":{"caPem":"not a certificate"}})"}) {
        next.name = "unchanged";
        assert(!patch(s, text, next)); assert(next.name == "unchanged");
    }
    assert(patch(s, R"({"niko":{"enabled":false,"controller":"192.0.2.11","username":"","jwt":"","caPem":""}})", next));
    assert(!next.niko.enabled && next.niko.jwt.empty() && next.niko.allowed_lights.empty());
    Json input(cJSON_CreateObject(), cJSON_Delete); auto *n = cJSON_AddObjectToObject(input.get(), "niko");
    cJSON_AddStringToObject(n,"controller","192.0.2.11"); cJSON_AddStringToObject(n,"username","fresh");
    cJSON_AddStringToObject(n,"jwt","fresh-token"); cJSON_AddStringToObject(n,"caPem",ca);
    std::string error; assert(buddy_settings_patch(input.get(),s,next,error)); assert(next.niko.allowed_lights.empty());
    n = cJSON_GetObjectItemCaseSensitive(input.get(), "niko");
    const std::string oversized(2049,'x'); cJSON_ReplaceItemInObjectCaseSensitive(n,"jwt",cJSON_CreateString(oversized.c_str()));
    assert(!buddy_settings_patch(input.get(),s,next,error));
    assert(patch(s, R"({"niko":{"enabled":false,"username":"","jwt":"","caPem":"","allowedLights":[]}})", next));
    assert(next.niko.username.empty() && next.niko.jwt.empty() && next.niko.ca_pem.empty());
}
void native_endpoints() {
    auto s = configured(); BuddySettings next;
    for (const char *endpoint : {"wss://[::1]/v1/realtime", "wss://voice.example.org/v1/realtime/",
        "wss://voice.example.org:0/v1/realtime", "wss://voice.example.org:65536/v1/realtime",
        "wss://user:secret@voice.example.org/v1/realtime", "wss://999.1.1.1/v1/realtime",
        "wss://voice.example.org/%76%31/realtime", "wss://voice.example.org/v1/realtime#secret"}) {
        Json root(cJSON_CreateObject(),cJSON_Delete); auto *live = cJSON_AddObjectToObject(root.get(),"live");
        cJSON_AddStringToObject(live,"endpoint",endpoint); cJSON_AddBoolToObject(live,"clearApiKey",true);
        std::string error; assert(!buddy_settings_patch(root.get(),s,next,error));
    }
    assert(patch(s,R"({"live":{"endpoint":"wss://192.0.2.10:8443/v1/realtime","apiKey":"fresh"},"conversationMode":"native_audio"})",next));
}
void persistence() {
    auto s = configured(); std::string error; assert(buddy_settings_save(s,error));
    const auto existing_conversation = committed.at("conversation");
    auto loaded = buddy_settings_defaults(); assert(buddy_settings_load(loaded,error));
    assert(loaded.niko.jwt == s.niko.jwt && loaded.niko.ca_pem == s.niko.ca_pem && loaded.niko.allowed_lights == s.niko.allowed_lights);
    Json root(buddy_settings_public_json(loaded), cJSON_Delete); assert(root);
    auto *niko = cJSON_GetObjectItemCaseSensitive(root.get(),"niko"); assert(niko);
    assert(!cJSON_GetObjectItemCaseSensitive(niko,"username") && !cJSON_GetObjectItemCaseSensitive(niko,"jwt") && !cJSON_GetObjectItemCaseSensitive(niko,"caPem"));
    char *encoded = cJSON_PrintUnformatted(root.get()); assert(encoded);
    assert(!std::strstr(encoded,"owner-fixture") && !std::strstr(encoded,"token-fixture") && !std::strstr(encoded,"BEGIN CERTIFICATE") && !std::strstr(encoded,"live-fixture")); cJSON_free(encoded);
    const auto baseline = committed;
    available_entries = 126; const size_t before_writes = blob_writes;
    assert(!buddy_settings_save(s,error) && error.find("capacity") != std::string::npos);
    assert(blob_writes == before_writes && committed == baseline); available_entries = 2048;
    fail_commit = true; s.niko.jwt = "replacement"; assert(!buddy_settings_save(s,error)); fail_commit = false;
    loaded = buddy_settings_defaults(); assert(buddy_settings_load(loaded,error)); assert(loaded.niko.jwt == "token-fixture");
    for (size_t index : {size_t(4), size_t(5), size_t(6), size_t(7)}) {
        committed = baseline; committed["niko"][index] = 255;
        loaded = buddy_settings_defaults(); assert(!buddy_settings_load(loaded,error));
        assert(!loaded.niko.enabled && loaded.niko.allowed_lights.empty() && loaded.niko.jwt.empty());
    }
    committed = baseline; committed["niko"].push_back(0);
    loaded = buddy_settings_defaults(); assert(!buddy_settings_load(loaded,error)); assert(!loaded.niko.enabled);
    committed = baseline; committed.erase("niko"); // Existing UBC1 owners need no legacy wire aliases.
    loaded = buddy_settings_defaults(); assert(buddy_settings_load(loaded,error));
    assert(loaded.live.api_key == "live-fixture" && !loaded.niko.enabled && loaded.niko.allowed_lights.empty());
    assert(committed.at("conversation") == existing_conversation);
    committed = baseline; committed["conversation"][4] = 2;
    loaded = buddy_settings_defaults(); assert(!buddy_settings_load(loaded,error)); assert(!loaded.niko.enabled);
}
}
extern "C" {
esp_err_t nvs_open(const char *, int, nvs_handle_t *handle) { staged.clear(); *handle = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t, const char *key, void *bytes, size_t *size) {
    const auto it = committed.find(key); if (it == committed.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (!bytes) { *size = it->second.size(); return ESP_OK; }
    if (*size < it->second.size()) return ESP_ERR_NVS_INVALID_LENGTH;
    std::memcpy(bytes,it->second.data(),it->second.size()); *size = it->second.size(); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char *key, const void *bytes, size_t size) {
    ++blob_writes;
    const auto *b = static_cast<const uint8_t *>(bytes); staged[key].assign(b,b+size); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    if (fail_commit) return ESP_FAIL;
    for (const auto &entry : staged) committed[entry.first] = entry.second;
    staged.clear(); return ESP_OK;
}
esp_err_t nvs_get_stats(const char *, nvs_stats_t *stats) { *stats = {}; stats->free_entries = available_entries; return ESP_OK; }
void nvs_close(nvs_handle_t) { staged.clear(); }
void mbedtls_platform_zeroize(void *buffer, size_t size) { auto *b = static_cast<volatile unsigned char *>(buffer); while (size--) *b++ = 0; }
}
int main() {
    patches(); native_endpoints(); persistence();
    std::cout << "PASS: actual settings serializer secret masking, controller credential hops, strict version/length/permission rejection, UBC1 compatibility and failed commit admission\n";
}
