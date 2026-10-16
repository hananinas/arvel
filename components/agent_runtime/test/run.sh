#!/usr/bin/env bash
set -euo pipefail
# Actual C++ Agent, Rust RPC/runtime and cJSON; scripted provider only.
# No SDK configuration, microphone, provider network or dependency installation.
# Optional CFLAGS/CXXFLAGS/LDFLAGS follow the existing host test convention.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/agent-runtime-test.XXXXXXXX")"
trap 'rm -rf "$work"' EXIT
cc="${CC:-cc}"
cxx="${CXX:-c++}"
read -r -a cflags <<< "${CFLAGS:-}"
read -r -a cxxflags <<< "${CXXFLAGS:-}"
read -r -a ldflags <<< "${LDFLAGS:-}"
json="$root/managed_components/espressif__cjson/cJSON"
if [[ ! -f "$json/cJSON.c" || ! -f "$json/cJSON.h" ]]; then
    printf '%s\n' 'Missing downloaded real cJSON component; no dependencies will be installed.' >&2
    exit 1
fi
host=""
while IFS= read -r line; do
    if [[ "$line" == 'host: '* ]]; then host="${line#host: }"; fi
done < <(rustc +stable -vV)
[[ -n "$host" ]]
target="$root/build/agent-runtime-host"
CARGO_TARGET_DIR="$target" cargo +stable build --offline --locked --release \
    --target "$host" --manifest-path "$root/components/agent_runtime/rust/Cargo.toml"
CARGO_TARGET_DIR="$target" cargo +stable test --offline --locked --lib \
    --target "$host" --manifest-path "$root/components/agent_runtime/rust/Cargo.toml"
"$cc" -std=c11 -O1 -g -Wall -Wextra "${cflags[@]}" -I"$json" \
    -c "$json/cJSON.c" -o "$work/cJSON.o"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra -pthread "${cxxflags[@]}" \
    -I"$root/components/agent_runtime/include" -I"$json" \
    "$root/components/agent_runtime/agent_runtime.cpp" \
    "$root/components/agent_runtime/test/behavior.cpp" "$work/cJSON.o" \
    "$target/$host/release/libunravel_agent_esp32.a" -ldl -lm "${ldflags[@]}" -o "$work/agent_behavior"
"$work/agent_behavior"
