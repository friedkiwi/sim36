#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
. test/probe-common.sh
out=$(python3 test/multi-session-signon.py)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -F \
    "PASS: W1, W2, and W3 reached MAIN concurrently"
