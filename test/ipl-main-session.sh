#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
. test/probe-common.sh
out=$(python3 test/ipl-main-session.py)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -F \
    "PASS: attended IPL completed; W2 reached MAIN and navigated option 1 to MENU COMMAND"
