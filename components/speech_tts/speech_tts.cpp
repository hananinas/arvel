#include "speech_tts.hpp"
#include "speech_pcm.hpp"
#include "user_audio_bsp.h"

namespace {
bool playback_sink(const int16_t *pcm, size_t frames, void *)
{
    return audio_playback_write_checked(pcm, static_cast<uint32_t>(frames * 2 * sizeof(int16_t)));
}
}

bool speech_tts_speak(const char *text, std::string &error)
{
    return speech_tts_synthesize(text, playback_sink, nullptr, error);
}
