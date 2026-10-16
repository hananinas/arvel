#pragma once

#include <cstddef>
#include <string>

// Callback bytes are owned by the live worker and valid only for this invocation.
// Callbacks may enqueue context or request stop; they must not call LVGL.
using speech_live_event_callback_t = void (*)(const char *json, size_t length, void *context);

bool speech_live_start(const char *voice, const char *instructions,
                       speech_live_event_callback_t callback, void *context,
                       std::string &error);
bool speech_live_active();
bool speech_live_muted();
bool speech_live_set_muted(bool muted, std::string &error);
void speech_live_stop();
bool speech_live_context(const char *text, const char *delegation_id, std::string &error);

// Prepare on the native worker before scheduling read-aloud. Cancel remains
// latched until the next preparation, including while authentication is pending.
void speech_live_prepare_speech();
void speech_live_cancel_speech();
// Synchronous, serialized read-aloud. Never opens microphone capture.
bool speech_live_speak(const char *text, const char *voice, std::string &error);
