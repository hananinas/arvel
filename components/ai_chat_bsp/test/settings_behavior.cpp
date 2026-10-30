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
std::vector<std::string> erased;
bool fail_commit = false;
size_t available_entries = 2048, blob_writes = 0;
BuddySettings configured() {
    auto s = buddy_settings_defaults();
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
    // The retired Niko group is an unknown group, not a silently ignored one.
    for (const char *text : {R"({"niko":{}})", R"({"niko":{"enabled":false}})"}) {
        next.name = "unchanged";
        assert(!patch(s, text, next)); assert(next.name == "unchanged");
    }
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
    auto s = configured(); std::string error;
    committed["niko"] = {'U','B','N','1',1,1,0}; // Credentials blob left by the removed integration.
    assert(buddy_settings_save(s,error));
    assert(!committed.count("niko") && erased.size() == 1 && erased[0] == "niko");
    const auto existing_conversation = committed.at("conversation");
    auto loaded = buddy_settings_defaults(); assert(buddy_settings_load(loaded,error));
    assert(loaded.live.api_key == "live-fixture");
    Json root(buddy_settings_public_json(loaded), cJSON_Delete); assert(root);
    assert(!cJSON_GetObjectItemCaseSensitive(root.get(),"niko"));
    char *encoded = cJSON_PrintUnformatted(root.get()); assert(encoded);
    assert(!std::strstr(encoded,"live-fixture")); cJSON_free(encoded);
    const auto baseline = committed;
    available_entries = 126; const size_t before_writes = blob_writes;
    assert(!buddy_settings_save(s,error) && error.find("capacity") != std::string::npos);
    assert(blob_writes == before_writes && committed == baseline); available_entries = 2048;
    fail_commit = true; s.live.api_key = "replacement"; assert(!buddy_settings_save(s,error)); fail_commit = false;
    loaded = buddy_settings_defaults(); assert(buddy_settings_load(loaded,error)); assert(loaded.live.api_key == "live-fixture");
    committed = baseline; committed["niko"] = {0xff}; // An unreadable leftover blob never blocks loading.
    loaded = buddy_settings_defaults(); assert(buddy_settings_load(loaded,error));
    assert(loaded.live.api_key == "live-fixture" && committed.at("conversation") == existing_conversation);
    committed = baseline; committed["conversation"][4] = 2;
    loaded = buddy_settings_defaults(); assert(!buddy_settings_load(loaded,error)); assert(loaded.live.api_key.empty());
}
}
extern "C" {
esp_err_t nvs_open(const char *, int, nvs_handle_t *handle) { staged.clear(); erased.clear(); *handle = 1; return ESP_OK; }
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
esp_err_t nvs_erase_key(nvs_handle_t, const char *key) {
    if (!committed.count(key) && !staged.count(key)) return ESP_ERR_NVS_NOT_FOUND;
    staged.erase(key); erased.push_back(key); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    if (fail_commit) return ESP_FAIL;
    for (const auto &entry : staged) committed[entry.first] = entry.second;
    for (const auto &key : erased) committed.erase(key);
    staged.clear(); return ESP_OK;
}
esp_err_t nvs_get_stats(const char *, nvs_stats_t *stats) { *stats = {}; stats->free_entries = available_entries; return ESP_OK; }
void nvs_close(nvs_handle_t) { staged.clear(); }
void mbedtls_platform_zeroize(void *buffer, size_t size) { auto *b = static_cast<volatile unsigned char *>(buffer); while (size--) *b++ = 0; }
}
int main() {
    patches(); native_endpoints(); persistence();
    std::cout << "PASS: actual settings serializer secret masking, retired Niko blob purge, strict version/length rejection, UBC1 compatibility and failed commit admission\n";
}
