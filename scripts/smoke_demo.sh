#!/usr/bin/env bash
# Smoke test: the demo mode must start, render (offscreen) and stay alive.
# The application is killed by `timeout` - exit code 124 means "still running".
set -uo pipefail

BIN="${1:-build/bin/bitrix24-client}"
export QT_QPA_PLATFORM=offscreen

if [[ ! -x "$BIN" ]]; then
    echo "SMOKE FAIL: binary not found: $BIN" >&2
    exit 2
fi

timeout 5 "$BIN" --demo >/tmp/bx24_smoke.log 2>&1
rc=$?
if [[ $rc -eq 124 ]]; then
    echo "SMOKE OK: demo mode started and kept running (killed after 5s)"
    exit 0
fi
if [[ $rc -eq 0 ]]; then
    echo "SMOKE OK: demo mode exited cleanly"
    exit 0
fi
echo "SMOKE FAIL: unexpected exit code $rc" >&2
cat /tmp/bx24_smoke.log >&2
exit 1
