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

# Determinism yardsticks.  A clean attended IPL is 71796 instructions and
# answering the panel reaches 78784.  Earlier totals (51458/58444) predate
# the printer IPL rebuild path, which runs more of SSP's file rebuild before
# the console idles; the totals before those included #CTEI and FETDP runs
# caused by treating every successful return-ACE task as an error
# termination, which also left SYS-1887 entries in HISTORY.  Both numbers
# are deterministic for this guest input and are preserved by bare `ipl`
# plus explicit `wait idle` synchronization.
check "asynchronous IPL follows the clean deterministic stream" \
      "wait: guest is idle after 71796 instruction(s)"
check "answering the panel follows the clean deterministic stream" \
      "wait: guest is idle after 78784 instruction(s)"
check "and stops for the reference reason" \
      "nudspchA's no-task exit (c180e04c)"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
