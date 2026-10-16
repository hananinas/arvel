#include "speech_live_protocol.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using speech_live_protocol::Frame;
using speech_live_protocol::Message;
void check(bool condition, const char *name) {
    if (!condition) { std::fprintf(stderr, "live protocol: %s\n", name); std::abort(); }
}
bool header(std::initializer_list<uint8_t> bytes, Frame &frame) {
    return speech_live_protocol::decode_header(bytes.begin(), bytes.size(), frame);
}
int main() {
    Frame frame;
    check(header({0x81, 3}, frame) && frame.final && frame.opcode == 1 && frame.length == 3, "small text");
    check(header({0x82, 126, 0x10, 0}, frame) && frame.length == 4096, "RFC binary header decoded before application rejection");
    check(header({0x81, 127, 0, 0, 0, 0, 0, 1, 0, 0}, frame) && frame.length == 65536, "message cap inclusive");
    check(!header({0x81, 127, 0, 0, 0, 0, 0, 1, 0, 1}, frame), "message cap overflow");
    check(!header({0x81, 127, 0x80, 0, 0, 0, 0, 0, 0, 0}, frame), "64-bit oversized payload");
    check(!header({0xc1, 0}, frame), "RSV extensions rejected");
    check(!header({0x81, 0x80}, frame), "masked server frame rejected");
    check(!header({0x83, 0}, frame), "reserved opcode rejected");
    check(!header({0x09, 0}, frame), "fragmented ping rejected");
    check(!header({0x89, 126, 0, 126}, frame), "oversized control rejected");
    check(!header({0x81, 126, 0, 125}, frame), "nonminimal 16-bit encoding rejected");
    check(!header({0x81, 127, 0, 0, 0, 0, 0, 0, 255, 255}, frame), "nonminimal 64-bit encoding rejected");
    check(!header({0x81, 126}, frame), "partial header not decoded prematurely");

    Message message;
    check(!message.begin(Frame{0, true, 1}), "orphan continuation rejected");
    check(message.begin(Frame{1, false, 2}), "start fragmented text");
    check(!message.finish(Frame{1, false, 2}), "first fragment not emitted");
    check(!message.begin(Frame{2, true, 4}), "new message inside fragments rejected");
    check(message.begin(Frame{9, true, 2}) && !message.finish(Frame{9, true, 2}) && message.length == 2,
          "interleaved ping preserves message boundary");
    check(message.begin(Frame{0, false, 3}) && !message.finish(Frame{0, false, 3}), "middle continuation");
    check(message.begin(Frame{0, true, 1}) && message.finish(Frame{0, true, 1}) && message.length == 6 && message.opcode == 1,
          "text fragments emitted exactly at FIN");
    check(!message.begin(Frame{2, true, 4}), "binary audio rejected on the real provider sideband");
    check(!message.begin(Frame{2, false, 4096}), "fragmented binary rejected before payload allocation");
    check(message.begin(Frame{1, false, 65536}) && !message.finish(Frame{1, false, 65536}), "fragmented text at cap");
    check(!message.begin(Frame{0, true, 1}), "fragmented text aggregate overflow rejected");

    const char *unicode = "voice \xc3\xa9 \xf0\x9f\x8e\xa4";
    check(speech_live_protocol::utf8(unicode, std::strlen(unicode)), "valid UTF-8");
    check(!speech_live_protocol::utf8("\xc0\xaf", 2), "overlong UTF-8 rejected");
    check(!speech_live_protocol::utf8("\xed\xa0\x80", 3), "UTF-8 surrogate rejected");
    check(!speech_live_protocol::utf8("\xf4\x90\x80\x80", 4), "out-of-range UTF-8 rejected");
    check(!speech_live_protocol::utf8("\xe2\x82", 2), "partial UTF-8 rejected after message reassembly");
    check(!speech_live_protocol::utf8("a\0b", 3), "embedded NUL rejected");
    std::puts("live websocket protocol checks passed");
}
