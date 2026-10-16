#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Private streaming engine interface. Buffers are 16 kHz stereo signed 16-bit PCM,
// valid only during the callback. All calls must share one synchronous worker.
using SpeechPcmSink = bool (*)(const int16_t *pcm, size_t frames, void *context);
bool speech_tts_synthesize(const char *text, SpeechPcmSink sink, void *context, std::string &error);
