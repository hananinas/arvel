#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Prepare once when the serialized worker admits a native operation, before
// publishing it as cancellable. Cancellation is sticky until the next prepare.
void speech_stt_prepare();
// Safe from any task. In-flight I/O observes Stop within a 100 ms transport poll.
void speech_stt_cancel();

// Synchronous HTTPS transcription of 16 kHz, stereo, signed 16-bit little-endian
// PCM (whole stereo frames, 1..320000 bytes). The caller retains the PCM buffer
// until return. Total connect/upload/headers/body deadline: 45 monotonic seconds.
// On failure/cancellation, text is empty. Partial transcripts are never accepted.
/* Settings are borrowed until return; caller keeps them unchanged during I/O. */
bool speech_stt_transcribe(const uint8_t *pcm, size_t pcm_size,
                           const char *url, const char *api_key, const char *model,
                           std::string &text, std::string &error);
