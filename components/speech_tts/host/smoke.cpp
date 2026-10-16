#include "speech_tts.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {
FILE *output = nullptr;
size_t frame_count = 0;
size_t nonzero_count = 0;
uint64_t waveform_hash = 14695981039346656037ULL;
bool reject_output = false;
}

// This is the only host hardware substitution: the actual production English
// engine and 22050 -> 16000 Hz stereo converter are linked unchanged.
extern "C" bool audio_playback_write_checked(const void *data, uint32_t bytes)
{
    if (reject_output) return false;
    if (!data || !bytes || bytes % 4 || bytes > 1024) return false;
    const auto *pcm = static_cast<const int16_t *>(data);
    for (uint32_t i = 0; i < bytes / sizeof(int16_t); i += 2) {
        if (pcm[i] != pcm[i + 1]) return false;
        ++frame_count;
        if (pcm[i]) ++nonzero_count;
    }
    const auto *raw = static_cast<const uint8_t *>(data);
    for (uint32_t i = 0; i < bytes; ++i) {
        waveform_hash ^= raw[i];
        waveform_hash *= 1099511628211ULL;
    }
    return !output || std::fwrite(data, 1, bytes, output) == bytes;
}

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "Usage: %s OUTPUT.pcm [English text]\n", argv[0]);
        return 2;
    }
    output = std::fopen(argv[1], "wb");
    if (!output) { std::perror(argv[1]); return 2; }
    std::string error;
    const bool ok = speech_tts_speak(argc == 3 ? argv[2] : "Hello. This is local English speech on Unravel.", error);
    const bool closed = std::fclose(output) == 0;
    output = nullptr;
    if (!ok || !closed || frame_count < 1600 || nonzero_count == 0) {
        std::fprintf(stderr, "Speech smoke failed: %s; frames=%zu nonzero=%zu\n", error.c_str(), frame_count, nonzero_count);
        return 1;
    }
    // Verify hardware errors are not reported as successful synthesis.
    reject_output = true;
    if (speech_tts_speak("The output must fail.", error) || error.empty()) {
        std::fprintf(stderr, "Speech output failure was not propagated\n");
        return 1;
    }
    reject_output = false;
    if (!speech_tts_speak("Speech resumes after a failed output.", error)) {
        std::fprintf(stderr, "Speech did not recover: %s\n", error.c_str());
        return 1;
    }
    std::printf("16 kHz stereo signed 16-bit PCM: frames=%zu nonzero=%zu hash=%016llx\n",
        frame_count, nonzero_count, static_cast<unsigned long long>(waveform_hash));
    return 0;
}
