#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Internal, transport-independent wire format shared by firmware and host smoke
// harnesses. PCM is borrowed and is never appended to either framing string.
namespace speech_stt_protocol {
constexpr size_t max_pcm_bytes = 320000;
constexpr size_t max_response_bytes = 16384;
constexpr size_t max_transcript_bytes = 8192;
constexpr size_t wav_header_bytes = 44;

bool validate_settings(const char *url, const char *key, const char *model,
                       std::string &error);
bool make_multipart(const char *model, const char *boundary, size_t pcm_size,
                    std::string &prefix, uint8_t (&wav)[wav_header_bytes],
                    std::string &suffix, std::string &error);
// response must include a terminator at response[size], not counted in size.
bool parse_response(const char *response, size_t size, int http_status,
                    std::string &text, std::string &error);
} // namespace speech_stt_protocol
