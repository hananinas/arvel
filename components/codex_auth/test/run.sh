#!/usr/bin/env bash
set -euo pipefail
# Compile the actual unchanged production auth implementation and real cJSON
# read/type helpers. Only ESP/FreeRTOS/NVS/HTTP and BoundedHttps are host models.
# No provider network, dependency install, SDK private configuration or tokens.
# Each scenario is a fresh process. Reload probes open the same actual NVS file
# in another process, rather than resetting production globals inside a test.
#
# Optional ASan/UBSan (including production's custom wipe-on-free JSON parser):
# CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
# CXXFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
# LDFLAGS='-fsanitize=address,undefined' bash components/codex_auth/test/run.sh
# CC and CXX select existing host compilers; flags are whitespace-separated.
# SCENARIOS='reject-escape refresh-reject-nul' selects isolated scenarios.
#
# Limitations: scripted HTTP verifies config/grant bodies and cancellation /
# total-deadline behavior, not real TLS/DNS or provider/account eligibility.
# Host threads and notifications execute the real production login/watchdog;
# fixture delay yields do not advance either independently controlled clock.
# NVS uses a bounded staged file with injected set/erase and commit failures.
# Its atomic rename is NOT ESP-IDF flash emulation: nvs_set_blob can write flash
# before commit; actual power loss / failed commit can leave old OR new records.
# Capacity includes old+new blob storage and conservative overhead, not exact
# ESP-IDF page fragmentation, garbage collection or flash wear accounting.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/codex-auth-test.XXXXXXXX")"
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
"$cc" -std=c11 -O1 -g -Wall -Wextra "${cflags[@]}" -I"$json" \
    -c "$json/cJSON.c" -o "$work/cJSON.o"
"$cxx" -std=c++17 -O1 -g -Wall -Wextra -pthread "${cxxflags[@]}" \
    -I"$root/components/codex_auth/test/include" \
    -I"$root/components/codex_auth/include" \
    -I"$root/components/speech_stt/include" -I"$json" \
    "$root/components/codex_auth/codex_auth.cpp" \
    "$root/components/codex_auth/test/host_fixture.cpp" \
    "$root/components/codex_auth/test/behavior.cpp" "$work/cJSON.o" \
    -Wl,--wrap=time "${ldflags[@]}" -o "$work/codex_auth_behavior"
defaults='login cancel-restart expiration logout-refresh logout-authorization concurrent-refresh refresh-deadline refresh-external-cancel refresh-provider refresh-stage refresh-commit refresh-full login-stage login-commit login-full disconnect-stage disconnect-commit corrupt reject-malformed reject-duplicate reject-nul reject-account reject-escape reject-number reject-trailing reject-deep reject-raw-nul refresh-reject-malformed refresh-reject-duplicate refresh-reject-nul refresh-reject-account refresh-reject-refresh-account refresh-reject-escape refresh-reject-number refresh-reject-trailing refresh-reject-deep refresh-reject-raw-nul'
read -r -a scenarios <<< "${SCENARIOS:-$defaults}"
for scenario in "${scenarios[@]}"; do
    record="$work/$scenario.record"
    "$work/codex_auth_behavior" "$scenario" "$record"
    case "$scenario" in
        logout-*|disconnect-*) reload=reload-empty ;;
        concurrent-refresh) reload=reload-refreshed ;;
        *) reload=reload ;;
    esac
    "$work/codex_auth_behavior" "$reload" "$record"
done
