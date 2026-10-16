#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/niko-protocol-test.XXXXXXXX")"
trap 'rm -rf "$work"' EXIT
json="$root/managed_components/espressif__cjson/cJSON"
cc="${CC:-cc}"; cxx="${CXX:-c++}"
read -r -a cflags <<< "${CFLAGS:-}"
read -r -a cxxflags <<< "${CXXFLAGS:-}"
read -r -a ldflags <<< "${LDFLAGS:-}"
"$cc" -std=c11 -O1 -g -Wall -Wextra "${cflags[@]}" -I"$json" -c "$json/cJSON.c" -o "$work/cJSON.o"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra "${cxxflags[@]}" \
    -I"$json" -I"$root/components/niko_home_control" \
    -I"$root/components/niko_home_control/include" -I"$root/components/agent_runtime/include" \
    "$root/components/niko_home_control/niko_protocol.cpp" \
    "$root/components/niko_home_control/test/behavior.cpp" "$work/cJSON.o" \
    "${ldflags[@]}" -o "$work/niko_behavior"
timeout 10s "$work/niko_behavior"
