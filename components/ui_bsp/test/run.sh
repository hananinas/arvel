#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/arvel-ui-test.XXXXXXXX")"
trap 'rm -rf "$work"' EXIT
# Production LVGL, UI and retained assets; no hardware, audio, providers or installs.
cmake -S "$root/components/ui_bsp/test" -B "$work" -DCMAKE_BUILD_TYPE=Debug
cmake --build "$work" --target ui_behavior -j2
"$work/ui_behavior"
