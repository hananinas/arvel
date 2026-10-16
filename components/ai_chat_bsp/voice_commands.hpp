#pragma once

#include <string>

namespace buddy_voice {
inline std::string normalize_command(const char *text) {
    std::string out;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        const unsigned char c = *p >= 'A' && *p <= 'Z' ? *p + ('a' - 'A') : *p;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80) out += static_cast<char>(c);
        else if (c == '\'') continue;
        else if (!out.empty() && out.back() != ' ') out += ' ';
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Only an entire final user utterance is a command, optionally addressed to
// the configured buddy. Mentioning the buddy or "end" in a sentence is not.
inline bool is_end_command(const char *text, const char *buddy_name) {
    static const char *const commands[] = {
        "end", "end chat", "end the chat", "end conversation", "end the conversation",
        "end session", "stop", "stop listening", "stop chat", "goodbye", "good bye",
        "bye", "hang up", "thats all", "that is all", "were done", "im done", "i am done"
    };
    std::string value = normalize_command(text);
    const std::string names[] = {normalize_command(buddy_name), "arvel"};
    for (const std::string &name : names) {
        if (!name.empty() && value.size() > name.size() && value.compare(0, name.size(), name) == 0 &&
            value[name.size()] == ' ') {
            value.erase(0, name.size() + 1);
            break;
        }
    }
    for (const char *command : commands) if (value == command) return true;
    return false;
}
} // namespace buddy_voice
