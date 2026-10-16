#include "speech_stt_protocol.hpp"

#include <cJSON.h>
#include <cstring>
#include <memory>

namespace speech_stt_protocol {
namespace {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;

bool valid_model(const char *model)
{
    if (!model || !*model) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(model); *p; ++p) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

void put16(uint8_t *p, uint16_t value)
{
    p[0] = value & 0xff;
    p[1] = value >> 8;
}

void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (value >> (8 * i)) & 0xff;
}

// cJSON represents strings with a NUL terminator, so an escaped NUL would
// silently truncate a transcript. Skip escaped backslashes, not literal text.
bool has_escaped_nul(const char *response, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (response[i] != '\\') continue;
        if (++i >= size) break;
        if (response[i] == 'u' && size - i >= 5 &&
            std::memcmp(response + i + 1, "0000", 4) == 0) return true;
    }
    return false;
}

bool unicode_space(uint32_t cp)
{
    return (cp >= 0x09 && cp <= 0x0d) || cp == 0x20 || cp == 0x85 ||
           cp == 0xa0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f ||
           cp == 0x3000;
}

bool valid_transcript(const char *value, size_t size, bool &has_words)
{
    has_words = false;
    const auto *bytes = reinterpret_cast<const unsigned char *>(value);
    for (size_t i = 0; i < size;) {
        const unsigned char first = bytes[i++];
        uint32_t cp = first;
        unsigned remaining = 0;
        uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            cp = first & 0x1f; remaining = 1; minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            cp = first & 0x0f; remaining = 2; minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            cp = first & 0x07; remaining = 3; minimum = 0x10000;
        } else if (first >= 0x80) {
            return false;
        }
        if (remaining > size - i) return false;
        for (unsigned n = 0; n < remaining; ++n) {
            const unsigned char next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        const bool whitespace = unicode_space(cp);
        if (!whitespace && (cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))) return false;
        if (!whitespace) has_words = true;
    }
    return true;
}

std::string http_error(int status)
{
    std::string result = "STT provider HTTP " + std::to_string(status) + ": ";
    if (status == 401 || status == 403) return result + "check the STT API key and endpoint permissions.";
    if (status == 404) return result + "check the transcription endpoint URL and model.";
    if (status == 429) return result + "provider rate limit or quota exhausted; check account quota and retry later.";
    if (status == 413) return result + "provider rejected recording size; check its audio limits.";
    if (status >= 300 && status < 400) return result + "redirect refused to protect the API key; configure the final HTTPS endpoint.";
    if (status >= 500) return result + "transcription service unavailable; retry later.";
    return result + "request rejected; check the model, audio support, and provider account.";
}
} // namespace

bool validate_settings(const char *url, const char *key, const char *model,
                       std::string &error)
{
    error.clear();
    if (!url || !*url) {
        error = "STT URL is missing; set the transcription endpoint in Buddy settings.";
        return false;
    }
    if (std::strncmp(url, "https://", 8) != 0) {
        error = "STT URL must use https:// with a certificate-trusted hostname.";
        return false;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(url); *p; ++p) {
        if (*p <= 0x20 || *p >= 0x7f || *p == '\\' || *p == '#') {
            error = "STT URL is invalid; use an ASCII HTTPS URL without whitespace or a fragment.";
            return false;
        }
    }
    const char *host = url + 8;
    const char *end = host + std::strcspn(host, "/?");
    if (host == end || std::memchr(host, '@', end - host)) {
        error = "STT URL must contain a hostname and must not embed credentials.";
        return false;
    }
    const char *port = nullptr;
    if (*host == '[') {
        const char *close = static_cast<const char *>(std::memchr(host, ']', end - host));
        if (!close || close == host + 1 || (close + 1 < end && close[1] != ':')) {
            error = "STT URL has an invalid IPv6 hostname.";
            return false;
        }
        if (close + 1 < end) port = close + 2;
    } else {
        const char *colon = static_cast<const char *>(std::memchr(host, ':', end - host));
        const char *host_end = colon ? colon : end;
        if (host_end == host || std::memchr(host, '[', host_end - host) ||
            std::memchr(host, ']', host_end - host)) {
            error = "STT URL has an invalid hostname.";
            return false;
        }
        if (colon) port = colon + 1;
    }
    if (port) {
        unsigned value = 0;
        for (const char *p = port; p < end; ++p) {
            if (*p < '0' || *p > '9' || value > 6553) {
                error = "STT URL port must be between 1 and 65535.";
                return false;
            }
            value = value * 10 + static_cast<unsigned>(*p - '0');
        }
        if (value == 0 || value > 65535) {
            error = "STT URL port must be between 1 and 65535.";
            return false;
        }
    }
    if (!key || !*key) {
        error = "STT API key is missing; open Buddy settings and configure the separate transcription service.";
        return false;
    }
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(key); *p; ++p) {
        if (*p <= 0x20 || *p >= 0x7f) {
            error = "STT API key is invalid; use a nonempty bearer token without whitespace or control characters.";
            return false;
        }
    }
    if (!valid_model(model)) {
        error = "STT model is missing or invalid; set a model without control characters in Buddy settings.";
        return false;
    }
    return true;
}

bool make_multipart(const char *model, const char *boundary, size_t pcm_size,
                    std::string &prefix, uint8_t (&wav)[wav_header_bytes],
                    std::string &suffix, std::string &error)
{
    prefix.clear();
    suffix.clear();
    error.clear();
    if (pcm_size == 0 || pcm_size > max_pcm_bytes || pcm_size % 4 != 0) {
        error = "STT recording must contain whole 16 kHz stereo 16-bit PCM frames, at most 320000 bytes (5 seconds).";
        return false;
    }
    if (!valid_model(model)) {
        error = "STT model is missing or invalid; set a model without control characters in Buddy settings.";
        return false;
    }
    if (!boundary || !*boundary || std::strlen(boundary) > 70) {
        error = "STT multipart boundary is invalid.";
        return false;
    }
    for (const char *p = boundary; *p; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-')) {
            error = "STT multipart boundary is invalid.";
            return false;
        }
    }
    prefix.reserve(std::strlen(model) + 3 * std::strlen(boundary) + 256);
    suffix.reserve(std::strlen(boundary) + 8);
    prefix = "--";
    prefix += boundary;
    prefix += "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
    prefix += model;
    prefix += "\r\n--";
    prefix += boundary;
    prefix += "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n--";
    prefix += boundary;
    prefix += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    suffix = "\r\n--";
    suffix += boundary;
    suffix += "--\r\n";

    std::memset(wav, 0, sizeof(wav));
    std::memcpy(wav, "RIFF", 4);
    put32(wav + 4, static_cast<uint32_t>(pcm_size + 36));
    std::memcpy(wav + 8, "WAVEfmt ", 8);
    put32(wav + 16, 16);
    put16(wav + 20, 1); // Linear PCM.
    put16(wav + 22, 2);
    put32(wav + 24, 16000);
    put32(wav + 28, 16000 * 2 * 2);
    put16(wav + 32, 4);
    put16(wav + 34, 16);
    std::memcpy(wav + 36, "data", 4);
    put32(wav + 40, static_cast<uint32_t>(pcm_size));
    return true;
}

bool parse_response(const char *response, size_t size, int http_status,
                    std::string &text, std::string &error)
{
    text.clear();
    error.clear();
    if (http_status < 200 || http_status >= 300) {
        error = http_error(http_status);
        return false;
    }
    if (!response || size == 0 || size > max_response_bytes ||
        std::memchr(response, '\0', size) || has_escaped_nul(response, size)) {
        error = "STT returned an empty, oversized, or invalid JSON response; no transcript accepted.";
        return false;
    }
    const char *end = nullptr;
    Json root(cJSON_ParseWithLengthOpts(response, size + 1, &end, true), cJSON_Delete);
    if (!root || !cJSON_IsObject(root.get()) || end != response + size) {
        error = "STT returned invalid or truncated JSON; no transcript accepted.";
        return false;
    }
    cJSON *provider_error = cJSON_GetObjectItemCaseSensitive(root.get(), "error");
    if (provider_error && !cJSON_IsNull(provider_error)) {
        error = "STT provider returned an error instead of a transcript; check the endpoint, model, API key, and provider quota.";
        return false;
    }
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root.get(), "text");
    if (!cJSON_IsString(value) || !value->valuestring) {
        error = "STT JSON response is missing a string text transcript; verify OpenAI-compatible response_format=json support.";
        return false;
    }
    const size_t length = std::strlen(value->valuestring);
    if (length > max_transcript_bytes) {
        error = "STT transcript exceeds 8192 bytes; it was rejected, not truncated.";
        return false;
    }
    bool has_words = false;
    if (!valid_transcript(value->valuestring, length, has_words)) {
        error = "STT transcript contains invalid UTF-8 or non-text control characters; no transcript accepted.";
        return false;
    }
    if (!has_words) {
        error = "STT transcript is empty or whitespace-only; record audible speech and try again.";
        return false;
    }
    text.assign(value->valuestring, length);
    return true;
}
} // namespace speech_stt_protocol
