#!/bin/sh
# The load-source selection: that each setting reaches the device it names, and
# that `disk` reaches neither.
#
# This is a differential test by construction - it asserts which DEVICE
# supervisor call the machine issues, which is a fact about the guest, not about
# our model of it.  A test that only checked the byte we wrote into word 1074
# would be the "model agreeing with its only caller" tautology.
set -u
cd "$(dirname "$0")/.."
. test/gate-common.sh

# `trace` BEFORE `ipl pause`: the panel is latched during the control
# processor's bring-up, which `ipl pause` performs, so tracing enabled
# afterwards misses it.  (The reference's copy of this gate traces, resets and
# then steps; since `reset` began releasing the construction latch, that
# sequence stops at "'step' requires a constructed machine" and the reference
# gate fails against the reference itself.  The sequence below is what the
# gate meant.)
printf 'trace svc,disk,csp\nipl pause\nstep 400000\nquit\n' > "$TMP/run.sim"

# NOTE: patterns for device calls include "iob=" on purpose.  The direct-area
# describer prints "...zero means DISKETTE (SVC 41), any bit set means TAPE
# (SVC 46)" when the word is read, so a bare "SVC 46" matches the PROSE as well
# as the call.
absent() {  # absent <name> <fixed-string pattern>
  if echo "$out" | grep -qF "$2"; then
    echo "  $1 FAIL  (should not contain: $2)"; fail=$((fail + 1))
  else
    echo "  $1 PASS"; pass=$((pass + 1))
  fi
}

run() {     # run <ipl_source> [ipl_type]
  # Attached `overlay`: each run is a fresh process, so the volume starts clean
  # every time without a scratch copy.
  cat > "$TMP/start.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
set machine ipl-type ${2:-unattend}
set machine ipl-source $1
attach disk0 $SIM36_VOLUME overlay
do run.sim
EOF
  out=$("$SIM36" -c "$TMP/start.sim" 2>&1)
  [ -n "${VERBOSE:-}" ] && echo "$out"
  return 0
}

# --- disk: no reload, so the load-source byte is never consulted -------------
# The phase-1 gate performs its two stores when the IPL parameter byte's bit 01
# is SET.  A disk IPL is the absence of a request, so the byte's bit 01 is clear
# and neither store happens.  The byte comes from the machine's configuration,
# not from the volume: power-on regenerates the UDT before the main-storage
# IPL and phase 1 stamps it 0xFF on the way past.
run disk
check  "disk      phase 1 leaves the gate clear " 'bit 01 CLEAR -> no reload requested'
check  "disk      no reload requested           " 'no reload requested'
absent "disk      never asks for a diskette     " 'SVC 41 iob='
absent "disk      never asks for a tape         " 'SVC 46 iob='
# With the module's bytes in storage of their own, real 0x1000 is free, the
# volume-geometry compare passes, and phase 1 reaches the #LIBRARY work and
# asks the disk to SCAN sector 75441 through a translated buffer.
check  "disk      reaches the LIBRARY scan      " 'sector=75441'
check  "disk      through a translated buffer   " 'buffer=806000'

# --- diskette: bits 1C clear -> SVC 41 ---------------------------------------
run diskette
check  "diskette  panel raises the reload gate  " "load source 'diskette' -> IPL parameter byte bit 01 SET"
check  "diskette  word 1074 = 0000              " 'word 1074 = 0000'
check  "diskette  resolves to DISKETTE          " 'DISKETTE (SVC 41)'
check  "diskette  really issues SVC 41          " 'SVC 41 iob='
absent "diskette  and never SVC 46              " 'SVC 46 iob='

# --- tape: any bit of 1C -> SVC 46 -------------------------------------------
run tape
check  "tape      word 1074 = 0008              " 'word 1074 = 0008'
check  "tape      resolves to TAPE arm 08       " 'TAPE (SVC 46), arm 08'
check  "tape      really issues SVC 46          " 'SVC 46 iob='
absent "tape      and never SVC 41              " 'SVC 41 iob='

run tape-04
check  "tape-04   resolves to TAPE arm 04       " 'TAPE (SVC 46), arm 04'
check  "tape-04   really issues SVC 46          " 'SVC 46 iob='

run tape-10
check  "tape-10   resolves to TAPE arm 10       " 'TAPE (SVC 46), arm 10'
check  "tape-10   really issues SVC 46          " 'SVC 46 iob='

# --- the attended flag is bit 0x80 of the same byte --------------------------
# Phase 1 consumes it in two extra instructions and clears it so it can reuse
# the bit.  The claim is the DIFFERENCE, so both ends are pinned; the absolute
# numbers move whenever a real defect below this path is fixed; the reference
# measured 5332/5334 on 2026-09-12 (its own gate text still says 5327/5329).
run diskette
check  "diskette  baseline is 5332             " 'stopped after 5332 instruction(s)'
run diskette attend
check  "attend    sets bit 80 of word 1074      " 'word 1074 = 0080'
check  "attend    named in the trace            " 'attended flag on'
check  "attend    costs exactly 2 instructions  " 'stopped after 5334 instruction(s)'

# --- the raw escape hatch still reaches the dispatch -------------------------
run 0x1C
check  "0x1C      raw byte accepted             " 'word 1074 = 001C'
check  "0x1C      any 1C bit is tape            " 'SVC 46 iob='

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
