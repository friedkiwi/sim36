#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
. test/probe-common.sh
out=$(python3 test/ipl-unattended-main-session.py)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -F \
    "PASS: unattended IPL completed; W2 navigated MAIN -> MENU COMMAND -> MAIN -> PROGRAM and started SEU"
