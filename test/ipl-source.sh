#!/bin/sh
# The single IPL-source selection controls both the control-processor phase-1
# bootstrap and the reload source that phase 1 sees.
#
# The disk case follows the guest far enough to prove the normal path remains
# functional.  With no removable media attached, the other cases prove the CSP
# tries the selected device rather than silently bootstrapping from disk.
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
do $ROOT/test/advanced36-nolisten.sim
set machine ipl-type ${2:-unattend}
set machine ipl-source $1
attach disk0 $SIM36_VOLUME overlay
do run.sim
EOF
  out=$("$SIM36" -c "$TMP/start.sim" 2>&1)
  [ -n "${VERBOSE:-}" ] && echo "$out"
  return 0
}

# --- disk: no reload, so the IPL-source byte is never consulted --------------
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
check  "diskette  selects removable bootstrap  " 'ipl_source = diskette, but the diskette drive is empty'
absent "diskette  does not fall back to disk    " 'sector=8191'

# --- tape: any bit of 1C -> SVC 46 -------------------------------------------
run tape
check  "tape      selects removable bootstrap  " 'ipl_source = tape, but the tape drive is empty or unloaded'
absent "tape      does not fall back to disk    " 'sector=8191'

# --- the attended flag is bit 0x80 of the same byte --------------------------
# It is independent of the source selection, so verify it on a normal disk IPL.
run disk attend
check  "attend    sets bit 80 of word 1074      " 'word 1074 = 0080'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
