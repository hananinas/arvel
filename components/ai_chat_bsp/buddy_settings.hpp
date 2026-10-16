#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cJSON.h"

/* Device preferences edited from the screen's settings panel or the web app.
 * Enumerations are stored as indexes into the matching *_names arrays. */
struct BuddyPreferences {
    std::string user_name;
    std::string timezone = "UTC0"; // POSIX TZ string
    uint8_t voice_style = 0;       // buddy_voice_style_names
    uint8_t speech_rate = 10;      // tenths of normal speed, 5..20
    bool captions = true;
    uint8_t brightness = 70;       // percent, 10..100 in steps of 10
    bool dim_at_night = true;
    uint8_t screen_sleep = 1;      // buddy_screen_sleep_names
    uint8_t persona = 0;           // buddy_persona_names
    uint8_t volume = 60;           // percent, 0..100 in steps of 10
};

extern const char *const buddy_voice_style_names[3];
extern const char *const buddy_screen_sleep_names[3];
extern const char *const buddy_persona_names[3];

struct BuddyLiveDeployment {
    std::string provider = "nebius";
    std::string model = "nvidia/NVIDIA-NemotronLabs-VoiceChat-11B";
    std::string protocol = "nemotron_labs_realtime";
    std::string endpoint;
    std::string api_key;
};

struct BuddyNikoProfile {
    bool enabled = false;
    std::string controller;
    std::string username;
    std::string jwt;
    std::string ca_pem;
    std::string tls_server_name;
    std::vector<std::string> allowed_lights;
};

struct BuddySettings {
    std::string chat_provider = "ollama";
    std::string conversation_mode = "text_pipeline";
    BuddyLiveDeployment live;
    BuddyNikoProfile niko;
    std::string chat_endpoint;
    std::string chat_model;
    std::string chat_api_key;
    std::string voice_endpoint;
    std::string voice_model;
    std::string voice_api_key;
    std::string tts_backend = "local";
    std::string tts_voice = "sol";
    std::string search_provider = "tavily";
    bool search_enabled = false;
    std::string tavily_api_key;
    std::string parallel_api_key;
    std::string name;
    std::string instructions;
    std::string scene = "meadow";
    bool read_aloud = false;
    BuddyPreferences prefs;
};

BuddySettings buddy_settings_defaults();
bool buddy_settings_load(BuddySettings &settings, std::string &error);
bool buddy_settings_patch(const cJSON *patch, const BuddySettings &current,
                          BuddySettings &candidate, std::string &error);
bool buddy_settings_save(const BuddySettings &settings, std::string &error);
/* String values are borrowed: keep settings alive and unchanged until JSON deletion. */
cJSON *buddy_settings_public_json(const BuddySettings &settings);
cJSON *buddy_preferences_json(const BuddyPreferences &prefs);
