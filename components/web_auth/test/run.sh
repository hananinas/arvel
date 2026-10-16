#!/usr/bin/env bash
set -euo pipefail
# Actual production web_auth.c, with real OpenSSL crypto and explicit ESP-IDF
# API fixtures. No package installation or target firmware changes.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
work="$(mktemp -d "${TMPDIR:-/tmp}/web-auth-test.XXXXXXXX")"
trap 'rm -rf "$work"' EXIT
cc="${CC:-cc}"
"$cc" -std=c11 -Wall -Wextra -Werror -pthread \
    -I"$root/components/web_auth/test/include" -I"$root/components/web_auth" \
    "$root/components/web_auth/web_auth.c" \
    "$root/components/web_auth/test/host_fixture.c" \
    "$root/components/web_auth/test/core_smoke.c" \
    -lcrypto -o "$work/web_auth_smoke"
for scenario in core physical-first unavailable corrupt wronglength readfailure; do
    "$work/web_auth_smoke" "$scenario" "$work/$scenario.record"
done
