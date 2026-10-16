#include "voice_commands.hpp"
#include <cassert>
#include <iostream>

int main() {
    for (const char *text : {"end", "END!", "Arvel, end.", "stop listening", "That's all.", "We're done.", "I'm done."})
        assert(buddy_voice::is_end_command(text, "Arvel"));
    for (const char *text : {"", "Arvel", "Hey Arvel", "The end is near", "Don't stop listening", "How do I end a chat?", "endless", "Arvel Arvel end"})
        assert(!buddy_voice::is_end_command(text, "Arvel"));
    assert(buddy_voice::is_end_command("Nova, hang up!", "Nova"));
    assert(!buddy_voice::is_end_command("Nova", "Nova"));
    assert(buddy_voice::is_end_command("明, end", "明"));
    assert(!buddy_voice::is_end_command("明", "明"));
    std::cout << "Spoken end boundaries passed\n";
}
