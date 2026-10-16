#include "speech_native_protocol.hpp"
#include "speech_native_audio.hpp"
#include "speech_live_protocol.hpp"
#include <cstring>
#include <cmath>
#include <limits>

namespace speech_native_protocol {
namespace {
constexpr uint16_t absent = UINT16_MAX;
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool decode_string(const char *s, const JsonNode &node, char *out, size_t cap, size_t &used) {
    used = 0;
    auto emit = [&](unsigned char c) {
        if (!c || used == std::numeric_limits<size_t>::max() || (out && used + 1 >= cap)) return false;
        if (out) out[used] = static_cast<char>(c);
        ++used;
        return true;
    };
    for (size_t p = node.begin + 1; p + 1 < node.end;) {
        unsigned char c = static_cast<unsigned char>(s[p++]);
        if (c < 32 || c == '"') return false;
        if (c != '\\') { if (!emit(c)) return false; continue; }
        if (p + 1 >= node.end) return false;
        c = static_cast<unsigned char>(s[p++]);
        switch (c) {
        case '"': case '\\': case '/': if (!emit(c)) return false; break;
        case 'b': if (!emit('\b')) return false; break;
        case 'f': if (!emit('\f')) return false; break;
        case 'n': if (!emit('\n')) return false; break;
        case 'r': if (!emit('\r')) return false; break;
        case 't': if (!emit('\t')) return false; break;
        case 'u': {
            auto unit = [&](uint32_t &v) {
                v = 0;
                if (p + 4 >= node.end) return false;
                for (int i = 0; i < 4; ++i) { int d = hex(s[p++]); if (d < 0) return false; v = (v << 4) | static_cast<unsigned>(d); }
                return true;
            };
            uint32_t v;
            if (!unit(v) || v == 0) return false;
            if (v >= 0xd800 && v <= 0xdbff) {
                if (p + 2 >= node.end || s[p++] != '\\' || s[p++] != 'u') return false;
                uint32_t low;
                if (!unit(low) || low < 0xdc00 || low > 0xdfff) return false;
                v = 0x10000 + ((v - 0xd800) << 10) + low - 0xdc00;
            } else if (v >= 0xdc00 && v <= 0xdfff) return false;
            if (v < 128) { if (!emit(static_cast<unsigned char>(v))) return false; }
            else if (v < 2048) {
                if (!emit(0xc0 | (v >> 6)) || !emit(0x80 | (v & 63))) return false;
            } else if (v < 65536) {
                if (!emit(0xe0 | (v >> 12)) || !emit(0x80 | ((v >> 6) & 63)) || !emit(0x80 | (v & 63))) return false;
            } else if (!emit(0xf0 | (v >> 18)) || !emit(0x80 | ((v >> 12) & 63)) || !emit(0x80 | ((v >> 6) & 63)) || !emit(0x80 | (v & 63))) return false;
            break;
        }
        default: return false;
        }
    }
    if (out) out[used] = 0;
    return true;
}
struct Parser {
    const char *s; size_t n; JsonWorkspace &w; const char *&error; size_t p = 0;
    void ws() { while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p; }
    bool string(uint16_t &idx) {
        if (w.count == max_json_nodes) { error = "native_json_node_limit"; return false; }
        if (p >= n || s[p] != '"') return false;
        idx = static_cast<uint16_t>(w.count++);
        JsonNode &v = w.nodes[idx]; v = {}; v.type = JsonType::String; v.begin = p++;
        bool escaped = false, closed = false;
        while (p < n) {
            char c = s[p++];
            if (!escaped && c == '"') { closed = true; break; }
            if (!escaped && c == '\\') escaped = true; else escaped = false;
        }
        v.end = p; v.next = static_cast<uint16_t>(w.count); v.child = v.key = v.sibling = absent;
        size_t bytes;
        return closed && decode_string(s, v, nullptr, 0, bytes);
    }
    bool value(uint16_t &idx, size_t depth) {
        ws();
        if (depth > max_json_depth) { error = "native_json_depth_limit"; return false; }
        if (w.count == max_json_nodes) { error = "native_json_node_limit"; return false; }
        if (p >= n) return false;
        if (s[p] == '"') return string(idx);
        idx = static_cast<uint16_t>(w.count++);
        JsonNode &v = w.nodes[idx]; v = {}; v.begin = p; v.child = v.key = v.sibling = absent;
        if (s[p] == '{' || s[p] == '[') {
            const bool object = s[p++] == '{'; v.type = object ? JsonType::Object : JsonType::Array;
            char close = object ? '}' : ']'; ws();
            if (p < n && s[p] == close) ++p;
            else {
                uint16_t last = absent;
                while (true) {
                    uint16_t key = absent, child;
                    if (object) {
                        ws(); if (!string(key)) return false;
                        char *name = w.key_scratch[0]; size_t used;
                        if (!decode_string(s, w.nodes[key], name, 129, used)) { error = "native_json_key_limit"; return false; }
                        for (uint16_t prev = v.child; prev != absent; prev = w.nodes[prev].sibling) {
                            char *prior = w.key_scratch[1]; size_t prior_used;
                            if (!decode_string(s, w.nodes[w.nodes[prev].key], prior, 129, prior_used)) return false;
                            if (!std::strcmp(name, prior)) { error = "native_json_duplicate_key"; return false; }
                        }
                        ws(); if (p == n || s[p++] != ':') return false;
                    }
                    if (!value(child, depth + 1)) return false;
                    w.nodes[child].key = key;
                    if (v.child == absent) v.child = child;
                    if (last != absent) w.nodes[last].sibling = child;
                    last = child;
                    ws(); if (p == n) return false;
                    if (s[p] == close) { ++p; break; }
                    if (s[p++] != ',') return false;
                }
            }
        } else {
            if (s[p] == '-' || (s[p] >= '0' && s[p] <= '9')) {
                v.type = JsonType::Number;
                if (s[p] == '-') { if (++p == n) return false; }
                if (s[p] == '0') ++p;
                else { if (s[p] < '1' || s[p] > '9') return false; do { ++p; } while (p < n && s[p] >= '0' && s[p] <= '9'); }
                if (p < n && s[p] == '.') { ++p; size_t start = p; while (p < n && s[p] >= '0' && s[p] <= '9') ++p; if (start == p) return false; }
                if (p < n && (s[p] == 'e' || s[p] == 'E')) { ++p; if (p < n && (s[p] == '+' || s[p] == '-')) ++p; size_t start = p; while (p < n && s[p] >= '0' && s[p] <= '9') ++p; if (start == p) return false; }
            } else {
                const char *literal = s[p] == 't' ? "true" : s[p] == 'f' ? "false" : "null";
                v.type = s[p] == 't' ? JsonType::True : s[p] == 'f' ? JsonType::False : JsonType::Null;
                size_t len = std::strlen(literal);
                if (len > n - p || std::memcmp(s + p, literal, len)) return false;
                p += len;
            }
        }
        v.end = p; v.next = static_cast<uint16_t>(w.count); return true;
    }
};
uint16_t field(const char *s, const JsonWorkspace &w, uint16_t object, const char *name) {
    if (object == absent || w.nodes[object].type != JsonType::Object) return absent;
    for (uint16_t c = w.nodes[object].child; c != absent; c = w.nodes[c].sibling) {
        char *key = w.key_scratch[1]; size_t used;
        if (decode_string(s, w.nodes[w.nodes[c].key], key, 129, used) && !std::strcmp(name, key)) return c;
    }
    return absent;
}
bool str(const char *s, const JsonWorkspace &w, uint16_t node, char *out, size_t cap) {
    size_t used;
    return node != absent && w.nodes[node].type == JsonType::String && decode_string(s, w.nodes[node], out, cap, used);
}
bool optional(const char *s, const JsonWorkspace &w, uint16_t object, const char *key, char *out, size_t cap) {
    uint16_t node = field(s, w, object, key);
    return node == absent || str(s, w, node, out, cap);
}
bool equal_node(const char *a, const JsonWorkspace &wa, uint16_t ia, const char *b, const JsonWorkspace &wb, uint16_t ib) {
    const auto &x = wa.nodes[ia]; const auto &y = wb.nodes[ib];
    if (x.type != y.type) return false;
    if (x.type == JsonType::String) {
        // Compare decoded Unicode without allocating a full string: encoded escape
        // spelling can differ, so compare individual bounded codepoint fragments.
        size_t pa = x.begin + 1, pb = y.begin + 1;
        while (pa + 1 < x.end && pb + 1 < y.end) {
            auto fragment = [](const char *s, size_t &p, char *out) {
                size_t start = p;
                if (s[p] == '\\') {
                    p += 2;
                    if (s[p - 1] == 'u') {
                        uint32_t unit = 0;
                        for (size_t i = 0; i < 4; ++i) unit = (unit << 4) | static_cast<unsigned>(hex(s[p + i]));
                        p += 4;
                        if (unit >= 0xd800 && unit <= 0xdbff) p += 6;
                    }
                }
                else { unsigned char c = static_cast<unsigned char>(s[p]); p += c < 128 ? 1 : c < 224 ? 2 : c < 240 ? 3 : 4; }
                // decode_string assumes bounding quotes; adjacent original bytes
                // are not inspected, only the interior start..p span.
                JsonNode n; n.begin = start - 1; n.end = p + 1; size_t used;
                return decode_string(s, n, out, 8, used);
            };
            char ca[8], cb[8];
            if (!fragment(a, pa, ca) || !fragment(b, pb, cb) || std::strcmp(ca, cb)) return false;
        }
        return pa + 1 == x.end && pb + 1 == y.end;
    }
    if (x.type != JsonType::Object && x.type != JsonType::Array)
        return x.end - x.begin == y.end - y.begin && !std::memcmp(a + x.begin, b + y.begin, x.end - x.begin);
    uint16_t cy = y.child; size_t nx = 0, ny = 0;
    for (uint16_t c = y.child; c != absent; c = wb.nodes[c].sibling) ++ny;
    for (uint16_t c = x.child; c != absent; c = wa.nodes[c].sibling) {
        ++nx;
        if (x.type == JsonType::Object) {
            char *key = wa.key_scratch[0]; size_t used;
            if (!decode_string(a, wa.nodes[wa.nodes[c].key], key, 129, used)) return false;
            cy = field(b, wb, ib, key);
        }
        if (cy == absent || cy >= y.next || !equal_node(a, wa, c, b, wb, cy)) return false;
        if (x.type == JsonType::Array) cy = wb.nodes[cy].sibling;
    }
    return nx == ny;
}
bool pcm_format(const char *s, const JsonWorkspace &w, uint16_t audio, const char *side) {
    uint16_t format = field(s, w, field(s, w, audio, side), "format");
    char type[32];
    if (!str(s, w, field(s, w, format, "type"), type, sizeof(type)) || std::strcmp(type, "audio/pcm")) return false;
    uint16_t rate = field(s, w, format, "rate");
    return rate != absent && w.nodes[rate].type == JsonType::Number && w.nodes[rate].end - w.nodes[rate].begin == 5 && !std::memcmp(s + w.nodes[rate].begin, "24000", 5);
}
int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
bool base64_size(const char *s, size_t n, size_t &bytes) {
    if (!s || !n || n > max_event_bytes || n % 4) return false;
    size_t pad = (s[n - 1] == '=') + (s[n - 2] == '=');
    for (size_t i = 0; i < n - pad; ++i) if (base64_value(static_cast<unsigned char>(s[i])) < 0) return false;
    for (size_t i = n - pad; i < n; ++i) if (s[i] != '=') return false;
    if ((pad == 2 && (base64_value(s[n - 3]) & 15)) || (pad == 1 && (base64_value(s[n - 2]) & 3))) return false;
    bytes = n / 4 * 3 - pad;
    return bytes % 2 == 0;
}
bool output_count(size_t n, unsigned up, unsigned down, unsigned phase, size_t &count) {
    if (n / down > std::numeric_limits<size_t>::max() / up) return false;
    count = n / down * up;
    for (size_t i = 0; i < n % down; ++i) { while (phase < up) { if (count == std::numeric_limits<size_t>::max()) return false; ++count; phase += down; } phase -= up; }
    return true;
}
int16_t sample(float v) {
    if (v >= 32767) return 32767;
    if (v <= -32768) return -32768;
    return static_cast<int16_t>(std::lround(v));
}
float convolve(const float *history, uint8_t cursor, const float *coef, size_t taps) {
    float sum = 0;
    for (size_t k = 0; k < taps; ++k) sum += history[(cursor + 48 - k) % 48] * coef[k];
    return sum;
}
}

bool validate_json(const char *s, size_t n, JsonWorkspace &w, const char *&error) {
    error = "invalid_json"; w.count = 0;
    if (n > max_event_bytes) { error = "native_event_too_large"; return false; }
    if (!s || !n) return false;
    if (!speech_live_protocol::utf8(s, n)) { error = "invalid_json_utf8"; return false; }
    Parser p{s, n, w, error}; uint16_t root;
    if (!p.value(root, 0)) return false;
    p.ws(); if (p.p != n) return false;
    error = nullptr; return true;
}
bool json_equal(const char *a, size_t na, const char *b, size_t nb, JsonWorkspace &wa, JsonWorkspace &wb) {
    const char *error;
    return &wa != &wb && validate_json(a, na, wa, error) && validate_json(b, nb, wb, error) && equal_node(a, wa, 0, b, wb, 0);
}
bool validate_health(const char *s, size_t n, JsonWorkspace &w, const char *&error) {
    if (!validate_json(s, n, w, error)) return false;
    const char *keys[] = {"status", "service", "mode", "triton_status"};
    const char *values[] = {"ok", "nemotron-voicechat-websocket-server", "triton", "ready"};
    for (size_t i = 0; i < 4; ++i) { char value[129]; if (!str(s, w, field(s, w, 0, keys[i]), value, sizeof(value)) || std::strcmp(value, values[i])) { error = "native_inference_not_ready"; return false; } }
    error = nullptr; return true;
}
bool escape_json(const char *s, size_t n, char *out, size_t cap, size_t &bytes) {
    bytes = 0;
    if (!s || !out || !speech_live_protocol::utf8(s, n) || cap < 3) return false;
    size_t need = 2;
    for (size_t i = 0; i < n; ++i) { unsigned char c = s[i]; size_t k = c < 32 ? 6 : c == '"' || c == '\\' ? 2 : 1; if (k > cap - 1 - need) return false; need += k; }
    size_t p = 0; out[p++] = '"'; const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { unsigned char c = s[i]; if (c < 32) { out[p++] = '\\'; out[p++] = 'u'; out[p++] = '0'; out[p++] = '0'; out[p++] = digits[c >> 4]; out[p++] = digits[c & 15]; } else { if (c == '"' || c == '\\') out[p++] = '\\'; out[p++] = c; } }
    out[p++] = '"'; out[p] = 0; bytes = p; return true;
}
bool parse_event(const char *s, size_t n, Event &e, const char *&error) {
    e.kind = EventKind::Ignore; e.effective_pcm24 = e.tools_acknowledged = false;
    e.provider_session[0] = e.item_id[0] = e.response_id[0] = e.call_id[0] = e.name[0] = e.error_code[0] = 0;
    e.model[0] = 0;
    e.text[0] = e.arguments[0] = e.audio_base64[0] = e.tools_json[0] = 0;
    if (!validate_json(s, n, e.workspace, error)) return false;
    auto &w = e.workspace; char type[129];
    error = "invalid_native_event";
    if (!str(s, w, field(s, w, 0, "type"), type, sizeof(type))) return false;
    if (!optional(s, w, 0, "session_id", e.provider_session, sizeof(e.provider_session)) ||
        !optional(s, w, 0, "item_id", e.item_id, sizeof(e.item_id)) ||
        !optional(s, w, 0, "response_id", e.response_id, sizeof(e.response_id))) return false;
    if (!std::strcmp(type, "session.created") || !std::strcmp(type, "session.updated")) {
        e.kind = !std::strcmp(type, "session.created") ? EventKind::SessionCreated : EventKind::SessionUpdated;
        uint16_t session = field(s, w, 0, "session");
        if (session == absent || w.nodes[session].type != JsonType::Object || !optional(s, w, session, "id", e.provider_session, sizeof(e.provider_session))) return false;
        e.model[0] = 0;
        if (!optional(s, w, session, "model", e.model, sizeof(e.model))) return false;
        if (e.kind == EventKind::SessionCreated && (!e.provider_session[0] || std::strcmp(e.model, "nemotron-voicechat"))) return false;
        uint16_t audio = field(s, w, session, "audio");
        e.effective_pcm24 = pcm_format(s, w, audio, "input") && pcm_format(s, w, audio, "output");
        uint16_t tools = field(s, w, session, "tools");
        if (tools != absent) {
            if (w.nodes[tools].type == JsonType::String) { if (!str(s, w, tools, e.tools_json, sizeof(e.tools_json))) return false; }
            else if (w.nodes[tools].type == JsonType::Array) { size_t len = w.nodes[tools].end - w.nodes[tools].begin; if (len > max_tools_bytes) { error = "native_tools_too_large"; return false; } std::memcpy(e.tools_json, s + w.nodes[tools].begin, len); e.tools_json[len] = 0; }
            else return false;
            if (!validate_json(e.tools_json, std::strlen(e.tools_json), w, error)) return false;
            if (w.nodes[0].type != JsonType::Array) { error = "invalid_native_tools"; return false; }
            e.tools_acknowledged = true;
        }
    } else if (!std::strcmp(type, "response.created") || !std::strcmp(type, "response.done")) {
        e.kind = !std::strcmp(type, "response.created") ? EventKind::ResponseCreated : EventKind::ResponseDone;
        if (!str(s, w, field(s, w, field(s, w, 0, "response"), "id"), e.response_id, sizeof(e.response_id)) || !e.response_id[0]) return false;
    } else if (!std::strcmp(type, "response.function_call_arguments.done")) {
        e.kind = EventKind::FunctionDone;
        if (!e.response_id[0] || !e.item_id[0] ||
            !str(s, w, field(s, w, 0, "call_id"), e.call_id, sizeof(e.call_id)) || !e.call_id[0] ||
            !str(s, w, field(s, w, 0, "name"), e.name, sizeof(e.name)) || !e.name[0] ||
            !str(s, w, field(s, w, 0, "arguments"), e.arguments, sizeof(e.arguments))) return false;
        if (!validate_json(e.arguments, std::strlen(e.arguments), w, error)) return false;
        if (w.nodes[0].type != JsonType::Object) { error = "invalid_native_arguments"; return false; }
    } else if (!std::strcmp(type, "input_audio_buffer.speech_started") || !std::strcmp(type, "input_audio_buffer.speech_stopped")) {
        e.kind = !std::strcmp(type, "input_audio_buffer.speech_started") ? EventKind::SpeechStarted : EventKind::SpeechStopped;
        if (!e.item_id[0]) return false;
    } else if (!std::strcmp(type, "response.output_audio.delta")) {
        e.kind = EventKind::AudioDelta;
        size_t bytes;
        if (!e.item_id[0] || !e.response_id[0] || !str(s, w, field(s, w, 0, "delta"), e.audio_base64, sizeof(e.audio_base64)) || !base64_size(e.audio_base64, std::strlen(e.audio_base64), bytes)) return false;
    } else if (!std::strcmp(type, "response.output_audio.done")) {
        e.kind = EventKind::AudioDone; if (!e.item_id[0] || !e.response_id[0]) return false;
    } else if (!std::strcmp(type, "conversation.item.input_audio_transcription.delta") || !std::strcmp(type, "conversation.item.input_audio_transcription.completed") || !std::strcmp(type, "response.output_audio_transcript.delta") || !std::strcmp(type, "response.output_audio_transcript.done")) {
        bool user = !std::strncmp(type, "conversation.", 13);
        bool final = !std::strcmp(type, "conversation.item.input_audio_transcription.completed") || !std::strcmp(type, "response.output_audio_transcript.done");
        e.kind = user ? (final ? EventKind::UserFinal : EventKind::UserDelta) : (final ? EventKind::AssistantFinal : EventKind::AssistantDelta);
        if (!e.item_id[0] || (!user && !e.response_id[0]) || !str(s, w, field(s, w, 0, final ? "transcript" : "delta"), e.text, sizeof(e.text))) return false;
    } else if (!std::strcmp(type, "error")) {
        e.kind = EventKind::Error;
        uint16_t node = field(s, w, 0, "error");
        if (!str(s, w, field(s, w, node, "code"), e.error_code, sizeof(e.error_code)) || !optional(s, w, node, "message", e.text, sizeof(e.text))) return false;
    } else if (!std::strcmp(type, "session.end")) e.kind = EventKind::SessionEnd;
    error = nullptr; return true;
}
bool update_transcript(char *text, size_t capacity, const char *incoming, bool final) {
    if (!text || !incoming || !capacity || capacity > max_text_bytes + 1) return false;
    const size_t current = strnlen(text, capacity), added = strnlen(incoming, capacity);
    if (current == capacity || added == capacity || (!final && added > capacity - current - 1)) return false;
    std::memcpy(text + (final ? 0 : current), incoming, added + 1);
    return true;
}
bool decode_audio(const char *s, size_t n, int16_t *out, size_t cap, size_t &samples, const char *&error) {
    samples = 0; error = "invalid_native_pcm"; size_t bytes;
    if (!out || !base64_size(s, n, bytes) || bytes / 2 > cap) return false;
    size_t pos = 0; uint8_t low = 0;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t v = static_cast<uint32_t>(base64_value(s[i])) << 18 | static_cast<uint32_t>(base64_value(s[i + 1])) << 12;
        if (s[i + 2] != '=') v |= static_cast<uint32_t>(base64_value(s[i + 2])) << 6;
        if (s[i + 3] != '=') v |= static_cast<uint32_t>(base64_value(s[i + 3]));
        for (int shift = 16; shift >= 0 && pos < bytes; shift -= 8) {
            uint8_t b = static_cast<uint8_t>(v >> shift);
            if (!(pos & 1)) low = b;
            else { uint16_t bits = static_cast<uint16_t>(low) | static_cast<uint16_t>(b) << 8; out[pos / 2] = static_cast<int16_t>(bits <= 32767 ? static_cast<int32_t>(bits) : static_cast<int32_t>(bits) - 65536); }
            ++pos;
        }
    }
    samples = bytes / 2; error = nullptr; return true;
}
bool encode_audio(const int16_t *in, size_t n, char *out, size_t cap, size_t &bytes) {
    bytes = 0;
    if (!in || !out || n > max_event_bytes / 2) return false;
    size_t raw = n * 2, need = (raw + 2) / 3 * 4;
    if (need >= cap || need > max_event_bytes) return false;
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto byte = [&](size_t i) { uint16_t v = static_cast<uint16_t>(in[i / 2]); return static_cast<uint8_t>(v >> ((i % 2) * 8)); };
    size_t p = 0;
    for (size_t i = 0; i < raw; i += 3) { uint32_t v = static_cast<uint32_t>(byte(i)) << 16; if (i + 1 < raw) v |= static_cast<uint32_t>(byte(i + 1)) << 8; if (i + 2 < raw) v |= byte(i + 2); out[p++] = alphabet[v >> 18]; out[p++] = alphabet[(v >> 12) & 63]; out[p++] = i + 1 < raw ? alphabet[(v >> 6) & 63] : '='; out[p++] = i + 2 < raw ? alphabet[v & 63] : '='; }
    out[p] = 0; bytes = p; return true;
}
void CaptureConverter::reset() { std::memset(history_, 0, sizeof(history_)); cursor_ = phase_ = 0; }
bool CaptureConverter::process(const int16_t *in, size_t n, int16_t *out, size_t cap, size_t &count) {
    count = 0; size_t need;
    if ((n && !in) || !output_count(n, 3, 2, phase_, need) || need > cap || (need && !out) || n > std::numeric_limits<size_t>::max() / 2) return false;
    for (size_t i = 0; i < n; ++i) {
        cursor_ = (cursor_ + 1) % 48;
        history_[cursor_] = (static_cast<int32_t>(in[i * 2]) + static_cast<int32_t>(in[i * 2 + 1])) * 0.5f;
        while (phase_ < 3) { out[count++] = sample(convolve(history_, cursor_, capture_fir[phase_], 32)); phase_ += 2; }
        phase_ -= 3;
    }
    return true;
}
void PlaybackConverter::reset() { std::memset(history_, 0, sizeof(history_)); cursor_ = phase_ = 0; }
bool PlaybackConverter::process(const int16_t *in, size_t n, int16_t *out, size_t cap, size_t &count) {
    count = 0; size_t need;
    if ((n && !in) || !output_count(n, 2, 3, phase_, need) || need > cap || (need && !out) || need > std::numeric_limits<size_t>::max() / 2) return false;
    for (size_t i = 0; i < n; ++i) {
        cursor_ = (cursor_ + 1) % 48; history_[cursor_] = in[i];
        while (phase_ < 2) { int16_t value = sample(convolve(history_, cursor_, playback_fir[phase_], 48)); out[count * 2] = out[count * 2 + 1] = value; ++count; phase_ += 3; }
        phase_ -= 2;
    }
    return true;
}
}
