#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
. test/gate-common.sh
out=$(python3 test/list-all-regression.py)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -F \
    "PASS: LIST ALL displayed source without a processor check"
