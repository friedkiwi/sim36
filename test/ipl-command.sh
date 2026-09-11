#!/bin/sh
# `ipl pause`, one-step execution and the concise normal SRC line.
set -eu
cd "$(dirname "$0")/.."
. test/probe-common.sh
out=$(run ipl-command)
checkin "IPL started line" "IPL started: advanced36, disk $(basename "$SIM36_VOLUME"), console W1 (monitor)" "$out"
checkin "IPL paused line" "IPL paused before instruction 1; use 'step N' or 'start'" "$out"
checkin "no instruction executed yet" "stopped=False  instructions=0" "$out"
checkin "one step" "1 instruction(s)" "$out"
checkin "one instruction executed" "stopped=False  instructions=1" "$out"
checkin "normal SRC line" "SRC posted: 0000  running normally" "$out"
if printf '%s\n' "$out" | grep -qF "DSPM36 shows"; then
  echo "  FAIL  normal SRC message still exposes the DSPM36 diagnostic"; fail=$((fail + 1))
else
  echo "  PASS  concise normal SRC output"; pass=$((pass + 1))
fi
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
