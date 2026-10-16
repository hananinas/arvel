#include "speech_native_protocol.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace speech_native_protocol;
namespace {
Event event;
JsonWorkspace other;
const char *error;
void check(bool condition, const char *name) {
    if (!condition) { std::fprintf(stderr, "native protocol: %s\n", name); std::abort(); }
}
bool parse(const std::string &s) { return parse_event(s.data(), s.size(), event, error); }
std::string quoted(const std::string &s) {
    std::vector<char> buffer(s.size() * 6 + 3); size_t bytes;
    check(escape_json(s.data(), s.size(), buffer.data(), buffer.size(), bytes), "JSON escaping succeeds");
    return std::string(buffer.data(), bytes);
}
bool same(const std::string &a, const std::string &b) {
    return json_equal(a.data(), a.size(), b.data(), b.size(), event.workspace, other);
}
std::string transcript(const std::string &text, bool user = false) {
    return std::string("{\"type\":\"") + (user ? "conversation.item.input_audio_transcription.completed" : "response.output_audio_transcript.done") + "\",\"item_id\":\"item-9\",\"response_id\":\"response-7\",\"transcript\":" + quoted(text) + "}";
}
std::string function(const std::string &arguments, const std::string &call = "call-42") {
    return "{\"type\":\"response.function_call_arguments.done\",\"item_id\":\"tool-item\",\"response_id\":\"reply-1\",\"call_id\":" + quoted(call) + ",\"name\":\"get_current_time\",\"arguments\":" + quoted(arguments) + "}";
}
std::string session(const std::string &input, const std::string &output, const std::string &tools) {
    return "{\"type\":\"session.updated\",\"session\":{\"audio\":{\"input\":{\"format\":" + input + "},\"output\":{\"format\":" + output + "}},\"tools\":" + tools + "}}";
}
void json_and_events() {
    check(parse("{\"type\":\"session.created\",\"session\":{\"id\":\"native-session-1\",\"model\":\"nemotron-voicechat\"}}") && event.kind == EventKind::SessionCreated && !std::strcmp(event.provider_session, "native-session-1"), "actual provider session preserved");
    check(!parse("{\"type\":\"session.created\",\"session\":{\"id\":\"s\",\"model\":\"other-model\"}}"), "foreign model rejected");
    check(parse("{\"type\":\"response.created\",\"response\":{\"id\":\"r-new\",\"output\":[]}}") && event.kind == EventKind::ResponseCreated && !std::strcmp(event.response_id, "r-new"), "nested response identity");
    check(parse("{\"type\":\"response.done\",\"response\":{\"id\":\"r-new\",\"output\":[]}}") && event.kind == EventKind::ResponseDone, "nested done response identity");
    const std::string pcm = "{\"type\":\"audio/pcm\",\"rate\":24000}";
    const std::string tools = "[{\"type\":\"function\",\"name\":\"get_current_time\",\"parameters\":{\"type\":\"object\",\"properties\":{}}}]";
    check(parse(session(pcm, pcm, quoted(tools))) && event.effective_pcm24 && event.tools_acknowledged && same(event.tools_json, tools), "string tools and effective formats acknowledged");
    check(parse(session(pcm, pcm, tools)) && event.tools_acknowledged && same(event.tools_json, tools), "array tools acknowledged");
    const std::string large_tools = "[{\"type\":\"function\",\"name\":\"search_web\",\"description\":" + quoted(std::string(max_text_bytes + 1000, 'd')) + ",\"parameters\":{\"type\":\"object\"}}]";
    check(parse(session(pcm, pcm, quoted(large_tools))) && event.tools_acknowledged && std::strlen(event.tools_json) > max_text_bytes && same(event.tools_json, large_tools), "tools acknowledgment bound independent of transcript bound");
    check(parse(session(pcm, pcm, large_tools)) && event.tools_acknowledged && same(event.tools_json, large_tools), "large raw tools acknowledgment is not truncated to transcript cap");
    check(parse(session(pcm, pcm, "[]")) && !same(event.tools_json, tools), "empty acknowledgment cannot match registered tool");
    check(parse(session("\"pcm16\"", pcm, "[]")) && !event.effective_pcm24, "legacy format not accepted as effective structured PCM24");
    check(parse(session(pcm, "{\"type\":\"audio/pcm\",\"rate\":22050}", "[]")) && !event.effective_pcm24, "wrong output rate rejected");
    check(parse(session("{\"type\":\"audio/opus\",\"rate\":24000}", pcm, "[]")) && !event.effective_pcm24, "wrong input encoding rejected");
    check(!parse(session(pcm, pcm, quoted("{}"))) && error, "non-array tools rejected with reason");
    check(!parse(session(pcm, pcm, quoted("["))), "malformed decoded tools rejected");
    check(same("[{\"name\":\"clock\",\"parameters\":{\"type\":\"object\",\"required\":[\"a\",\"b\"]}}]", "[ {\"parameters\":{\"required\":[\"a\",\"b\"],\"type\":\"object\"},\"name\":\"cl\\u006fck\"} ]"), "schema equality ignores object order whitespace escaped spelling");
    check(same("\"\\uD83D\\uDE00\"", "\"\xf0\x9f\x98\x80\""), "semantic equality decodes uppercase surrogate pair");
    check(!same("[\"a\",\"b\"]", "[\"b\",\"a\"]"), "schema arrays remain ordered");
    check(!same("{\"a\":1,\"b\":2}", "{\"a\":1,\"b\":3}"), "changed schema not equal");
    check(!same("{\"a\":1}", "{\"a\":1,\"b\":2}"), "extra schema field not equal");
    check(parse(function("{\"city\":\"Montr\\u00e9al\",\"n\":-12.5e+2}")) && event.kind == EventKind::FunctionDone && !std::strcmp(event.call_id, "call-42") && !std::strcmp(event.item_id, "tool-item") && !std::strcmp(event.response_id, "reply-1") && !std::strcmp(event.name, "get_current_time"), "only complete exact call identity accepted");
    check(parse("{\"type\":\"response.function_call_arguments.delta\",\"call_id\":\"call-42\",\"delta\":\"{\"}") && event.kind == EventKind::Ignore, "partial function arguments never execute");
    check(parse("{\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"call-42\",\"arguments\":\"{}\"}}") && event.kind == EventKind::Ignore, "output item is not a second call dispatch");
    check(!parse(function("[]")) && error, "function arguments must be object");
    check(!parse(function("{\"a\":1,\"a\":2}")), "duplicate argument member rejected");
    check(!parse(function("{\"x\":1,}")), "malformed complete argument object rejected");
    check(!parse(function("{}", "")), "empty call ID rejected");
    check(parse(function("{}", std::string(128, 'c'))) && std::strlen(event.call_id) == 128, "full ID retained without truncation");
    check(!parse(function("{}", std::string(129, 'c'))), "oversized ID rejected not aliased");
    check(parse(transcript("final-only answer \"quoted\"\n\xf0\x9f\x8e\xa4")) && event.kind == EventKind::AssistantFinal && !std::strcmp(event.item_id, "item-9") && !std::strcmp(event.text, "final-only answer \"quoted\"\n\xf0\x9f\x8e\xa4"), "final-only escaped transcript has exact item and UTF8");
    check(parse(transcript("final-only user", true)) && event.kind == EventKind::UserFinal, "final-only user transcript accepted");
    check(parse(transcript(std::string(max_text_bytes, 'x'))) && std::strlen(event.text) == max_text_bytes, "transcript bound inclusive");
    check(!parse(transcript(std::string(max_text_bytes + 1, 'x'))), "oversized transcript rejected");
    const char *bad[] = {
        "{\"type\":\"session.end\",\"type\":\"error\"}",
        "{\"type\":\"session.end\",\"ty\\u0070e\":\"error\"}",
        "{\"type\":\"session.end\",\"x\":{\"a\":1,\"b\":2,\"a\":3}}",
        "{\"type\":\"session.end\"} garbage", "{\"type\":\"session.end\",}",
        "{\"type\":\"session.end\",\"x\":01}", "{\"type\":\"session.end\",\"x\":1e}",
        "{\"type\":\"session.end\",\"x\":NaN}",
        "{\"type\":\"session.end\",\"x\":\"\\u0000\"}",
        "{\"type\":\"session.end\",\"x\":\"\\uD800\"}",
        "{\"type\":\"session.end\",\"x\":\"\\uDC00\"}",
        "{\"type\":\"session.end\",\"x\":\"\\q\"}",
        "{\"type\":\"session.end\",\"x\":\"\xc0\xaf\"}",
        "{\"type\":\"session.end\",\"x\":\"\xed\xa0\x80\"}",
        "{\"type\":\"session.end\",\"x\":\"\xf4\x90\x80\x80\"}",
        "{\"type\":\"session.end\",\"x\":\"\xe2\x82\"}"
    };
    for (const char *s : bad) check(!parse(s) && error, "malformed JSON or UTF8 rejected with static reason");
    std::string nul = "{\"type\":\"session.end\",\"x\":\"a"; nul.push_back(0); nul += "b\"}";
    check(!parse(nul), "embedded raw NUL rejected using supplied length");
    std::string depth = "{\"type\":\"session.end\",\"x\":" + std::string(max_json_depth + 1, '[') + "0" + std::string(max_json_depth + 1, ']') + "}";
    check(!parse(depth), "nested parser depth bounded");
    std::string nodes = "{\"type\":\"session.end\",\"x\":[";
    for (size_t i = 0; i < max_json_nodes; ++i) { if (i) nodes += ','; nodes += "0"; } nodes += "]}";
    check(!parse(nodes), "node budget bounded even for small scalars");
    std::string oversized = "{\"type\":\"session.end\",\"x\":\"" + std::string(max_event_bytes, 'a') + "\"}";
    check(!parse(oversized), "wire event byte budget bounded");
    check(!parse_event("{}", max_event_bytes + 1, event, error), "oversize input rejected before reading beyond buffer");
    check(parse("{\"type\":\"session.end\",\"stats\":{\"chunks_received\":150}}") && !event.call_id[0] && !event.text[0] && !event.model[0], "reused event clears previous fields");
    const std::string healthy = "{\"status\":\"ok\",\"service\":\"nemotron-voicechat-websocket-server\",\"mode\":\"triton\",\"triton_status\":\"ready\",\"model_inference_stats\":{\"success_count\":1234}}";
    check(validate_health(healthy.data(), healthy.size(), other, error), "real Triton health schema");
    std::string loopback = healthy; loopback.replace(loopback.find("\"triton\""), 8, "\"loopback\"");
    check(!validate_health(loopback.data(), loopback.size(), other, error), "loopback never inference ready");
    std::string missing = "{\"status\":\"ok\",\"service\":\"nemotron-voicechat-websocket-server\",\"mode\":\"triton\",\"triton_ready\":true}";
    check(!validate_health(missing.data(), missing.size(), other, error), "invented readiness flag rejected");
}
void transcript_accumulation() {
    char text[16]{};
    check(update_transcript(text, sizeof(text), "ha", false) &&
          update_transcript(text, sizeof(text), "ha", false) &&
          !std::strcmp(text, "haha"), "repeated deltas retain repeated words");
    text[0] = 0;
    check(update_transcript(text, sizeof(text), "a", false) &&
          update_transcript(text, sizeof(text), "gain", false) &&
          update_transcript(text, sizeof(text), "a", false) &&
          !std::strcmp(text, "againa"), "prefix-shaped delta is still incremental");
    check(update_transcript(text, sizeof(text), "final answer", true) &&
          !std::strcmp(text, "final answer"), "final transcript replaces partials");
    check(!update_transcript(text, sizeof(text), "extra", false) &&
          !std::strcmp(text, "final answer"), "overflow preserves prior transcript");
    text[0] = 0;
    check(update_transcript(text, sizeof(text), "only final", true) &&
          !std::strcmp(text, "only final"), "final-only transcript retained");
    check(update_transcript(text, sizeof(text), "", true) && !text[0],
          "empty final clears abandoned partial");
}
void pcm() {
    const int16_t values[] = {-32768, -1, 0, 1, 32767, 0x1234};
    char encoded[32]; size_t bytes, count; int16_t decoded[6];
    for (size_t n = 1; n <= 6; ++n) {
        check(encode_audio(values, n, encoded, sizeof(encoded), bytes), "PCM base64 encode");
        check(decode_audio(encoded, bytes, decoded, 6, count, error) && count == n && !std::memcmp(values, decoded, n * sizeof(int16_t)), "PCM LE signed roundtrip every padding length");
    }
    check(encode_audio(values + 5, 1, encoded, sizeof(encoded), bytes) && !std::strcmp(encoded, "NBI="), "wire PCM little endian independent of host");
    const char *invalid[] = {"", "AAA", "AA==", "AAAA", "AAB=", "====", "AA=A", "AA-_", "AAA=\n", "A A="};
    for (const char *s : invalid) {
        std::fill(std::begin(decoded), std::end(decoded), 1111);
        check(!decode_audio(s, std::strlen(s), decoded, 6, count, error) && !count && decoded[0] == 1111 && decoded[5] == 1111, "invalid base64 or odd PCM writes nothing");
    }
    check(encode_audio(values, 6, encoded, sizeof(encoded), bytes), "prepare capacity PCM");
    std::fill(std::begin(decoded), std::end(decoded), 2222);
    check(!decode_audio(encoded, bytes, decoded, 5, count, error) && !count && decoded[0] == 2222 && decoded[5] == 2222, "decode capacity failure consumes no output");
    char tiny[] = {'x', 'x', 'x', 'x'};
    check(!encode_audio(values, 1, tiny, sizeof(tiny), bytes) && !bytes && tiny[0] == 'x', "encode includes NUL capacity all-or-nothing");
    check(parse("{\"type\":\"response.output_audio.delta\",\"response_id\":\"r\",\"item_id\":\"i\",\"delta\":\"AAA=\"}") && event.kind == EventKind::AudioDelta, "valid PCM event");
    check(!parse("{\"type\":\"response.output_audio.delta\",\"response_id\":\"r\",\"item_id\":\"i\",\"delta\":\"AAAA\"}"), "odd PCM event rejected before playback");
    char escaped[8] = {}; size_t used;
    check(!escape_json("\n\n", 2, escaped, sizeof(escaped), used) && !used && !escaped[0], "JSON escaping capacity writes nothing");
}
std::vector<int16_t> tone(size_t n, double frequency, double rate, bool stereo) {
    std::vector<int16_t> v(n * (stereo ? 2 : 1));
    for (size_t i = 0; i < n; ++i) { int16_t x = static_cast<int16_t>(std::lround(12000 * std::sin(2 * 3.141592653589793 * frequency * i / rate))); v[i * (stereo ? 2 : 1)] = x; if (stereo) v[i * 2 + 1] = x; }
    return v;
}
double rms(const std::vector<int16_t> &v, size_t skip, size_t stride) {
    double sum = 0; size_t count = 0;
    for (size_t i = skip * stride; i < v.size(); i += stride) { sum += static_cast<double>(v[i]) * v[i]; ++count; }
    return std::sqrt(sum / count);
}
void conversion() {
    auto input = tone(12800, 731, 16000, true);
    std::vector<int16_t> whole(19200), split(19200);
    CaptureConverter a, b; size_t count;
    check(a.process(input.data(), 12800, whole.data(), whole.size(), count) && count == 19200, "capture exact rational count");
    size_t consumed = 0, produced = 0; unsigned seed = 12345;
    while (consumed < 12800) {
        seed = seed * 1664525u + 1013904223u;
        size_t frames = std::min<size_t>(1 + seed % 83, 12800 - consumed);
        check(b.process(input.data() + consumed * 2, frames, split.data() + produced, split.size() - produced, count), "arbitrary capture chunk accepted"); consumed += frames; produced += count;
    }
    check(produced == whole.size() && whole == split, "capture chunk splits preserve exact phase and samples");
    a.reset(); b.reset(); std::fill(split.begin(), split.end(), 777);
    check(!b.process(input.data(), 1280, split.data(), 1919, count) && count == 0 && split.front() == 777 && split.back() == 777, "capture capacity failure writes and consumes nothing");
    check(a.process(input.data(), 1280, whole.data(), 1920, count) && count == 1920, "capture reference chunk");
    check(b.process(input.data(), 1280, split.data(), 1920, count) && count == 1920 && std::equal(whole.begin(), whole.begin() + 1920, split.begin()), "capture retry after capacity failure has unchanged state");
    check(!b.process(input.data(), std::numeric_limits<size_t>::max(), split.data(), split.size(), count) && !count, "capture size arithmetic overflow rejected");
    auto mono = tone(19200, 731, 24000, false);
    std::vector<int16_t> play_whole(25600), play_split(25600);
    PlaybackConverter c, d;
    check(c.process(mono.data(), mono.size(), play_whole.data(), 12800, count) && count == 12800, "playback exact rational count");
    consumed = produced = 0;
    while (consumed < mono.size()) {
        seed = seed * 1664525u + 1013904223u;
        size_t samples = std::min<size_t>(1 + seed % 97, mono.size() - consumed);
        check(d.process(mono.data() + consumed, samples, play_split.data() + produced * 2, 12800 - produced, count), "arbitrary playback chunk accepted"); consumed += samples; produced += count;
    }
    check(produced == 12800 && play_whole == play_split, "playback chunk splits preserve exact phase and samples");
    c.reset(); d.reset(); std::fill(play_split.begin(), play_split.end(), 777);
    check(!d.process(mono.data(), 1920, play_split.data(), 1279, count) && !count && play_split.front() == 777 && play_split.back() == 777, "playback capacity failure writes and consumes nothing");
    check(c.process(mono.data(), 1920, play_whole.data(), 1280, count) && count == 1280, "playback reference chunk");
    check(d.process(mono.data(), 1920, play_split.data(), 1280, count) && count == 1280 && std::equal(play_whole.begin(), play_whole.begin() + 2560, play_split.begin()), "playback retry preserves filter state");
    d.reset(); check(d.process(mono.data(), 1, play_split.data(), 1, count) && count == 1, "playback first phase");
    check(d.process(mono.data() + 1, 1, play_split.data(), 1, count) && count == 1, "playback second phase");
    check(d.process(mono.data() + 2, 1, nullptr, 0, count) && !count, "playback zero-output phase consumes valid input without output buffer");
    check(a.process(nullptr, 0, nullptr, 0, count) && !count && c.process(nullptr, 0, nullptr, 0, count) && !count, "empty converter chunks preserve state");
    auto low = tone(24000, 1000, 24000, false), high = tone(24000, 9000, 24000, false);
    std::vector<int16_t> pass(32000), stop(32000); c.reset(); d.reset();
    check(c.process(low.data(), low.size(), pass.data(), 16000, count), "low frequency playback conversion");
    check(d.process(high.data(), high.size(), stop.data(), 16000, count), "high frequency playback conversion");
    double pass_rms = rms(pass, 200, 2), stop_rms = rms(stop, 200, 2);
    check(pass_rms > 8200 && pass_rms < 8700, "downsampling passband signal retained");
    check(stop_rms < pass_rms * 0.02, "above-Nyquist input attenuated by FIR not aliased interpolation");
    for (size_t i = 0; i < pass.size(); i += 2) check(pass[i] == pass[i + 1], "playback duplicates mono to stereo faithfully");
    // Continuous sinusoid after causal group delay, not just RMS: catches periodic
    // phase discontinuities and incorrect chunk-boundary filter history.
    double squared_error = 0;
    for (size_t i = 200; i < 16000; ++i) { double expected = 12000 * std::sin(2 * 3.141592653589793 * 1000 * (static_cast<double>(i) - 47.0 / 3.0) / 16000); double diff = pass[i * 2] - expected; squared_error += diff * diff; }
    check(std::sqrt(squared_error / 15800) < 30, "playback phase continuity and causal FIR delay");
    auto capture_low = tone(16000, 1000, 16000, true); std::vector<int16_t> native(24000); a.reset();
    check(a.process(capture_low.data(), 16000, native.data(), native.size(), count) && count == native.size(), "capture passband conversion");
    squared_error = 0;
    for (size_t i = 200; i < native.size(); ++i) { double expected = 12000 * std::sin(2 * 3.141592653589793 * 1000 * (static_cast<double>(i) - 47.0 / 2.0) / 24000); double diff = native[i] - expected; squared_error += diff * diff; }
    check(std::sqrt(squared_error / (native.size() - 200)) < 30, "capture phase continuity and reconstruction filter");
    std::vector<int16_t> opposite(2560); for (size_t i = 0; i < 1280; ++i) { opposite[i * 2] = 16000; opposite[i * 2 + 1] = -16000; }
    a.reset(); check(a.process(opposite.data(), 1280, native.data(), native.size(), count) && std::all_of(native.begin(), native.begin() + count, [](int16_t v) { return v == 0; }), "capture averages stereo without integer overflow");
}
}
int main() {
    json_and_events(); transcript_accumulation(); pcm(); conversion();
    std::puts("native protocol checks passed");
}
