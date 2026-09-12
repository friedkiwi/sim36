#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
. test/probe-common.sh

out=$(python3 test/machine-lifecycle.py 2>&1)
pass=0 fail=0
check() {
    if printf '%s\n' "$out" | grep -qF "$2"; then
        echo "  $1 PASS"; pass=$((pass + 1))
    else
        echo "  $1 FAIL"; fail=$((fail + 1))
    fi
}

check "listener exists before construction" "multiplexer accepted W2 before IPL"
check "IPL constructs around an existing client" "preconnected W2 reached SIGN ON after lazy construction"
check "reset preserves and re-presents the client" "same W2 socket survived reset and reached SIGN ON after reconstruction"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
