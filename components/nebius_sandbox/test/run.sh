#!/usr/bin/env bash
set -euo pipefail
# Real Tools + C++ Agent + Rust runtime + cJSON; only cloud transport/provider scripted.
# No credentials, provider network, dependency installation or local guest-code execution.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/nebius-sandbox-test.XXXXXXXX")"
trap 'rm -rf "$work"' EXIT
cc="${CC:-cc}"; cxx="${CXX:-c++}"
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
"$cc" -std=c11 -O1 -g -Wall -Wextra "${cflags[@]}" -I"$json" \
    -c "$json/cJSON.c" -o "$work/cJSON.o"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra -pthread "${cxxflags[@]}" \
    -I"$json" -I"$root/components/agent_runtime/include" -I"$root/components/nebius_sandbox/include" \
    "$root/components/nebius_sandbox/nebius_sandbox.cpp" \
    "$root/components/agent_runtime/agent_runtime.cpp" \
    "$root/components/nebius_sandbox/test/behavior.cpp" "$work/cJSON.o" \
    "$target/$host/release/libunravel_agent_esp32.a" -ldl -lm "${ldflags[@]}" -o "$work/nebius_behavior"
timeout 20s "$work/nebius_behavior"
