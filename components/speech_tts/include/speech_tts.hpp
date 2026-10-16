#pragma once
#include <string>

// English-only local speech. Call synchronously from the chat worker, with recording disabled.
// The codec must already be opened by audio_play_init() at 16 kHz / stereo / 16-bit.
// Returns true when the text was spoken completely or interrupted by speech_tts_cancel().
bool speech_tts_speak(const char *text, std::string &error);

// Speaking speed in tenths of normal (5..20) and voice style (0 warm, 1 bright, 2 soft);
// applied at the start of the next utterance. Safe to call from any task.
void speech_tts_configure(int rate_tenths, int style);
// Stops the utterance in progress, if any. Safe to call from any task.
void speech_tts_cancel();
// Effective speaking rate, for pacing on-screen captions.
int speech_tts_words_per_minute();
