#include "buddy_settings.hpp"
#include "arvel_art.h"

#include "sdkconfig.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <cstdint>
#include "mbedtls/platform_util.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

namespace {
constexpr size_t endpoint_limit = 512;
constexpr size_t model_limit = 128;
constexpr size_t key_limit = 1024;
constexpr size_t name_limit = 32;
constexpr size_t instructions_limit = 2048;
constexpr size_t scene_limit = 32;
constexpr size_t field_count = 9;
constexpr size_t header_size = 6 + field_count * 2;
constexpr size_t record_limit = header_size + 2 * endpoint_limit + 2 * model_limit +
                                2 * key_limit + name_limit + instructions_limit + scene_limit;
constexpr char storage_namespace[] = "buddy";
constexpr char storage_key[] = "settings";
constexpr char prefs_key[] = "prefs";
constexpr size_t timezone_limit = 64;
constexpr size_t prefs_fixed = 4 + 1 + 8 + 2; // v1: magic, version, 8 values, 2 lengths
constexpr size_t prefs_fixed_v2 = 4 + 1 + 9 + 2; // Previously flashed v2 records include a removed flag.
constexpr char tts_key[] = "tts";
constexpr size_t tts_voice_limit = 16;
constexpr size_t tts_fixed = 7; // magic, version, backend, voice length
constexpr size_t legacy_tts_fixed = 11;
constexpr const char *tts_voices[] = {
    "arbor", "breeze", "cove", "ember", "juniper", "maple", "sol", "spruce", "vale"
};
constexpr char conversation_key[] = "conversation";
constexpr size_t provider_limit = 64;
constexpr size_t conversation_fields = 5, conversation_header = 6 + 2 * conversation_fields;
constexpr size_t conversation_limit = conversation_header + 2 * provider_limit + model_limit + endpoint_limit + key_limit;
constexpr const char *conversation_modes[] = {"text_pipeline", "codex_live", "native_audio"};
constexpr size_t niko_host_limit = 253, niko_username_limit = 128;
constexpr size_t niko_jwt_limit = 2048, niko_ca_limit = 8192, niko_fixture_limit = 16;
constexpr size_t niko_fields = 5, niko_header = 7 + 2 * niko_fields;
constexpr size_t niko_record_limit = niko_header + 2 * niko_host_limit +
    niko_username_limit + niko_jwt_limit + niko_ca_limit + 36 * niko_fixture_limit;

struct NvsHandle {
    nvs_handle_t value = 0;
    ~NvsHandle() { if (value) nvs_close(value); }
};
using Buffer = std::unique_ptr<uint8_t, decltype(&std::free)>;
struct SecretRecord {
    Buffer bytes;
    size_t size;
    explicit SecretRecord(size_t length) : bytes(static_cast<uint8_t *>(std::malloc(length)), &std::free), size(length) {}
    ~SecretRecord() { if (bytes) mbedtls_platform_zeroize(bytes.get(), size); }
};

bool fail(std::string &error, const char *message)
{
    error = message;
    return false;
}

bool ascii_equal(const char *text, size_t size, const char *expected)
{
    if (size != std::strlen(expected)) return false;
    for (size_t i = 0; i < size; ++i) {
        const char c = text[i] >= 'A' && text[i] <= 'Z' ? text[i] + ('a' - 'A') : text[i];
        if (c != expected[i]) return false;
    }
    return true;
}

bool whitespace(uint32_t c)
{
    return c == 0x20 || (c >= 9 && c <= 13) || c == 0x85 || c == 0xa0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
           c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

// Validate every byte, including embedded NUL in length-bearing persisted fields.
bool valid_text(const std::string &text, size_t limit, bool allow_empty,
                bool allow_whitespace, bool multiline)
{
    if (text.size() > limit || (!allow_empty && text.empty())) return false;
    bool nonblank = false;
    for (size_t i = 0; i < text.size();) {
        uint32_t c = static_cast<unsigned char>(text[i++]);
        unsigned continuation = 0;
        uint32_t minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { continuation = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { continuation = 2; minimum = 0x800; c &= 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { continuation = 3; minimum = 0x10000; c &= 7; }
        else if (c >= 0x80) return false;
        if (continuation > text.size() - i) return false;
        for (unsigned j = 0; j < continuation; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            c = (c << 6) | (next & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        if ((c < 0x20 || (c >= 0x7f && c <= 0x9f)) &&
            !(multiline && (c == '\n' || c == '\r' || c == '\t'))) return false;
        if (!allow_whitespace && whitespace(c)) return false;
        if (!whitespace(c)) nonblank = true;
    }
    return allow_empty || nonblank;
}

bool alphanumeric(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ascii_key(const std::string &key)
{
    for (unsigned char c : key) if (c < 0x21 || c > 0x7e) return false;
    return key.size() <= key_limit;
}

bool valid_url(const std::string &url, const char *scheme)
{
    const size_t scheme_size = std::strlen(scheme);
    if (url.size() > endpoint_limit || url.size() < scheme_size + 2 ||
        url.compare(0, scheme_size, scheme) != 0) return false;
    for (unsigned char c : url) {
        if (c <= 0x20 || c >= 0x7f || c == '?' || c == '#' || c == '\\') return false;
    }
    const char *host = url.data() + scheme_size;
    const char *end = url.data() + url.size();
    const char *path = static_cast<const char *>(std::memchr(host, '/', end - host));
    if (!path || path == host || std::memchr(host, '@', path - host)) return false;
    const char *port = nullptr;
    if (*host == '[') {
        const char *close = static_cast<const char *>(std::memchr(host, ']', path - host));
        if (!close || close == host + 1 || (close + 1 != path && close[1] != ':')) return false;
        const size_t size = close - host - 1;
        char address[INET6_ADDRSTRLEN];
        if (size >= sizeof(address)) return false;
        std::memcpy(address, host + 1, size);
        address[size] = '\0';
        struct in6_addr parsed;
        if (inet_pton(AF_INET6, address, &parsed) != 1) return false;
        if (close + 1 != path) port = close + 2;
    } else {
        const char *colon = static_cast<const char *>(std::memchr(host, ':', path - host));
        const char *host_end = colon ? colon : path;
        size_t size = host_end - host;
        if (size == 0 || size > 253) return false;
        if (host[size - 1] == '.') --size;
        if (!size) return false;
        size_t label = 0;
        bool numeric = true;
        for (size_t i = 0; i < size; ++i) {
            const char c = host[i];
            if (c == '.') {
                if (!label || host[i - 1] == '-') return false;
                label = 0;
            } else {
                if ((!alphanumeric(c) && c != '-') || (!label && c == '-') || ++label > 63) return false;
                if (c < '0' || c > '9') numeric = false;
            }
        }
        if (!label || host[size - 1] == '-') return false;
        if (numeric) {
            char address[INET_ADDRSTRLEN];
            if (size >= sizeof(address)) return false;
            std::memcpy(address, host, size);
            address[size] = '\0';
            struct in_addr parsed;
            if (inet_pton(AF_INET, address, &parsed) != 1) return false;
        }
        if (colon) port = colon + 1;
    }
    if (port) {
        if (port == path || path - port > 5) return false;
        unsigned value = 0;
        for (const char *p = port; p != path; ++p) {
            if (*p < '0' || *p > '9') return false;
            value = value * 10 + static_cast<unsigned>(*p - '0');
        }
        if (value == 0 || value > 65535) return false;
    }
    for (const char *p = path; p != end; ++p) {
        if (*p == '%') {
            if (end - p < 3) return false;
            const int high = hex_digit(p[1]), low = hex_digit(p[2]);
            if (high < 0 || low < 0) return false;
            const unsigned decoded = static_cast<unsigned>(high * 16 + low);
            if (decoded < 0x20 || decoded == 0x7f) return false;
            p += 2;
        } else if (!alphanumeric(*p) && !std::strchr("-._~!$&'()*+,;=:@/", *p)) {
            return false;
        }
    }
    return true;
}

bool valid_endpoint(const std::string &url) { return valid_url(url, "https://"); }
bool valid_live_endpoint(const std::string &url) {
    if (!valid_url(url, "wss://") || url[6] == '[') return false;
    const size_t path = url.find('/', 6);
    return path != std::string::npos && url.compare(path, std::string::npos, "/v1/realtime") == 0;
}

bool valid_niko_host(const std::string &host) {
    if (host.empty() || host.size() > niko_host_limit || host.find_first_of(":/[]@?#\\") != std::string::npos)
        return false;
    return valid_url("https://" + host + "/", "https://");
}

bool valid_fixture_id(const std::string &id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (hex_digit(id[i]) < 0 || (id[i] >= 'A' && id[i] <= 'F')) return false;
    }
    return true;
}

bool validate_niko(const BuddyNikoProfile &niko, std::string &error) {
    if ((!niko.controller.empty() && !valid_niko_host(niko.controller)) ||
        (!niko.tls_server_name.empty() && !valid_niko_host(niko.tls_server_name)))
        return fail(error, "Niko controller and TLS identity must be DNS names or IPv4 addresses, without URL or port.");
    if (!valid_text(niko.username, niko_username_limit, true, false, false) ||
        niko.jwt.size() > niko_jwt_limit ||
        !valid_text(niko.jwt, niko_jwt_limit, true, false, false))
        return fail(error, "Niko username/token exceed their 128/2048 byte limits or contain whitespace or controls.");
    for (unsigned char c : niko.jwt) if (c < 0x21 || c > 0x7e)
        return fail(error, "Niko token must contain printable ASCII without whitespace.");
    if (!valid_text(niko.ca_pem, niko_ca_limit, true, true, true) ||
        (!niko.ca_pem.empty() && (niko.ca_pem.find("-----BEGIN CERTIFICATE-----") != 0 ||
         niko.ca_pem.find("-----END CERTIFICATE-----") == std::string::npos)))
        return fail(error, "Provide the Niko certificate PEM (at most 8192 bytes), not a trust bypass.");
    if (niko.allowed_lights.size() > niko_fixture_limit)
        return fail(error, "At most 16 individual Niko light/dimmer IDs may be enabled.");
    for (size_t i = 0; i < niko.allowed_lights.size(); ++i) {
        if (!valid_fixture_id(niko.allowed_lights[i])) return fail(error, "Niko fixture IDs must be lowercase UUIDs.");
        for (size_t j = 0; j < i; ++j) if (niko.allowed_lights[j] == niko.allowed_lights[i])
            return fail(error, "Duplicate Niko fixture ID.");
    }
    if (niko.enabled && (niko.controller.empty() || niko.tls_server_name.empty() ||
        niko.username.empty() || niko.jwt.empty() || niko.ca_pem.empty()))
        return fail(error, "Enabled Niko requires controller, verified TLS identity, username, token and provided CA.");
    return true;
}

bool validate_conversation(const BuddySettings &settings, std::string &error) {
    bool mode = false;
    for (const char *name : conversation_modes) if (settings.conversation_mode == name) mode = true;
    if (!mode) return fail(error, "Choose text_pipeline, codex_live or native_audio conversation mode.");
    if (!valid_text(settings.chat_provider, provider_limit, false, false, false) ||
        !valid_text(settings.live.provider, provider_limit, false, false, false))
        return fail(error, "Provider identities must be nonblank UTF-8 up to 64 bytes, without whitespace or controls.");
    if (settings.live.protocol != "nemotron_labs_realtime" ||
        settings.live.model != "nvidia/NVIDIA-NemotronLabs-VoiceChat-11B")
        return fail(error, "Native audio requires the documented released Labs 11B model and nemotron_labs_realtime protocol.");
    if ((!settings.live.endpoint.empty() && !valid_live_endpoint(settings.live.endpoint)) ||
        !ascii_key(settings.live.api_key) || (settings.live.endpoint.empty() && !settings.live.api_key.empty()))
        return fail(error, "Native deployment requires an exact verified wss://host[:port]/v1/realtime URL and a bounded ASCII key.");
    if (settings.conversation_mode == "native_audio" && settings.live.endpoint.empty())
        return fail(error, "Set the native deployment WebSocket endpoint before selecting native audio.");
    return true;
}

bool ollama_endpoint(const std::string &url)
{
    if (url.size() < 8) return false;
    const char *host = url.data() + 8;
    size_t size = std::strcspn(host, ":/");
    if (size && host[size - 1] == '.') --size;
    return ascii_equal(host, size, "ollama.com");
}

void normalize_model(BuddySettings &settings)
{
    if (ollama_endpoint(settings.chat_endpoint) && settings.chat_model.size() >= 6 &&
        settings.chat_model.compare(settings.chat_model.size() - 6, 6, ":cloud") == 0) {
        settings.chat_model.resize(settings.chat_model.size() - 6);
    }
}

bool valid_timezone(const std::string &tz)
{
    if (tz.empty() || tz.size() > timezone_limit) return false;
    for (unsigned char c : tz) if (c < 0x21 || c > 0x7e) return false;
    return true;
}

bool validate_prefs(const BuddyPreferences &prefs, std::string &error)
{
    if (!valid_text(prefs.user_name, name_limit, true, true, false))
        return fail(error, "Your name must be UTF-8, at most 32 bytes, without controls.");
    if (!valid_timezone(prefs.timezone))
        return fail(error, "Timezone must be a POSIX TZ string of 1 to 64 printable ASCII characters.");
    if (prefs.voice_style > 2 || prefs.screen_sleep > 2 || prefs.persona > 2)
        return fail(error, "Voice, screen sleep and personality must be one of the listed options.");
    if (prefs.speech_rate < 5 || prefs.speech_rate > 20)
        return fail(error, "Speaking speed must be between 5 and 20 tenths.");
    if (prefs.brightness < 10 || prefs.brightness > 100 || prefs.brightness % 10)
        return fail(error, "Brightness must be 10 to 100 percent in steps of 10.");
    if (prefs.volume > 100 || prefs.volume % 10)
        return fail(error, "Volume must be 0 to 100 percent in steps of 10.");
    return true;
}

bool validate_tts(const BuddySettings &settings, std::string &error)
{
    if (settings.tts_backend != "local" && settings.tts_backend != "codex")
        return fail(error, "Speech backend must be local or codex.");
    bool known_voice = false;
    for (const char *voice : tts_voices) {
        if (settings.tts_voice == voice) { known_voice = true; break; }
    }
    if (!known_voice) return fail(error, "Codex voice must be one of the listed live voices.");
    return true;
}

bool validate(const BuddySettings &settings, std::string &error)
{
    if (!valid_endpoint(settings.chat_endpoint) || !valid_endpoint(settings.voice_endpoint))
        return fail(error, "Endpoints must be full HTTPS URLs with a valid host, port and path, without credentials, query or fragment.");
    if (!valid_text(settings.chat_model, model_limit, false, false, false) ||
        !valid_text(settings.voice_model, model_limit, false, false, false))
        return fail(error, "Models must be nonblank UTF-8, at most 128 bytes, without whitespace or controls.");
    if (!ascii_key(settings.chat_api_key) || !ascii_key(settings.voice_api_key))
        return fail(error, "API keys must be at most 1024 ASCII bytes without whitespace or controls.");
    if ((settings.search_provider != "tavily" && settings.search_provider != "parallel") ||
        !ascii_key(settings.tavily_api_key) || !ascii_key(settings.parallel_api_key))
        return fail(error, "Search requires Tavily or Parallel and ASCII API keys of at most 1024 bytes.");
    if (!valid_text(settings.name, name_limit, false, true, false))
        return fail(error, "Buddy name must be nonblank UTF-8, at most 32 bytes, without controls.");
    if (!valid_text(settings.instructions, instructions_limit, true, true, true))
        return fail(error, "Instructions must be valid UTF-8, at most 2048 bytes, with only newline and tab controls.");
    bool known_scene = false;
    for (const auto &scene : arvel_scenes) {
        if (settings.scene == scene.id) { known_scene = true; break; }
    }
    if (!known_scene) return fail(error, "Scene must be one of the built-in Arvel scenes.");
    if (!validate_tts(settings, error)) return false;
    if (!validate_conversation(settings, error)) return false;
    if (!validate_niko(settings.niko, error)) return false;
    return validate_prefs(settings.prefs, error);
}

bool string_field(const cJSON *field, size_t limit, std::string &target, std::string &error)
{
    // cJSON has no decoded lengths: its caller must reject escaped/literal NUL before parsing.
    if (!cJSON_IsString(field) || !field->valuestring)
        return fail(error, "Settings string fields must be strings.");
    const size_t size = strnlen(field->valuestring, limit + 1);
    if (size > limit) return fail(error, "Settings string field exceeds its byte limit.");
    target.assign(field->valuestring, size);
    return true;
}

bool patch_provider(const cJSON *group, const std::string &old_endpoint,
                    const std::string &old_key, std::string &endpoint,
                    std::string &model, std::string &key, std::string &error,
                    std::string *provider = nullptr, const std::string *old_provider = nullptr)
{
    if (!cJSON_IsObject(group)) return fail(error, "Provider settings must be objects.");
    unsigned seen = 0;
    bool clear_key = false;
    const cJSON *supplied_key = nullptr;
    for (const cJSON *field = group->child; field; field = field->next) {
        unsigned bit = 0;
        if (!field->string) return fail(error, "Invalid settings field.");
        if (std::strcmp(field->string, "endpoint") == 0) bit = 1;
        else if (std::strcmp(field->string, "model") == 0) bit = 2;
        else if (std::strcmp(field->string, "apiKey") == 0) bit = 4;
        else if (std::strcmp(field->string, "clearApiKey") == 0) bit = 8;
        else if (provider && std::strcmp(field->string, "provider") == 0) bit = 16;
        else return fail(error, "Unknown provider settings field.");
        if (seen & bit) return fail(error, "Duplicate settings field.");
        seen |= bit;
        if (bit == 1 && !string_field(field, endpoint_limit, endpoint, error)) return false;
        if (bit == 2 && !string_field(field, model_limit, model, error)) return false;
        if (bit == 16 && !string_field(field, provider_limit, *provider, error)) return false;
        if (bit == 4) {
            if (!cJSON_IsString(field) || !field->valuestring ||
                strnlen(field->valuestring, key_limit + 1) > key_limit)
                return fail(error, "API key must be a string of at most 1024 bytes.");
            supplied_key = field;
        }
        if (bit == 8) {
            if (!cJSON_IsBool(field)) return fail(error, "clearApiKey must be boolean.");
            clear_key = cJSON_IsTrue(field);
        }
    }
    const bool fresh_key = supplied_key && supplied_key->valuestring[0];
    if (clear_key && fresh_key) return fail(error, "Cannot both supply and clear an API key.");
    if ((endpoint != old_endpoint || (provider && old_provider && *provider != *old_provider)) &&
        !old_key.empty() && !fresh_key && !clear_key)
        return fail(error, "Changing a provider or endpoint requires a fresh API key or explicit clearing of the saved key.");
    if (clear_key) key.clear();
    else if (fresh_key) key.assign(supplied_key->valuestring);
    return true;
}

bool patch_tts(const cJSON *group, BuddySettings &settings, std::string &error)
{
    if (!cJSON_IsObject(group)) return fail(error, "Speech settings must be an object.");
    unsigned seen = 0;
    for (const cJSON *field = group->child; field; field = field->next) {
        unsigned bit = 0;
        if (!field->string) return fail(error, "Invalid speech settings field.");
        if (std::strcmp(field->string, "backend") == 0) bit = 1;
        else if (std::strcmp(field->string, "voice") == 0) bit = 2;
        else return fail(error, "Unknown speech settings field.");
        if (seen & bit) return fail(error, "Duplicate speech settings field.");
        seen |= bit;
        if (bit == 1 && !string_field(field, 5, settings.tts_backend, error)) return false;
        if (bit == 2 && !string_field(field, tts_voice_limit, settings.tts_voice, error)) return false;
    }
    return true;
}

bool patch_buddy(const cJSON *group, BuddySettings &settings, std::string &error)
{
    if (!cJSON_IsObject(group)) return fail(error, "Buddy settings must be an object.");
    unsigned seen = 0;
    for (const cJSON *field = group->child; field; field = field->next) {
        unsigned bit = 0;
        if (!field->string) return fail(error, "Invalid settings field.");
        if (std::strcmp(field->string, "name") == 0) bit = 1;
        else if (std::strcmp(field->string, "instructions") == 0) bit = 2;
        else if (std::strcmp(field->string, "readAloud") == 0) bit = 4;
        else if (std::strcmp(field->string, "scene") == 0) bit = 8;
        else return fail(error, "Unknown Buddy settings field.");
        if (seen & bit) return fail(error, "Duplicate settings field.");
        seen |= bit;
        if (bit == 1 && !string_field(field, name_limit, settings.name, error)) return false;
        if (bit == 2 && !string_field(field, instructions_limit, settings.instructions, error)) return false;
        if (bit == 4) {
            if (!cJSON_IsBool(field)) return fail(error, "readAloud must be boolean.");
            settings.read_aloud = cJSON_IsTrue(field);
        }
        if (bit == 8 && !string_field(field, scene_limit, settings.scene, error)) return false;
    }
    return true;
}

bool enum_field(const cJSON *field, const char *const (&names)[3], uint8_t &target, std::string &error)
{
    if (cJSON_IsString(field) && field->valuestring) {
        for (uint8_t i = 0; i < 3; ++i) {
            if (std::strcmp(field->valuestring, names[i]) == 0) { target = i; return true; }
        }
    }
    return fail(error, "Preference must be one of the listed options.");
}

bool number_field(const cJSON *field, uint8_t &target, std::string &error)
{
    if (!cJSON_IsNumber(field) || field->valuedouble < 0 || field->valuedouble > 255 ||
        field->valuedouble != static_cast<double>(static_cast<int>(field->valuedouble)))
        return fail(error, "Numeric preferences must be whole numbers.");
    target = static_cast<uint8_t>(field->valueint);
    return true;
}

bool bool_field(const cJSON *field, bool &target, std::string &error)
{
    if (!cJSON_IsBool(field)) return fail(error, "Switch preferences must be boolean.");
    target = cJSON_IsTrue(field);
    return true;
}

bool patch_prefs(const cJSON *group, BuddyPreferences &prefs, std::string &error)
{
    if (!cJSON_IsObject(group)) return fail(error, "Preferences must be an object.");
    static const char *const names[] = {"userName", "timezone", "voiceStyle", "speechRate", "captions",
                                        "brightness", "dimAtNight", "screenSleep", "persona", "volume"};
    unsigned seen = 0;
    for (const cJSON *field = group->child; field; field = field->next) {
        if (!field->string) return fail(error, "Invalid settings field.");
        unsigned index = 0;
        while (index < 10 && std::strcmp(field->string, names[index]) != 0) ++index;
        if (index == 10) return fail(error, "Unknown preference field.");
        if (seen & (1u << index)) return fail(error, "Duplicate settings field.");
        seen |= 1u << index;
        bool ok = true;
        switch (index) {
            case 0: ok = string_field(field, name_limit, prefs.user_name, error); break;
            case 1: ok = string_field(field, timezone_limit, prefs.timezone, error); break;
            case 2: ok = enum_field(field, buddy_voice_style_names, prefs.voice_style, error); break;
            case 3: ok = number_field(field, prefs.speech_rate, error); break;
            case 4: ok = bool_field(field, prefs.captions, error); break;
            case 5: ok = number_field(field, prefs.brightness, error); break;
            case 6: ok = bool_field(field, prefs.dim_at_night, error); break;
            case 7: ok = enum_field(field, buddy_screen_sleep_names, prefs.screen_sleep, error); break;
            case 8: ok = enum_field(field, buddy_persona_names, prefs.persona, error); break;
            case 9: ok = number_field(field, prefs.volume, error); break;
        }
        if (!ok) return false;
    }
    return true;
}

bool save_prefs(nvs_handle_t handle, const BuddyPreferences &prefs)
{
    const size_t size = prefs_fixed + prefs.user_name.size() + prefs.timezone.size();
    Buffer bytes(static_cast<uint8_t *>(std::malloc(size)), &std::free);
    if (!bytes) return false;
    uint8_t *b = bytes.get();
    std::memcpy(b, "UBP1", 4);
    b[4] = 1;
    const uint8_t values[8] = {prefs.voice_style, prefs.speech_rate, prefs.captions, prefs.brightness,
                               prefs.dim_at_night, prefs.screen_sleep, prefs.persona, prefs.volume};
    std::memcpy(b + 5, values, sizeof(values));
    b[13] = static_cast<uint8_t>(prefs.user_name.size());
    b[14] = static_cast<uint8_t>(prefs.timezone.size());
    std::memcpy(b + prefs_fixed, prefs.user_name.data(), prefs.user_name.size());
    std::memcpy(b + prefs_fixed + prefs.user_name.size(), prefs.timezone.data(), prefs.timezone.size());
    return nvs_set_blob(handle, prefs_key, b, size) == ESP_OK;
}

// Preferences live in their own blob so older settings records keep loading.
void load_prefs(nvs_handle_t handle, BuddyPreferences &prefs)
{
    uint8_t b[prefs_fixed_v2 + name_limit + timezone_limit];
    size_t size = sizeof(b);
    if (nvs_get_blob(handle, prefs_key, b, &size) != ESP_OK || size < prefs_fixed ||
        std::memcmp(b, "UBP1", 4) != 0 || (b[4] != 1 && b[4] != 2)) return;
    const bool v2 = b[4] == 2;
    const size_t fixed = v2 ? prefs_fixed_v2 : prefs_fixed;
    if (size < fixed) return;
    const size_t name_len = b[fixed - 2], tz_len = b[fixed - 1];
    if (size != fixed + name_len + tz_len) return;
    BuddyPreferences loaded;
    loaded.voice_style = b[5];
    loaded.speech_rate = b[6];
    loaded.captions = b[7] != 0;
    loaded.brightness = b[8];
    loaded.dim_at_night = b[9] != 0;
    loaded.screen_sleep = b[10];
    loaded.persona = b[11];
    loaded.volume = b[12];
    loaded.user_name.assign(reinterpret_cast<const char *>(b + fixed), name_len);
    loaded.timezone.assign(reinterpret_cast<const char *>(b + fixed + name_len), tz_len);
    std::string ignored;
    if (validate_prefs(loaded, ignored)) prefs = std::move(loaded);
}

bool save_tts(nvs_handle_t handle, const BuddySettings &settings)
{
    uint8_t bytes[tts_fixed + tts_voice_limit] = {};
    std::memcpy(bytes, "UBT2", 4);
    bytes[4] = 2;
    bytes[5] = settings.tts_backend == "codex" ? 1 : 0;
    bytes[6] = static_cast<uint8_t>(settings.tts_voice.size());
    std::memcpy(bytes + tts_fixed, settings.tts_voice.data(), settings.tts_voice.size());
    return nvs_set_blob(handle, tts_key, bytes, tts_fixed + settings.tts_voice.size()) == ESP_OK;
}

// Migrate only the legacy voice choice; bridge addresses and keys are obsolete.
bool load_tts(nvs_handle_t handle, BuddySettings &settings, std::string &error)
{
    size_t size = 0;
    const esp_err_t result = nvs_get_blob(handle, tts_key, nullptr, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK || size < tts_fixed ||
        size > legacy_tts_fixed + endpoint_limit + tts_voice_limit + key_limit)
        return fail(error, "Saved speech record is unreadable; boot defaults retained.");
    Buffer bytes(static_cast<uint8_t *>(std::malloc(size)), &std::free);
    if (!bytes) return fail(error, "Not enough memory to load speech settings; boot defaults retained.");
    const size_t expected = size;
    if (nvs_get_blob(handle, tts_key, bytes.get(), &size) != ESP_OK || size != expected) {
        mbedtls_platform_zeroize(bytes.get(), expected);
        return fail(error, "Saved speech record is unreadable; boot defaults retained.");
    }
    const uint8_t *b = bytes.get();
    const bool legacy = size >= legacy_tts_fixed && std::memcmp(b, "UBT1", 4) == 0 && b[4] == 1;
    size_t voice_size = b[6], voice_offset = tts_fixed;
    bool valid = std::memcmp(b, "UBT2", 4) == 0 && b[4] == 2 && b[5] <= 1 &&
                 voice_size <= tts_voice_limit && size == tts_fixed + voice_size;
    if (legacy) {
        const size_t endpoint_size = b[6] | (static_cast<size_t>(b[7]) << 8);
        const size_t key_size = b[9] | (static_cast<size_t>(b[10]) << 8);
        voice_size = b[8];
        voice_offset = legacy_tts_fixed + endpoint_size;
        valid = b[5] <= 1 && endpoint_size <= endpoint_limit && key_size <= key_limit &&
                voice_size <= tts_voice_limit && size == voice_offset + voice_size + key_size;
    }
    if (valid) {
        settings.tts_backend = b[5] == 1 ? "codex" : "local";
        settings.tts_voice.assign(reinterpret_cast<const char *>(b + voice_offset), voice_size);
    }
    mbedtls_platform_zeroize(bytes.get(), expected);
    if (!valid || !validate_tts(settings, error))
        return fail(error, "Saved speech settings failed validation; boot defaults retained.");
    if (legacy) {
        NvsHandle migration;
        if (nvs_open(storage_namespace, NVS_READWRITE, &migration.value) != ESP_OK ||
            !save_tts(migration.value, settings) || nvs_commit(migration.value) != ESP_OK)
            return fail(error, "Could not migrate speech settings; boot defaults retained.");
    }
    return true;
}

bool patch_live(const cJSON *group, const BuddySettings &current, BuddySettings &settings, std::string &error) {
    if (!cJSON_IsObject(group)) return fail(error, "Live deployment settings must be an object.");
    unsigned seen = 0;
    bool clear = false, supplied = false;
    std::string key;
    for (const cJSON *field = group->child; field; field = field->next) {
        unsigned bit = 0;
        if (!field->string) return fail(error, "Invalid live deployment setting.");
        if (!std::strcmp(field->string, "provider")) bit = 1;
        else if (!std::strcmp(field->string, "model")) bit = 2;
        else if (!std::strcmp(field->string, "protocol")) bit = 4;
        else if (!std::strcmp(field->string, "endpoint")) bit = 8;
        else if (!std::strcmp(field->string, "apiKey")) bit = 16;
        else if (!std::strcmp(field->string, "clearApiKey")) bit = 32;
        else return fail(error, "Unknown live deployment setting.");
        if (seen & bit) return fail(error, "Duplicate live deployment setting.");
        seen |= bit;
        if (bit == 1 && !string_field(field, provider_limit, settings.live.provider, error)) return false;
        if (bit == 2 && !string_field(field, model_limit, settings.live.model, error)) return false;
        if (bit == 4 && !string_field(field, 32, settings.live.protocol, error)) return false;
        if (bit == 8 && !string_field(field, endpoint_limit, settings.live.endpoint, error)) return false;
        if (bit == 16) { supplied = true; if (!string_field(field, key_limit, key, error)) return false; }
        if (bit == 32) {
            if (!cJSON_IsBool(field)) return fail(error, "clearApiKey must be boolean.");
            clear = cJSON_IsTrue(field);
        }
    }
    if (clear && !key.empty()) return fail(error, "Cannot supply and clear the native deployment key together.");
    if ((settings.live.provider != current.live.provider || settings.live.endpoint != current.live.endpoint) &&
        !current.live.api_key.empty() && key.empty() && !clear)
        return fail(error, "Changing a live provider or endpoint requires a fresh key or explicit key clearing.");
    if (clear) settings.live.api_key.clear();
    else if (supplied && !key.empty()) settings.live.api_key = std::move(key);
    return true;
}

bool patch_niko(const cJSON *group, const BuddyNikoProfile &current,
                BuddyNikoProfile &next, std::string &error) {
    if (!cJSON_IsObject(group)) return fail(error, "Niko settings must be an object.");
    static const char *names[] = {"enabled", "controller", "tlsServerName", "username", "jwt", "caPem", "allowedLights"};
    unsigned seen = 0;
    for (const cJSON *field = group->child; field; field = field->next) {
        unsigned index = 0;
        if (!field->string) return fail(error, "Invalid Niko setting.");
        while (index < 7 && std::strcmp(field->string, names[index])) ++index;
        if (index == 7) return fail(error, "Unknown Niko setting.");
        if (seen & (1u << index)) return fail(error, "Duplicate Niko setting.");
        seen |= 1u << index;
        if (index == 0 && !bool_field(field, next.enabled, error)) return false;
        if (index == 1 && !string_field(field, niko_host_limit, next.controller, error)) return false;
        if (index == 2 && !string_field(field, niko_host_limit, next.tls_server_name, error)) return false;
        if (index == 3 && !string_field(field, niko_username_limit, next.username, error)) return false;
        if (index == 4 && !string_field(field, niko_jwt_limit, next.jwt, error)) return false;
        if (index == 5 && !string_field(field, niko_ca_limit, next.ca_pem, error)) return false;
        if (index == 6) {
            if (!cJSON_IsArray(field) || cJSON_GetArraySize(field) > int(niko_fixture_limit))
                return fail(error, "Niko allowedLights must contain at most 16 UUIDs.");
            next.allowed_lights.clear();
            for (const cJSON *id = field->child; id; id = id->next) {
                std::string value;
                if (!string_field(id, 36, value, error)) return false;
                next.allowed_lights.push_back(std::move(value));
            }
        }
    }
    if (next.controller != current.controller || next.tls_server_name != current.tls_server_name) {
        const bool fresh = (seen & 56u) == 56u && !next.username.empty() && !next.jwt.empty() && !next.ca_pem.empty();
        const bool cleared = !next.enabled && (seen & 56u) == 56u &&
            next.username.empty() && next.jwt.empty() && next.ca_pem.empty();
        if (!fresh && !cleared)
            return fail(error, "Changing Niko controller or TLS identity requires fresh username, token and CA, or explicit clearing with Niko disabled.");
        // An old controller's fixture UUIDs are never authority on a replacement.
        if (!(seen & 64u)) next.allowed_lights.clear();
    }
    return validate_niko(next, error);
}


bool save_conversation(nvs_handle_t handle, const BuddySettings &settings) {
    const std::string *values[] = {&settings.chat_provider, &settings.live.provider, &settings.live.model,
                                  &settings.live.endpoint, &settings.live.api_key};
    size_t size = conversation_header;
    for (const auto *value : values) size += value->size();
    SecretRecord record(size);
    if (!record.bytes) return false;
    auto *bytes = record.bytes.get();
    std::memcpy(bytes, "UBC1", 4); bytes[4] = 1; bytes[5] = 0;
    for (unsigned i = 0; i < 3; ++i) if (settings.conversation_mode == conversation_modes[i]) bytes[5] = i;
    size_t offset = conversation_header;
    for (unsigned i = 0; i < conversation_fields; ++i) {
        const size_t length = values[i]->size();
        bytes[6 + i * 2] = length; bytes[7 + i * 2] = length >> 8;
        std::memcpy(bytes + offset, values[i]->data(), length); offset += length;
    }
    return nvs_set_blob(handle, conversation_key, bytes, size) == ESP_OK;
}

bool load_conversation(nvs_handle_t handle, BuddySettings &settings, std::string &error) {
    size_t size = 0;
    const esp_err_t found = nvs_get_blob(handle, conversation_key, nullptr, &size);
    if (found == ESP_ERR_NVS_NOT_FOUND) {
        // One-time stored migration; standalone read-aloud remains a separate profile.
        settings.conversation_mode = settings.tts_backend == "codex" ? "codex_live" : "text_pipeline";
        settings.chat_provider = ollama_endpoint(settings.chat_endpoint) ? "ollama" : "custom";
        NvsHandle migration;
        if (nvs_open(storage_namespace, NVS_READWRITE, &migration.value) != ESP_OK ||
            !save_conversation(migration.value, settings) || nvs_commit(migration.value) != ESP_OK)
            return fail(error, "Could not migrate conversation settings; boot defaults retained.");
        return true;
    }
    if (found != ESP_OK || size < conversation_header || size > conversation_limit)
        return fail(error, "Saved conversation settings are unreadable; boot defaults retained.");
    SecretRecord record(size);
    if (!record.bytes) return fail(error, "Not enough memory to load conversation settings.");
    const size_t expected = size;
    auto *bytes = record.bytes.get();
    if (nvs_get_blob(handle, conversation_key, bytes, &size) != ESP_OK || size != expected ||
        std::memcmp(bytes, "UBC1", 4) || bytes[4] != 1 || bytes[5] >= 3)
        return fail(error, "Saved conversation settings failed validation; boot defaults retained.");
    std::string *values[] = {&settings.chat_provider, &settings.live.provider, &settings.live.model,
                            &settings.live.endpoint, &settings.live.api_key};
    const size_t limits[] = {provider_limit, provider_limit, model_limit, endpoint_limit, key_limit};
    size_t offset = conversation_header;
    for (unsigned i = 0; i < conversation_fields; ++i) {
        const size_t length = bytes[6 + i * 2] | (size_t(bytes[7 + i * 2]) << 8);
        if (length > limits[i] || length > size - offset)
            return fail(error, "Saved conversation field exceeds its bound.");
        values[i]->assign(reinterpret_cast<const char *>(bytes + offset), length); offset += length;
    }
    settings.conversation_mode = conversation_modes[bytes[5]];
    return (offset == size && validate_conversation(settings, error)) ||
           fail(error, "Saved conversation settings failed validation; boot defaults retained.");
}

bool save_niko(nvs_handle_t handle, const BuddyNikoProfile &niko) {
    const std::string *values[] = {&niko.controller, &niko.tls_server_name, &niko.username, &niko.jwt, &niko.ca_pem};
    size_t size = niko_header + 36 * niko.allowed_lights.size();
    for (const auto *value : values) size += value->size();
    SecretRecord record(size);
    if (!record.bytes) return false;
    auto *b = record.bytes.get();
    std::memcpy(b, "UBN1", 4); b[4] = 1; b[5] = niko.enabled; b[6] = niko.allowed_lights.size();
    size_t offset = niko_header;
    for (size_t i = 0; i < niko_fields; ++i) {
        const size_t length = values[i]->size();
        b[7 + 2*i] = length; b[8 + 2*i] = length >> 8;
        std::memcpy(b + offset, values[i]->data(), length); offset += length;
    }
    for (const auto &id : niko.allowed_lights) { std::memcpy(b + offset, id.data(), 36); offset += 36; }
    return nvs_set_blob(handle, "niko", b, size) == ESP_OK;
}

bool load_niko(nvs_handle_t handle, BuddyNikoProfile &niko, std::string &error) {
    size_t size = 0;
    const esp_err_t found = nvs_get_blob(handle, "niko", nullptr, &size);
    if (found == ESP_ERR_NVS_NOT_FOUND) return true;
    if (found != ESP_OK || size < niko_header || size > niko_record_limit)
        return fail(error, "Saved Niko profile is unreadable; boot defaults retained.");
    SecretRecord record(size);
    if (!record.bytes) return fail(error, "Not enough memory to load Niko profile.");
    auto *b = record.bytes.get(); const size_t expected = size;
    if (nvs_get_blob(handle, "niko", b, &size) != ESP_OK || size != expected ||
        std::memcmp(b, "UBN1", 4) || b[4] != 1 || b[5] > 1 || b[6] > niko_fixture_limit)
        return fail(error, "Saved Niko version or flags are invalid; boot defaults retained.");
    BuddyNikoProfile loaded; loaded.enabled = b[5] == 1;
    std::string *values[] = {&loaded.controller, &loaded.tls_server_name, &loaded.username, &loaded.jwt, &loaded.ca_pem};
    const size_t limits[] = {niko_host_limit, niko_host_limit, niko_username_limit, niko_jwt_limit, niko_ca_limit};
    size_t offset = niko_header;
    for (size_t i = 0; i < niko_fields; ++i) {
        const size_t length = b[7 + 2*i] | (size_t(b[8 + 2*i]) << 8);
        if (length > limits[i] || length > size - offset)
            return fail(error, "Saved Niko field exceeds its bound; boot defaults retained.");
        values[i]->assign(reinterpret_cast<const char *>(b + offset), length); offset += length;
    }
    if (size - offset != 36 * size_t(b[6]))
        return fail(error, "Saved Niko fixture record is invalid; boot defaults retained.");
    for (size_t i = 0; i < b[6]; ++i) {
        loaded.allowed_lights.emplace_back(reinterpret_cast<const char *>(b + offset), 36); offset += 36;
    }
    if (!validate_niko(loaded, error)) return false;
    niko = std::move(loaded);
    return true;
}

bool storage_capacity(const BuddySettings &settings, size_t base_size, std::string &error) {
    const size_t conversation_size = conversation_header + settings.chat_provider.size() + settings.live.provider.size() +
        settings.live.model.size() + settings.live.endpoint.size() + settings.live.api_key.size();
    const size_t niko_size = niko_header + settings.niko.controller.size() + settings.niko.tls_server_name.size() +
        settings.niko.username.size() + settings.niko.jwt.size() + settings.niko.ca_pem.size() + 36 * settings.niko.allowed_lights.size();
    const size_t sizes[] = {base_size, prefs_fixed + settings.prefs.user_name.size() + settings.prefs.timezone.size(),
        tts_fixed + settings.tts_voice.size(), 11 + settings.tavily_api_key.size() + settings.parallel_api_key.size(),
        conversation_size, niko_size};
    // Blob v2 needs entries for the index, each <=4000-byte chunk and each
    // 32 bytes of data. Reserve a complete page rather than assuming a raw
    // byte total fits the owner's existing 24 KiB NVS partition.
    size_t required = 126;
    for (size_t size : sizes) required += 1 + (size + 3999) / 4000 + (size + 31) / 32;
    nvs_stats_t stats{};
    if (nvs_get_stats(nullptr, &stats) != ESP_OK)
        return fail(error, "Could not inspect settings storage capacity; nothing written.");
    if (required > stats.free_entries)
        return fail(error, "NVS settings capacity is too small for this update and its safety reserve; remove unused saved keys or use a smaller complete certificate chain. Nothing written.");
    return true;
}

bool patch_search(const cJSON *group, BuddySettings &settings, std::string &error)
{
    if (!cJSON_IsObject(group)) return fail(error, "Search settings must be an object.");
    unsigned seen = 0;
    bool clear_tavily = false, clear_parallel = false;
    std::string tavily, parallel;
    for (const cJSON *item = group->child; item; item = item->next) {
        unsigned bit = 0;
        if (!item->string) return fail(error, "Invalid search setting.");
        if (!std::strcmp(item->string, "provider")) bit = 1;
        else if (!std::strcmp(item->string, "enabled")) bit = 2;
        else if (!std::strcmp(item->string, "tavilyApiKey")) bit = 4;
        else if (!std::strcmp(item->string, "parallelApiKey")) bit = 8;
        else if (!std::strcmp(item->string, "clearTavilyApiKey")) bit = 16;
        else if (!std::strcmp(item->string, "clearParallelApiKey")) bit = 32;
        else return fail(error, "Unknown search setting.");
        if (seen & bit) return fail(error, "Duplicate search setting.");
        seen |= bit;
        if (bit == 1 && !string_field(item, 8, settings.search_provider, error)) return false;
        if (bit == 4 && !string_field(item, key_limit, tavily, error)) return false;
        if (bit == 8 && !string_field(item, key_limit, parallel, error)) return false;
        if (bit == 2 || bit == 16 || bit == 32) {
            if (!cJSON_IsBool(item)) return fail(error, "Search flags must be boolean.");
            if (bit == 2) settings.search_enabled = cJSON_IsTrue(item);
            if (bit == 16) clear_tavily = cJSON_IsTrue(item);
            if (bit == 32) clear_parallel = cJSON_IsTrue(item);
        }
    }
    if ((clear_tavily && !tavily.empty()) || (clear_parallel && !parallel.empty()))
        return fail(error, "Cannot set and clear the same search key.");
    if (clear_tavily) settings.tavily_api_key.clear();
    else if (!tavily.empty()) settings.tavily_api_key = std::move(tavily);
    if (clear_parallel) settings.parallel_api_key.clear();
    else if (!parallel.empty()) settings.parallel_api_key = std::move(parallel);
    return true;
}

bool save_search(nvs_handle_t handle, const BuddySettings &settings)
{
    constexpr size_t fixed = 11;
    const size_t a = settings.tavily_api_key.size(), b = settings.parallel_api_key.size(), size = fixed+a+b;
    Buffer data(static_cast<uint8_t *>(std::malloc(size)), &std::free);
    if (!data) return false;
    std::memcpy(data.get(), "UBW1", 4); data.get()[4] = 1;
    data.get()[5] = settings.search_provider == "parallel"; data.get()[6] = settings.search_enabled;
    data.get()[7] = a; data.get()[8] = a >> 8; data.get()[9] = b; data.get()[10] = b >> 8;
    std::memcpy(data.get()+fixed, settings.tavily_api_key.data(), a);
    std::memcpy(data.get()+fixed+a, settings.parallel_api_key.data(), b);
    const bool saved = nvs_set_blob(handle, "search", data.get(), size) == ESP_OK;
    mbedtls_platform_zeroize(data.get(), size);
    return saved;
}

bool load_search(nvs_handle_t handle, BuddySettings &settings, std::string &error)
{
    constexpr size_t fixed = 11;
    size_t size = 0; const esp_err_t found = nvs_get_blob(handle, "search", nullptr, &size);
    if (found == ESP_ERR_NVS_NOT_FOUND) return true;
    if (found != ESP_OK || size < fixed || size > fixed+2*key_limit)
        return fail(error, "Saved search settings are unreadable; boot defaults retained.");
    Buffer data(static_cast<uint8_t *>(std::malloc(size)), &std::free);
    if (!data) return fail(error, "Not enough memory to load search settings.");
    const size_t expected = size;
    if (nvs_get_blob(handle, "search", data.get(), &size) != ESP_OK || size != expected) {
        mbedtls_platform_zeroize(data.get(), expected);
        return fail(error, "Could not read search settings; boot defaults retained.");
    }
    const size_t a = data.get()[7] | (size_t(data.get()[8])<<8), b = data.get()[9] | (size_t(data.get()[10])<<8);
    const bool valid = !std::memcmp(data.get(), "UBW1", 4) && data.get()[4] == 1 && data.get()[5] <= 1 && data.get()[6] <= 1 &&
        a <= key_limit && b <= key_limit && fixed+a+b == size;
    if (valid) {
        settings.search_provider = data.get()[5] ? "parallel" : "tavily";
        settings.search_enabled = data.get()[6];
        settings.tavily_api_key.assign(reinterpret_cast<const char *>(data.get()+fixed), a);
        settings.parallel_api_key.assign(reinterpret_cast<const char *>(data.get()+fixed+a), b);
    }
    mbedtls_platform_zeroize(data.get(), size);
    return valid || fail(error, "Saved search settings failed validation; boot defaults retained.");
}

void fields(const BuddySettings &settings, const std::string *(&result)[field_count])
{
    result[0] = &settings.chat_endpoint;
    result[1] = &settings.chat_model;
    result[2] = &settings.chat_api_key;
    result[3] = &settings.voice_endpoint;
    result[4] = &settings.voice_model;
    result[5] = &settings.voice_api_key;
    result[6] = &settings.name;
    result[7] = &settings.instructions;
    result[8] = &settings.scene;
}

bool add_string(cJSON *root, const char *name, const std::string &value)
{
    cJSON *item = cJSON_CreateStringReference(value.c_str());
    if (item && cJSON_AddItemToObject(root, name, item)) return true;
    cJSON_Delete(item);
    return false;
}

bool add_provider(cJSON *root, const char *name, const std::string &endpoint,
                  const std::string &model, const std::string &key)
{
    cJSON *group = cJSON_AddObjectToObject(root, name);
    return group && add_string(group, "endpoint", endpoint) &&
           add_string(group, "model", model) &&
           cJSON_AddBoolToObject(group, "apiKeyConfigured", !key.empty());
}
} // namespace

const char *const buddy_voice_style_names[3] = {"warm", "bright", "soft"};
const char *const buddy_screen_sleep_names[3] = {"1min", "5min", "never"};
const char *const buddy_persona_names[3] = {"calm", "chatty", "focused"};

BuddySettings buddy_settings_defaults()
{
    BuddySettings settings;
    settings.chat_endpoint = "https://ollama.com/v1/chat/completions";
    settings.chat_model = CONFIG_UNRAVEL_OLLAMA_MODEL;
    settings.chat_api_key = CONFIG_UNRAVEL_OLLAMA_API_KEY;
    settings.voice_endpoint = CONFIG_UNRAVEL_STT_URL;
    settings.voice_model = CONFIG_UNRAVEL_STT_MODEL;
    settings.voice_api_key = CONFIG_UNRAVEL_STT_API_KEY;
#if CONFIG_UNRAVEL_TTS_CODEX
    settings.tts_backend = "codex";
    settings.conversation_mode = "codex_live";
    settings.tts_voice = CONFIG_UNRAVEL_TTS_VOICE;
#endif
    settings.name = "Arvel";
    settings.instructions = "Be warm, practical and concise. Prefer short English paragraphs that fit a narrow screen and sound natural aloud.";
    normalize_model(settings);
    return settings;
}

bool buddy_settings_patch(const cJSON *patch, const BuddySettings &current,
                          BuddySettings &candidate, std::string &error)
{
    error.clear();
    if (!cJSON_IsObject(patch)) return fail(error, "Settings patch must be an object.");
    BuddySettings next = current;
    unsigned seen = 0;
    for (const cJSON *group = patch->child; group; group = group->next) {
        unsigned bit = 0;
        if (!group->string) return fail(error, "Invalid settings group.");
        if (std::strcmp(group->string, "chat") == 0) bit = 1;
        else if (std::strcmp(group->string, "voice") == 0) bit = 2;
        else if (std::strcmp(group->string, "buddy") == 0) bit = 4;
        else if (std::strcmp(group->string, "preferences") == 0) bit = 8;
        else if (std::strcmp(group->string, "tts") == 0) bit = 16;
        else if (std::strcmp(group->string, "search") == 0) bit = 32;
        else if (std::strcmp(group->string, "conversationMode") == 0) bit = 64;
        else if (std::strcmp(group->string, "live") == 0) bit = 128;
        else if (std::strcmp(group->string, "niko") == 0) bit = 256;
        else return fail(error, "Unknown settings group.");
        if (seen & bit) return fail(error, "Duplicate settings group.");
        seen |= bit;
        if (bit == 1 && !patch_provider(group, current.chat_endpoint, current.chat_api_key,
                                       next.chat_endpoint, next.chat_model, next.chat_api_key, error,
                                       &next.chat_provider, &current.chat_provider)) return false;
        if (bit == 2 && !patch_provider(group, current.voice_endpoint, current.voice_api_key,
                                       next.voice_endpoint, next.voice_model, next.voice_api_key, error)) return false;
        if (bit == 4 && !patch_buddy(group, next, error)) return false;
        if (bit == 8 && !patch_prefs(group, next.prefs, error)) return false;
        if (bit == 16 && !patch_tts(group, next, error)) return false;
        if (bit == 32 && !patch_search(group, next, error)) return false;
        if (bit == 64 && !string_field(group, 16, next.conversation_mode, error)) return false;
        if (bit == 128 && !patch_live(group, current, next, error)) return false;
        if (bit == 256 && !patch_niko(group, current.niko, next.niko, error)) return false;
    }
    normalize_model(next);
    if (!validate(next, error)) return false;
    candidate = std::move(next);
    return true;
}

bool buddy_settings_save(const BuddySettings &settings, std::string &error)
{
    error.clear();
    if (!validate(settings, error)) return false;
    const std::string *values[field_count];
    fields(settings, values);
    size_t size = header_size;
    for (const auto *value : values) size += value->size();
    SecretRecord record(size);
    auto &bytes = record.bytes;
    if (!bytes) return fail(error, "Not enough memory to save settings.");
    // Preserve the original versioned settings blob; preferences and TTS have separate blobs.
    std::memcpy(bytes.get(), "UBS1", 4);
    bytes.get()[4] = 1;
    bytes.get()[5] = settings.read_aloud ? 1 : 0;
    size_t offset = header_size;
    for (size_t i = 0; i < field_count; ++i) {
        const size_t length = values[i]->size();
        bytes.get()[6 + 2 * i] = static_cast<uint8_t>(length);
        bytes.get()[7 + 2 * i] = static_cast<uint8_t>(length >> 8);
        std::memcpy(bytes.get() + offset, values[i]->data(), length);
        offset += length;
    }
    if (!storage_capacity(settings, size, error)) return false;
    NvsHandle handle;
    if (nvs_open(storage_namespace, NVS_READWRITE, &handle.value) != ESP_OK)
        return fail(error, "Could not open settings storage.");
    if (nvs_set_blob(handle.value, storage_key, bytes.get(), size) != ESP_OK ||
        !save_prefs(handle.value, settings.prefs) || !save_tts(handle.value, settings) ||
        !save_search(handle.value, settings) || !save_conversation(handle.value, settings) ||
        !save_niko(handle.value, settings.niko))
        return fail(error, "Could not write settings storage.");
    if (nvs_commit(handle.value) != ESP_OK)
        return fail(error, "Could not commit settings storage.");
    return true;
}

bool buddy_settings_load(BuddySettings &settings, std::string &error)
{
    error.clear();
    NvsHandle handle;
    esp_err_t result = nvs_open(storage_namespace, NVS_READONLY, &handle.value);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return fail(error, "Could not open settings storage; boot defaults retained.");
    size_t size = 0;
    result = nvs_get_blob(handle.value, storage_key, nullptr, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result != ESP_OK) return fail(error, "Could not read settings storage; boot defaults retained.");
    if (size < header_size || size > record_limit)
        return fail(error, "Saved settings record has an invalid size; boot defaults retained.");
    SecretRecord record(size);
    auto &bytes = record.bytes;
    if (!bytes) return fail(error, "Not enough memory to load settings; boot defaults retained.");
    const size_t expected_size = size;
    if (nvs_get_blob(handle.value, storage_key, bytes.get(), &size) != ESP_OK || size != expected_size)
        return fail(error, "Could not read settings storage; boot defaults retained.");
    if (std::memcmp(bytes.get(), "UBS1", 4) != 0 || bytes.get()[4] != 1 || bytes.get()[5] > 1)
        return fail(error, "Saved settings version or flags are invalid; boot defaults retained.");
    BuddySettings loaded;
    std::string *values[field_count] = {&loaded.chat_endpoint, &loaded.chat_model, &loaded.chat_api_key,
                                      &loaded.voice_endpoint, &loaded.voice_model, &loaded.voice_api_key,
                                      &loaded.name, &loaded.instructions, &loaded.scene};
    constexpr size_t limits[field_count] = {endpoint_limit, model_limit, key_limit, endpoint_limit,
                                          model_limit, key_limit, name_limit, instructions_limit, scene_limit};
    size_t offset = header_size;
    for (size_t i = 0; i < field_count; ++i) {
        const size_t length = bytes.get()[6 + 2 * i] | (static_cast<size_t>(bytes.get()[7 + 2 * i]) << 8);
        if (length > limits[i] || length > size - offset)
            return fail(error, "Saved settings fields are invalid; boot defaults retained.");
        values[i]->assign(reinterpret_cast<const char *>(bytes.get() + offset), length);
        offset += length;
    }
    if (offset != size) return fail(error, "Saved settings contains trailing data; boot defaults retained.");
    loaded.read_aloud = bytes.get()[5] == 1;
    load_prefs(handle.value, loaded.prefs);
    if (!load_tts(handle.value, loaded, error)) return false;
    if (!load_search(handle.value, loaded, error)) return false;
    if (!load_conversation(handle.value, loaded, error)) return false;
    if (!load_niko(handle.value, loaded.niko, error)) return false;
    normalize_model(loaded);
    if (!validate(loaded, error)) {
        error = "Saved settings failed validation; boot defaults retained.";
        return false;
    }
    settings = std::move(loaded);
    return true;
}

cJSON *buddy_settings_public_json(const BuddySettings &settings)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return nullptr;
    bool okay = add_provider(root, "chat", settings.chat_endpoint, settings.chat_model, settings.chat_api_key) &&
                add_provider(root, "voice", settings.voice_endpoint, settings.voice_model, settings.voice_api_key);
    cJSON *chat = okay ? cJSON_GetObjectItemCaseSensitive(root, "chat") : nullptr;
    okay = chat && add_string(chat, "provider", settings.chat_provider) &&
        add_string(root, "conversationMode", settings.conversation_mode);
    cJSON *live = okay ? cJSON_AddObjectToObject(root, "live") : nullptr;
    okay = live && add_string(live, "provider", settings.live.provider) &&
        add_string(live, "model", settings.live.model) && add_string(live, "protocol", settings.live.protocol) &&
        add_string(live, "endpoint", settings.live.endpoint) &&
        cJSON_AddBoolToObject(live, "apiKeyConfigured", !settings.live.api_key.empty());
    cJSON *tts = okay ? cJSON_AddObjectToObject(root, "tts") : nullptr;
    okay = tts && add_string(tts, "backend", settings.tts_backend) &&
           add_string(tts, "voice", settings.tts_voice);
    cJSON *search = okay ? cJSON_AddObjectToObject(root, "search") : nullptr;
    okay = search && add_string(search, "provider", settings.search_provider) &&
        cJSON_AddBoolToObject(search, "enabled", settings.search_enabled) &&
        cJSON_AddBoolToObject(search, "tavilyApiKeyConfigured", !settings.tavily_api_key.empty()) &&
        cJSON_AddBoolToObject(search, "parallelApiKeyConfigured", !settings.parallel_api_key.empty());
    cJSON *niko = okay ? cJSON_AddObjectToObject(root, "niko") : nullptr;
    okay = niko && cJSON_AddBoolToObject(niko, "enabled", settings.niko.enabled) &&
        add_string(niko, "controller", settings.niko.controller) &&
        add_string(niko, "tlsServerName", settings.niko.tls_server_name) &&
        cJSON_AddBoolToObject(niko, "hasUsername", !settings.niko.username.empty()) &&
        cJSON_AddBoolToObject(niko, "hasJwt", !settings.niko.jwt.empty()) &&
        cJSON_AddBoolToObject(niko, "hasCa", !settings.niko.ca_pem.empty());
    cJSON *allowed = okay ? cJSON_AddArrayToObject(niko, "allowedLights") : nullptr;
    okay = allowed != nullptr;
    for (const auto &id : settings.niko.allowed_lights) {
        if (!okay) break;
        cJSON *value = cJSON_CreateString(id.c_str());
        if (!value || !cJSON_AddItemToArray(allowed, value)) { cJSON_Delete(value); okay = false; }
    }
    cJSON *buddy = okay ? cJSON_AddObjectToObject(root, "buddy") : nullptr;
    okay = buddy && add_string(buddy, "name", settings.name) &&
           add_string(buddy, "instructions", settings.instructions) &&
           add_string(buddy, "scene", settings.scene) &&
           cJSON_AddBoolToObject(buddy, "readAloud", settings.read_aloud);
    cJSON *prefs = okay ? buddy_preferences_json(settings.prefs) : nullptr;
    if (!prefs || !cJSON_AddItemToObject(root, "preferences", prefs)) {
        cJSON_Delete(prefs);
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

cJSON *buddy_preferences_json(const BuddyPreferences &prefs)
{
    cJSON *root = cJSON_CreateObject();
    const bool okay = root && add_string(root, "userName", prefs.user_name) &&
        add_string(root, "timezone", prefs.timezone) &&
        cJSON_AddStringToObject(root, "voiceStyle", buddy_voice_style_names[prefs.voice_style]) &&
        cJSON_AddNumberToObject(root, "speechRate", prefs.speech_rate) &&
        cJSON_AddBoolToObject(root, "captions", prefs.captions) &&
        cJSON_AddNumberToObject(root, "brightness", prefs.brightness) &&
        cJSON_AddBoolToObject(root, "dimAtNight", prefs.dim_at_night) &&
        cJSON_AddStringToObject(root, "screenSleep", buddy_screen_sleep_names[prefs.screen_sleep]) &&
        cJSON_AddStringToObject(root, "persona", buddy_persona_names[prefs.persona]) &&
        cJSON_AddNumberToObject(root, "volume", prefs.volume);
    if (!okay) { cJSON_Delete(root); return nullptr; }
    return root;
}
