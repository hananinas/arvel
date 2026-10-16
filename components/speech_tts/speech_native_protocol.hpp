#pragma once
#include <cstddef>
#include <cstdint>

namespace speech_native_protocol {
constexpr size_t max_event_bytes = 32768;
constexpr size_t max_text_bytes = 8192;
// Tool registration has an independent bound; enclosing setup/event framing
// must still fit its own 32KiB cap after JSON escaping and metadata.
constexpr size_t max_tools_bytes = 32768;
constexpr size_t pcm_chunk_bytes = 3840;
constexpr size_t max_json_nodes = 512;
constexpr size_t max_json_depth = 16;

enum class JsonType : uint8_t { Object, Array, String, Number, True, False, Null };
struct JsonNode {
    size_t begin = 0, end = 0;
    uint16_t next = 0, child = 0, key = 0, sibling = 0;
    JsonType type = JsonType::Null;
};
// Caller-owned workspace: keep alongside the event in PSRAM, not on an audio task stack.
struct JsonWorkspace {
    JsonNode nodes[max_json_nodes];
    size_t count = 0;
    mutable char key_scratch[2][129] = {};
};
bool validate_json(const char *, size_t, JsonWorkspace &, const char *&error);
bool json_equal(const char *, size_t, const char *, size_t, JsonWorkspace &, JsonWorkspace &);
bool validate_health(const char *, size_t, JsonWorkspace &, const char *&error);
// Produces a complete quoted JSON string and a terminating NUL; bytes excludes NUL.
bool escape_json(const char *, size_t, char *, size_t capacity, size_t &bytes);
// Provider deltas are incremental (including repeats); final text replaces them.
// Capacity includes NUL. Failure leaves the accumulated transcript unchanged.
bool update_transcript(char *, size_t capacity, const char *incoming, bool final);

enum class EventKind {
    Ignore, SessionCreated, SessionUpdated, ResponseCreated, SpeechStarted, SpeechStopped,
    AudioDelta, AudioDone, UserDelta, UserFinal, AssistantDelta, AssistantFinal,
    FunctionDone, ResponseDone, Error, SessionEnd
};
struct Event {
    EventKind kind = EventKind::Ignore;
    char provider_session[129] = {}, item_id[129] = {}, response_id[129] = {};
    char call_id[129] = {}, name[129] = {}, error_code[129] = {};
    char model[129] = {};
    char text[max_text_bytes + 1] = {}, arguments[max_text_bytes + 1] = {};
    char audio_base64[max_event_bytes + 1] = {}, tools_json[max_tools_bytes + 1] = {};
    bool effective_pcm24 = false, tools_acknowledged = false;
    JsonWorkspace workspace;
};
bool parse_event(const char *, size_t, Event &, const char *&static_error);
// Strict canonical base64; only even-byte PCM16 LE payloads are accepted.
// A failure consumes no samples and writes no output.
bool decode_audio(const char *, size_t, int16_t *, size_t capacity_samples,
                  size_t &samples, const char *&error);
bool encode_audio(const int16_t *, size_t samples, char *, size_t capacity, size_t &bytes);

// Causal 95-tap / 48kHz Blackman-windowed sinc rational FIRs. Zero initial history;
// no utterance-sized state or look-ahead, no clock changes. Capacity failure does not
// consume input or modify filter history. Each 1280 capture frames yields 1920 samples.
class CaptureConverter {
public:
    void reset();
    bool process(const int16_t *stereo_16k, size_t input_frames, int16_t *mono_24k,
                 size_t output_capacity_samples, size_t &output_samples);
private:
    float history_[48] = {};
    uint8_t cursor_ = 0, phase_ = 0;
};
class PlaybackConverter {
public:
    void reset();
    bool process(const int16_t *mono_24k, size_t input_samples, int16_t *stereo_16k,
                 size_t output_capacity_frames, size_t &output_frames);
private:
    float history_[48] = {};
    uint8_t cursor_ = 0, phase_ = 0;
};
}
