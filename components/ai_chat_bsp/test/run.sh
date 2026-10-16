#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/desk-buddy-test.XXXXXXXX")"
trap 'rm -rf "$work"' EXIT
json="$root/managed_components/espressif__cjson/cJSON"
cc="${CC:-cc}"; cxx="${CXX:-c++}"
read -r -a cflags <<< "${CFLAGS:-}"
read -r -a cxxflags <<< "${CXXFLAGS:-}"
read -r -a ldflags <<< "${LDFLAGS:-}"
"$cc" -std=c11 -O1 -g -Wall -Wextra "${cflags[@]}" -I"$json" -c "$json/cJSON.c" -o "$work/cJSON.o"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra -pthread "${cxxflags[@]}" \
    -I"$json" -I"$root/components/agent_runtime/include" -I"$root/components/ai_chat_bsp" "$root/components/ai_chat_bsp/desk_buddy.cpp" \
    "$root/components/ai_chat_bsp/test/desk_behavior.cpp" "$work/cJSON.o" "${ldflags[@]}" -o "$work/desk_behavior"
timeout 20s "$work/desk_behavior"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra "${cxxflags[@]}" \
    -I"$root/components/ai_chat_bsp" "$root/components/ai_chat_bsp/test/voice_commands.cpp" \
    "${ldflags[@]}" -o "$work/voice_commands"
timeout 5s "$work/voice_commands"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra "${cxxflags[@]}" \
    -I"$json" -I"$root/components/ai_chat_bsp/test/include" \
    -I"$root/components/web_auth/test/include" -I"$root/components/ui_bsp" \
    -I"$root/components/ai_chat_bsp" "$root/components/ai_chat_bsp/buddy_settings.cpp" \
    "$root/components/ai_chat_bsp/test/settings_behavior.cpp" "$work/cJSON.o" \
    "${ldflags[@]}" -o "$work/settings_behavior"
timeout 5s "$work/settings_behavior"
