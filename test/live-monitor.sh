#!/bin/sh
# The monitor stays usable while the machine runs: inspect, type into the
# console panel the guest is waiting on, and answer it, with no instruction-count guess
# anywhere.  docs/s36/live-monitor-design-2026-09-08.md
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$here"
. test/probe-common.sh
instantiate default-machine
out=$(cd "$here" && "$SIM36" -c "$TMP/default-machine.sim" -s test/live-monitor.sim 2>&1)


check "ipl returns to the prompt instead of owning it" \
      "execution: IPL started"
check "the monitor answers while the guest thread runs" \
      "machine running"
check "wait idle syncs on a condition, not an instruction count" \
      "wait: guest is idle after"
check "the attended SIGN ON panel reached the console" \
      "console: PUT 1,39 'SIGN ON"
check "the field table is readable while running" \
      "console: field  6,56 length   8"
check "typing into a running machine's field works" \
      "console: typed 'QSECOFR' at 6,56"
check "the operator response is submitted to the running guest" \
      "console: sent ENTER with 6,56='QSECOFR'"
check "stop leaves execution at a safe point" \
      "execution: stopped after"
# The running guest actually consumed the answer: it validated the date and
# repainted the panel.  Not an emulator-side echo - SYS-5519 is SSP's.
check "the running guest processes the answer and repaints" \
      "SYS-5519 Date or Time changed"

# Determinism yardsticks. The attended IPL is 59388 instructions and ends at
# nudspchA's no-task exit; answering the panel reaches 74295.  (The reference's
# copy of this script pins 59382/75497; the reference itself prints 59388 and
# 74295 today, and SIM/36 matches it line for line - docs/checkpoints/06-*.md.)
# Both numbers are deterministic for the same guest input and are preserved by
# bare `ipl` plus explicit `wait idle` synchronization.
# so continuous execution follows the SAME instruction stream, not a similar one.
check "asynchronous IPL matches the reference run exactly" \
      "wait: guest is idle after 59388 instruction(s)"
check "answering the panel costs the same instructions as the scripted form" \
      "wait: guest is idle after 74295 instruction(s)"
check "and stops for the reference reason" \
      "nudspchA's no-task exit (c180e04c)"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
