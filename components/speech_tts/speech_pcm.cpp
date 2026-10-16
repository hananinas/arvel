#include "speech_pcm.hpp"
#include "speech_tts.hpp"
#include <atomic>
#include <cstring>

extern "C" {
#include "espeak-ng/espeak_ng.h"
#include "embedded_workspace.h"
// The upstream internal headers are C-only; the public API does not expose RNG seeding.
void espeak_srand(long seed);
}
#include "dict/en_dict.h"
#include "voice/en.h"
#include "phoneme/phondata.h"
#include "phoneme/phonindex.h"
#include "phoneme/phontab.h"
#include "phoneme/intonations.h"

namespace {
constexpr uint32_t kOutputRate = 16000;
constexpr size_t kChunkFrames = 256;
bool initialized = false;
uint32_t source_rate = 0;
std::atomic<int> rate_wpm{espeakRATE_NORMAL};
std::atomic<int> voice_style{0};
std::atomic<bool> cancel_requested{false};
// Pitch and pitch range per voice style: warm (lower), bright (higher), soft (narrow).
constexpr int kStylePitch[3] = {40, 64, 50};
constexpr int kStyleRange[3] = {45, 60, 30};

bool status_error(espeak_ng_STATUS status, const char *operation, std::string &error)
{
    char description[160] = {};
    espeak_ng_GetStatusCodeMessage(status, description, sizeof(description));
    error = operation;
    error += ": ";
    error += description;
    error += ". Check PSRAM availability and the embedded English eSpeak assets.";
    return false;
}

bool initialize(std::string &error)
{
    if (initialized) return true;
    if (!espeak_translation_workspace_init() || !espeak_phoneme_workspace_init() ||
        !espeak_pitch_workspace_init() || !espeak_clause_workspace_init()) {
        error = "Cannot allocate eSpeak clause workspace. Enable 8 MiB PSRAM and free recording buffers before speaking.";
        return false;
    }
    espeak_InstallDict(__espeakng_dict, sizeof(__espeakng_dict));
    espeak_InstallPhonIndex(_phonindex, sizeof(_phonindex));
    espeak_InstallPhonTab(_phontab, sizeof(_phontab));
    espeak_InstallPhonData(_phondata, sizeof(_phondata));
    espeak_InstallIntonations(_intonations, sizeof(_intonations));
    espeak_InstallVoice(__rawdata_en, sizeof(__rawdata_en));
    espeak_ng_ERROR_CONTEXT context = nullptr;
    espeak_ng_STATUS status = espeak_ng_Initialize(&context);
    espeak_ng_ClearErrorContext(&context);
    if (status == ENS_OK)
        status = espeak_ng_InitializeOutput(ENOUTPUT_MODE_SYNCHRONOUS, 60, nullptr);
    if (status == ENS_OK)
        status = espeak_ng_SetVoiceByFile("internal");
    if (status != ENS_OK) {
        espeak_ng_Terminate();
        return status_error(status, "eSpeak initialization failed", error);
    }
    source_rate = static_cast<uint32_t>(espeak_ng_GetSampleRate());
    if (source_rate != 22050) {
        espeak_ng_Terminate();
        error = "Embedded English voice has an unexpected sample rate; expected 22050 Hz.";
        return false;
    }
    initialized = true;
    return true;
}

struct Stream {
    SpeechPcmSink sink;
    void *context;
    int16_t stereo[kChunkFrames * 2] = {};
    size_t frames = 0;
    int32_t until_output = 0;
    int16_t previous = 0;
    bool have_previous = false;
    bool failed = false;

    bool flush()
    {
        if (frames && !sink(stereo, frames, context)) {
            failed = true;
            return false;
        }
        frames = 0;
        return true;
    }

    bool emit(int16_t sample)
    {
        stereo[frames * 2] = sample;
        stereo[frames * 2 + 1] = sample;
        ++frames;
        return frames != kChunkFrames || flush();
    }

    bool consume(const short *mono, int count)
    {
        for (int i = 0; i < count; ++i) {
            const int16_t sample = mono[i];
            if (!have_previous) {
                if (!emit(sample)) return false;
                have_previous = true;
                until_output = static_cast<int32_t>(source_rate);
            } else {
                // Rational phase in 1/(source_rate*16000)-second units; bounded
                // across callbacks and arbitrarily long utterances without 64-bit arithmetic.
                until_output -= static_cast<int32_t>(kOutputRate);
                while (until_output <= 0) {
                    const int32_t fraction = until_output + static_cast<int32_t>(kOutputRate);
                    const int32_t value = static_cast<int32_t>(previous) +
                        ((static_cast<int32_t>(sample) - previous) * fraction) /
                        static_cast<int32_t>(kOutputRate);
                    if (!emit(static_cast<int16_t>(value))) return false;
                    until_output += static_cast<int32_t>(source_rate);
                }
            }
            previous = sample;
        }
        return true;
    }
};
Stream *active_stream = nullptr;

int synth_callback(short *samples, int count, espeak_EVENT *)
{
    if (!active_stream || cancel_requested.load()) return 1;
    if (!samples) return active_stream->flush() ? 0 : 1;
    return active_stream->consume(samples, count) ? 0 : 1;
}
} // namespace

bool speech_tts_synthesize(const char *text, SpeechPcmSink sink, void *context, std::string &error)
{
    error.clear();
    if (!text || !*text || !sink) {
        error = "Cannot speak empty text or write to an unavailable PCM output.";
        return false;
    }
    if (!initialize(error)) return false;
    Stream stream{sink, context};
    const int style = voice_style.load();
    espeak_ng_SetParameter(espeakRATE, rate_wpm.load(), 0);
    espeak_ng_SetParameter(espeakPITCH, kStylePitch[style], 0);
    espeak_ng_SetParameter(espeakRANGE, kStyleRange[style], 0);
    cancel_requested.store(false);
    active_stream = &stream;
    espeak_SetSynthCallback(synth_callback);
    // The same process input and initial state produce reproducible speech, not time-seeded noise.
    espeak_srand(1);
    const espeak_ng_STATUS status = espeak_ng_Synthesize(text, std::strlen(text) + 1, 0,
        POS_CHARACTER, 0, espeakCHARS_UTF8 | espeakENDPAUSE, nullptr, nullptr);
    active_stream = nullptr;
    if (cancel_requested.exchange(false)) {
        // An interrupted reply is not an error: stop synthesis and report success.
        espeak_AbortSynthesis();
        espeak_ng_Cancel();
        return true;
    }
    if (stream.failed || status != ENS_OK) {
        espeak_AbortSynthesis();
        espeak_ng_Cancel();
        if (stream.failed) {
            error = "PCM playback failed. Check codec initialization, ES8311 wiring and the 16000 Hz stereo 16-bit playback configuration.";
            return false;
        }
        return status_error(status, "eSpeak synthesis failed", error);
    }
    return true;
}

void speech_tts_configure(int rate_tenths, int style)
{
    const int wpm = espeakRATE_NORMAL * rate_tenths / 10;
    rate_wpm.store(wpm < espeakRATE_MINIMUM ? espeakRATE_MINIMUM : wpm > espeakRATE_MAXIMUM ? espeakRATE_MAXIMUM : wpm);
    voice_style.store(style >= 0 && style <= 2 ? style : 0);
}

void speech_tts_cancel()
{
    cancel_requested.store(true);
}

int speech_tts_words_per_minute()
{
    return rate_wpm.load();
}
