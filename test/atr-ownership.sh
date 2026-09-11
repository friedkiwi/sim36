#!/bin/sh
# The ATR file belongs to the REQUEST BLOCK, not to the task group.
#
# `nucratr` does not address the translation registers directly. It takes the
# handle at `rb+56..58`, adds the pool base `NuEmul[0x1160]`, loads
# `NuPtt[0x128]` and compares it against the request block it was called for,
# and only then does it have an array to fill (c189716c..c1897198). `nuprbbld`
# (c18a62d8) and `csipl` (c1831968) give every request block one of its own;
# `nuprbf2` (c18a5c2c) hands it back. `nup2000`, `nupexit` and `nudspchA` run
# the same ownership check and REPOINT the live file rather than rebuilding it,
# which is SA21-9436 1-29's `A5 PATR - Fast task switch for ATRs`.
#
# What this suite exists to prevent is the regression that made it necessary: a
# single ATR file shared per task group, where a callee's `nucratr` destroyed
# its caller's mapping and MSIPL phase 2 then faulted on its own next translated
# instruction fetch at logical 1C26.
#
# Part 1 is the real sequence, out of the IPL: phase 2 calls a transient whose
# `pb+52` attribute is 02, for which `nucratr` maps no module pages at all, and
# comes back to 1C26 with its own thirteen pages still mapped.
# Part 2 forces the ownership check to fail and checks that it refuses.
#
# docs/s36/storage-protection.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh


# ---------------------------------------------------------------------------
# Part 1: the IPL, where the caller and the callee really do own different files
# ---------------------------------------------------------------------------
# Phase 1 writes to the volume, so this half needs the writable scratch copy the
# shipped config already points at.

cat > "$TMP/ipl.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
trace csp
ipl pause
step 400000
show ptt
show atr
quit
EOF

# This suite attaches no 5250 client, so its command file defines no listener.
ipl=$("$SIM36" -c "$TMP/ipl.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$ipl"

checkre() {  # checkre <name> <extended regex>
  # Used wherever a guest ADDRESS would otherwise be pinned. Heap placement is
  # NOT this suite's subject - it is system-queue-space.sh's - and pinning it
  # here made seven assertions fail the moment the allocator was made faithful,
  # for no change in ATR behaviour at all. Assert the relationship instead.
  if echo "$3" | grep -qE "$2"; then
    echo "  $1 PASS"; pass=$((pass + 1))
  else
    echo "  $1 FAIL  (no match: $2)"; fail=$((fail + 1))
  fi
}
checkno() { # checkno <name> <fixed-string pattern that must NOT appear>
  if echo "$3" | grep -qF "$2"; then
    echo "  $1 FAIL  (found, and should not have: $2)"; fail=$((fail + 1))
  else
    echo "  $1 PASS"; pass=$((pass + 1))
  fi
}

# csipl c1831968..c18319f8 builds the IPL block's file the same way nuprbbld
# builds every other one, and the handle lands in the guest field rb+56..58.
check "csipl gives the IPL block a file  " \
  'csipl: request block 0E00 owns ATR file 0000' "$ipl"

# nuprbbld c18a62d8: MSIPL phase 2's block gets a file of its OWN, a different
# one, and fills it with its thirteen module pages.
checkre "phase 2 owns a different file   " \
  'SVC 10: request block [0-9A-F]+ owns ATR file 0138' "$ipl"
check "...and nucratr fills THAT file     " \
  'nucratr: 13 module ATR(s) from index 2 into ATR file 0138' "$ipl"

# The transient: attribute 02, so nucratr maps no module pages at all. Under one
# shared file this is the build that wiped phase 2's mapping out.
checkre "the transient owns its own file " \
  'SVC 10: request block [0-9A-F]+ owns ATR file 0000' "$ipl"
check "...whose build maps no module pages" \
  'nucratr: 0 module ATR(s) from index 2 into ATR file 0000' "$ipl"

# nuprbf2 c18a5c2c: the callee's file goes back to the free list on the way out.
checkre "SVC 11 releases the callee's file" \
  'SVC 11: ATR file [0-9A-F]+ released by request block [0-9A-F]+' "$ipl"

# The point of the whole exercise. c18a415c..c18a4198: pb+18 == pb+20 and
# rb+44 bit 0x40 clear, so nupexit returns WITHOUT calling nucratr - which is
# only sound because the caller's own file was never touched.
checkre "nupexit skips nucratr as written" \
  'SVC 11: nupexit skips nucratr - program block [0-9A-F]+ is ready and rb\+44 bit 40 is clear' "$ipl"

# ...and the caller resumes at the address that used to fault.
checkre "phase 2 resumes at 1C26         " \
  'SVC 11: returned to request block [0-9A-F]+, program block [0-9A-F]+, 1C26 with prefix 80' "$ipl"
checkno "no protection violation at 1C26   " \
  'storage protection violation at 1C26' "$ipl"

# Re-derived 2026-08: these four used to pin the END STATE of a run that
# stopped mid-phase-2 - which files existed at the stop and which pages the
# phase-2 module's file held. The end state is a property of wherever the
# frontier happens to be (the run now ends deep in phase 3's wait, with four
# frames live), so the same CLAIMS are asserted as run events instead.

# A release really clears ownership: the release trace names it, and the same
# file number is later handed to a NEW owner - impossible unless the release
# freed it.
released=$(echo "$ipl" | grep -oE 'ATR file [0-9A-F]+ released' | head -1 | awk '{print $3}')
if [ -n "$released" ] && echo "$ipl" | grep -qE "owns ATR file $released" ; then
  echo "  a released file is handed out again PASS"; pass=$((pass + 1))
else
  echo "  a released file is handed out again FAIL  (file $released never reused)"
  fail=$((fail + 1))
fi

# The live registers are exactly some constructed file's registers - the ptt
# and the live ATR agree, whichever file is current at the stop.
live=$(echo "$ipl" | grep -E '^ATR  0:' | head -1 | sed 's/^ATR  0: //')
if [ -n "$live" ] && echo "$ipl" | grep -E '^  file [0-9A-F]+  owner' | grep -qF "$live"; then
  echo "  the live registers are a real file  PASS"; pass=$((pass + 1))
else
  echo "  the live registers are a real file  FAIL  (ATR 0 = '$live' matches no file)"
  fail=$((fail + 1))
fi

# Reuse is a property of the WHOLE run: far more frames own files than distinct
# files are ever constructed. A pool that grew per call would show one file per
# frame.
files=$(echo "$ipl" | grep -oE 'owns ATR file [0-9A-F]+' | sort -u | wc -l)
grants=$(echo "$ipl" | grep -cE 'owns ATR file [0-9A-F]+')
if [ "$grants" -gt "$files" ] && [ "$files" -le 6 ]; then
  echo "  the free list is reused, not grown PASS  ($grants grants over $files files)"; pass=$((pass + 1))
else
  echo "  the free list is reused, not grown FAIL  ($grants grants over $files files)"
  fail=$((fail + 1))
fi

# And nothing regressed. This deliberately does NOT pin which supervisor call the
# run stops on: that is the IPL frontier, it moves whenever anything ahead of it
# is fixed, and pinning it here made this suite fail for a reason that has
# nothing to do with ATR ownership. What matters is that the fault this suite
# exists for - a protection violation from a clobbered ATR file at 1C26 - does
# not come back, and that the run still gets past where it used to die there.
#
# It is pinned to `at 1C26` on purpose. A bare 'storage protection violation'
# would also catch the level-5 the run now takes DOWNSTREAM, when the IPL task
# walks an uninitialised all-FF pointer into a protected page and the control
# processor abnormally terminates it (docs/s36/storage-protection.md). That is a
# different, correctly handled event, not the clobbered-ATR regression this
# assertion guards.
checkno "no clobbered-ATR fault at 1C26     " 'storage protection violation at 1C26' "$ipl"
reached=$(echo "$ipl" | grep -oE 'stopped after [0-9]+' | grep -oE '[0-9]+')
if [ -n "$reached" ] && [ "$reached" -gt 10900 ]; then
  echo "  the IPL runs past the old 1C26 fault PASS"; pass=$((pass + 1))
else
  echo "  the IPL runs past the old 1C26 fault FAIL  (reached ${reached:-none})"
  fail=$((fail + 1))
fi

# ---------------------------------------------------------------------------
# Part 2: the ownership check refuses a block that does not own the file it names
# ---------------------------------------------------------------------------
# `NuPtt[0x128]` is not decoration. Point the live request block's rb+56..58 at
# a file it does not own and the check has to fail closed: on the machine
# `nucratr` would fill through a null pointer, so the only honest answer is to
# build nothing and leave every register protected.
python3 test/build-storage-vectors.py "$TMP/vectors.bin" "$TMP/program.bin"
printf '\377\377\377' > "$TMP/badhandle.bin"

# 0x0E00 + 56 = 0x0E38, the handle field of the request block csipl built.
cat > "$TMP/bad.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
ipl pause
loadfile $TMP/vectors.bin 0C00
loadfile $TMP/program.bin 1000
trace csp
loadfile $TMP/badhandle.bin 0E38
set pxr1 80
set xr1 0000
set xr2 0D48
set iar 1003
step 1
show atr
show ptt
quit
EOF

bad=$("$SIM36" -c "$TMP/bad.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$bad"

check "nucratr refuses a foreign file    " \
  'nucratr: request block 0E00 does not own ATR file FFFFFF' "$bad"
check "...and builds no registers        " \
  'so no registers are built' "$bad"
check "the live file is left protected   " \
  'ATR  0: FFFF FFFF FFFF FFFF FFFF FFFF FFFF FFFF' "$bad"
# The real file is untouched by the refusal - its owner word still names 0E00.
check "the real file keeps its owner     " \
  'file 0000  owner 0E00' "$bad"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
