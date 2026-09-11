#!/bin/sh
# Task termination probes: the timer and I/O queue cleanup and the retained
# task-status arm, both driven by a root SVC 11 at `ipl pause` with synthetic
# queue elements.  Synthetic: no SSP product code runs.
set -eu
cd "$(dirname "$0")/.."
. test/probe-common.sh

out=$(run nupterm-queue-cleanup)
checkin "nutetqdq removes the task's Q54 entry" \
      "nupterm nutetqdq dequeued task 0F00 element 090000 from queue 54" "$out"
checkin "nuteiopg removes the Q32 entry" \
      "nupterm nuteiopg dequeued task 0F00 element 090100 from queue 32" "$out"
checkin "nuteiopg removes the Q30 entry" \
      "nupterm nuteiopg dequeued task 0F00 element 090200 from queue 30" "$out"
checkin "nuteiopg removes the Q29 entry" \
      "nupterm nuteiopg dequeued task 0F00 element 090300 from queue 29" "$out"
checkin "the cleanup summary accounts for all four" \
      "timer/I-O queue cleanup removed 4 element(s) for task 0F00" "$out"
checkin "all four queue heads are empty" \
      "000b70  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00" "$out"

clear=$(run nupterm-retain-status-clear)
checkin "root SVC 11 with TB_STAT 00 takes the retain arm" \
      "nupterm no-return-ACE task 0F00 has tb+32=00, so status-derived r31=0" "$clear"
checkin "bit 0x80 clear: the status store and the post are skipped" \
      "nupterm retained context: tb+32 = 00 has bit 80 clear, so c18a4fa8 branches to c18a4fd0 - no status store and no nupotb" "$clear"
checkin "TB_STAT stays 00 (no request-block byte is planted)" \
      "000f20  00 00 00 00" "$clear"

set_=$(run nupterm-retain-status-set)
checkin "root SVC 11 with TB_STAT A0 still takes the retain arm (bit 0x40 clear)" \
      "nupterm no-return-ACE task 0F00 has tb+32=A0, so status-derived r31=0" "$set_"
checkin "bit 0x80 set: tb+32 = A0 & 7F = 20 and the task is posted 08" \
      "nupterm retained context: tb+32 bit 80 was set, so c18a4fbc stores tb+32 = A0 & 7F = 20 and calls nupotb(tb,08)" "$set_"
checkin "the post reached the task" \
      "nupterm retained-context nupotb: task block 0F00 posted 08" "$set_"
checkin "TB_STAT is 20 afterwards" \
      "000f20  20 00 00 00" "$set_"

scan=$(run nupterm-dependent-scan)
checkin "ACE target selects dependent" \
      "dependency ACE 90100 selects task 90000 for dying task 0F00" "$scan"
checkin "scan takes the deferred arm" \
      "deferred: tb+6=FF, tb+32 40->44" "$scan"
checkin "monitor reports one dependent" \
      "selected 1, immediate 0, deferred 1" "$scan"
checkin "tb+6, status and tb+48 mutations are exact" \
      "090000  e3 c2 00 00 00 00 ff 00 00 00 00 00 00 00 00 00" "$scan"
checkin "selected flag is present at tb+48" \
      "090030  01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00" "$scan"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
