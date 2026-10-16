#pragma once

#include <cstddef>
#include <cstdint>

namespace speech_live_protocol {
constexpr size_t max_message = 65536;

struct Frame {
    uint8_t opcode = 0;
    bool final = false;
    size_t length = 0;
};

// Server frames must be unmasked and cannot negotiate extensions. Call with
// exactly 2, 4 or 10 bytes, as selected by the second header byte.
inline bool decode_header(const uint8_t *bytes, size_t count, Frame &frame) {
    if (count < 2 || (bytes[0] & 0x70) || (bytes[1] & 0x80)) return false;
    frame.opcode = bytes[0] & 15;
    frame.final = (bytes[0] & 0x80) != 0;
    if (frame.opcode != 0 && frame.opcode != 1 && frame.opcode != 2 &&
        frame.opcode != 8 && frame.opcode != 9 && frame.opcode != 10) return false;
    uint64_t length = bytes[1] & 127;
    const size_t expected = length == 126 ? 4 : length == 127 ? 10 : 2;
    if (count != expected) return false;
    if (expected > 2) {
        length = 0;
        for (size_t index = 2; index < count; ++index) length = (length << 8) | bytes[index];
        if ((expected == 4 && length < 126) || (expected == 10 && length < 65536)) return false;
    }
    if (length > max_message || ((frame.opcode & 8) && (!frame.final || length > 125))) return false;
    frame.length = static_cast<size_t>(length);
    return true;
}

struct Message {
    size_t length = 0;
    uint8_t opcode = 0;
    bool fragmented = false;

    bool begin(const Frame &frame) {
        if (frame.opcode & 8) return true; // Controls never alter message boundaries.
        if (frame.opcode == 0) {
            if (!fragmented) return false;
        } else {
            if (fragmented || frame.opcode != 1) return false; // Real sideband is text-only.
            opcode = frame.opcode;
            length = 0;
        }
        return length <= max_message && frame.length <= max_message - length;
    }
    // Caller first reads exactly frame.length bytes into storage + length.
    bool finish(const Frame &frame) {
        if (frame.opcode & 8) return false;
        length += frame.length;
        fragmented = !frame.final;
        return frame.final;
    }
};

inline bool utf8(const char *text, size_t length) {
    const auto *bytes = reinterpret_cast<const uint8_t *>(text);
    for (size_t index = 0; index < length;) {
        uint32_t value = bytes[index++];
        if (value < 0x80) { if (!value) return false; continue; }
        unsigned extra;
        uint32_t minimum;
        if (value >= 0xc2 && value <= 0xdf) { extra = 1; minimum = 0x80; value &= 31; }
        else if (value >= 0xe0 && value <= 0xef) { extra = 2; minimum = 0x800; value &= 15; }
        else if (value >= 0xf0 && value <= 0xf4) { extra = 3; minimum = 0x10000; value &= 7; }
        else return false;
        if (extra > length - index) return false;
        while (extra--) {
            const uint8_t next = bytes[index++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
} // namespace speech_live_protocol
