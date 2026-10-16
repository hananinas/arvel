#!/usr/bin/env bash
set -euo pipefail
# Compiles the pinned official SQLite, production owner/queue and checked disk VFS.
# Real temporary databases only; no network, hardware, provider or dependency installation.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/conversation-store-test.XXXXXXXX")"
trap 'rm -rf "$work"' EXIT
cc="${CC:-cc}"
cxx="${CXX:-c++}"
read -r -a cflags <<< "${CFLAGS:-}"
read -r -a cxxflags <<< "${CXXFLAGS:-}"
read -r -a ldflags <<< "${LDFLAGS:-}"
store="$root/components/conversation_store"
json="$root/managed_components/espressif__cjson/cJSON"
if [[ ! -f "$json/cJSON.c" || ! -f "$json/cJSON.h" ]]; then
    printf '%s\n' 'Missing downloaded real cJSON component; no dependencies will be installed.' >&2
    exit 1
fi
common=(-DCONVERSATION_STORE_HOST -include "$store/sqlite_config.h" -I"$store" -I"$store/include" -I"$store/vendor" -I"$json")
"$cc" -std=c11 -Os -g "${cflags[@]}" "${common[@]}" -c "$store/vendor/sqlite3.c" -o "$work/sqlite3.o"
"$cc" -std=c11 -O1 -g "${cflags[@]}" -I"$json" -c "$json/cJSON.c" -o "$work/cJSON.o"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra -pthread "${cxxflags[@]}" "${common[@]}" \
    -I"$root/components/agent_runtime/include" -c "$store/history_tool.cpp" -o "$work/history_tool.o"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra -pthread "${cxxflags[@]}" "${common[@]}" \
    "$store/conversation_store.cpp" "$store/checked_vfs.cpp" "$store/test/behavior.cpp" \
    "$work/sqlite3.o" "$work/cJSON.o" -lcrypto -lm "${ldflags[@]}" -o "$work/behavior"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra -pthread "${cxxflags[@]}" "${common[@]}" \
    "$store/checked_vfs.cpp" "$store/test/vfs.cpp" "$work/sqlite3.o" -lm "${ldflags[@]}" -o "$work/vfs"
"$work/vfs"
"$work/behavior"
