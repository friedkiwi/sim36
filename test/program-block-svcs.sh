#!/bin/sh
# SVC 31 ATASK, SVC 32 DTASK, SVC 2E Time of Day, and SVC 2F MAP action 4 -
# every call issued by the MSP as an instruction, through the dispatcher SSP
# uses, against guest storage built by test/build-program-block-vectors.py.
#
# What is asserted is behaviour the model does not decide for itself: the block
# ATASK returns has the fields the manual's WR6 named, the storage DTASK frees
# is handed out again by the next ATASK, the swap area comes back to the task
# work area, and MAP action 4 reproduces SA21-9436 3-125's own printed answer.
#
# docs/s36/svc-program-block-lifecycle.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

python3 test/build-program-block-vectors.py "$TMP/vectors.bin" "$TMP/program.bin"

cat > "$TMP/run.sim" <<EOF
do $ROOT/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
ipl pause
loadfile $TMP/vectors.bin 0C00
loadfile $TMP/program.bin 1000
trace csp
# --- SVC 31: WR6 = 0402, a 4-page region and 2 pages of main storage --------
set wr6 0402
set iar 1000
step 1
show cpu
dump FF70 40
# --- SVC 31 again, Q bit 3: the maximum swap area --------------------------
set wr6 0801
set iar 1003
step 1
show cpu
# --- SVC 32 on the second block: dequeue, free, give the swap area back -----
set pxr1 00
set xr1 FFB0
set iar 1006
step 1
dump FFB0 4
# --- SVC 31 a third time: the freed sixty-four bytes come back --------------
set wr6 0402
set iar 1000
step 1
show cpu
# --- SVC 2E ----------------------------------------------------------------
set iar 1009
step 1
show cpu
# --- chain the task work space on the task block's own +43 chain -----------
set pxr1 00
set xr1 0D00
set pxr2 00
set xr2 0F2B
set iar 100C
step 1
dump 0F29 3
# --- SVC 2F action 4, with SA21-9436 3-125's list and XR1 = 800000 ---------
set pxr1 80
set xr1 0000
set pxr2 00
set xr2 0D40
set iar 1013
step 1
show cpu
dump 0E40 8
# --- SVC 32 on a block with no references: nucdactv's underflow ------------
set pxr1 00
set xr1 0C00
set iar 1006
step 1
dump 0C1B 1
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


# SA21-9436 3-126: "Index register 1: Program block address", and "the PSR is
# returned set to equal".  The 64 bytes come from the system queue space.
check "31  XR1 is the program block         " 'XR1 FF70'
check "31  PSR Equal                        " 'IAR 1003  ARR 0000  XR1 FF70  XR2 0000  PSR 01'
# The block itself: "PB", type 1, flags 40, +16 the region size and +18 = +42 the
# main storage size, +22 FFFF, +24..26 the swap area reference + 1, +27 one
# reference, +37..39 the IPL task block at 000F00.
check "31  eyecatcher, type and flags       " '00ff70  d7 c2 00 00 01 00 00 00 40 00 00 00 00 00 00 00'
# +16 = 0004 the region, +18 = 0002 the main storage, +20 = 0002 (activePP's
# square-up), +22 = ffff, +24..26 = 000001 the swap area reference + 1, +27 = 01
# one reference.
check "31  +16, +18, +20, +22, +24, +27     " '00ff80  00 04 00 02 00 02 ff ff 00 00 01 01 00 00 00 00'
# +37..39 = 000f00, the IPL task block; +42..43 = 0002, the main storage size again.
check "31  +37..39 tb, +42 the size again   " '00ff90  00 00 00 00 00 00 0f 00 00 00 00 02 00 00 00 00'
# (region 4 pages x 8 sectors) + 2 = 34 sectors, nucbldsb c18bbfa0.
check "31  swap area is region x 8 + 2      " 'swap area of 34 sector(s) at task work area relative sector 0'
check "31  backing covers resident pages    " 'has 2 initially resident page(s) at real'
check "31  virtual region stays distinct    " 'its 4-page virtual region can acquire more backing through SVC 12'
# Q bit 3 replaces that with nucbldsb's flat 256 + 2, c18bbfa8, whatever WR6 says.
check "31  Q bit 3 is the maximum swap area " 'swap area of 258 sector(s) at task work area relative sector 34'
check "31  Q bit 3 reaches the flags byte   " 'nucbldsb flags 50 (Q bit 3, maximum swap area)'

# SA21-9436 3-127: "dequeues and frees the program block and deallocates any
# associated swap area".  The 258 sectors go back, and so do the 64 bytes.
# The reference's own copy of this check expects "cleared and deallocated";
# the reference emits "not cleared" here because this synthetic swap area's
# base identifier 00 has no QH block on queue header 46 to resolve through,
# and the reference test fails that check itself.  The behaviour, not the
# stale expectation, is the parity target recorded by this test.
check "32  swap area is deallocated (not clearable: no QH for base 00)" 'swap area of 258 sector(s) at relative sector 34 not cleared and deallocated'
check "32  the block is freed               " 'control block 00FFB0 dequeued and freed (64 bytes)'
check "32  an ATASK block is on no queue    " 'is on no queue - nucwsbsq returns zero'
check "32  the eyecatcher is gone           " '00ffb0  00 00 00 00'
# ...and the storage really is free: the next ATASK is handed it again, and the
# swap area allocator gives back the hole the 258 sectors left.
check "32  the freed 64 bytes are reused    " 'XR1 FFB0'
check "32  the freed swap sectors reused    " 'swap area of 34 sector(s) at task work area relative sector 34'

# SVC 2E: the unit is sourced (SC21-7908-3, LY21-0590-04) and the two halves go
# to XR1 and XR2 - nutitod c197deb4/c197debc.
check "2E  the unit is 8.192 ms and cited   " 'timer unit(s) of 8.192 ms (SC21-7908-3, LY21-0590-04)'
check "2E  the clock is named as policy     " "the CLOCK is the host's local time, which is emulator policy"

# SVC 0E put the work space on tb+43, which is where nucwsbsq would have queued
# it and where nucm1000's action 4 looks for it.
check "2F  the work space is on tb+43       " '000f29  00 0d 00'
# SA21-9436 3-125, printed: "At the end of the SVC, XR1 contains hex 805000 and
# the program is able to address data from hex 5000 through hex 57FF."  This is
# the example the corpus could only refuse until action 4 was decoded.
check "2F  action 4 finds the work space    " 'action 4 found type 81 id 0000 at 000D00'
check "2F  manual 3-125: XR1 = 805000       " 'XR1 5000'
check "2F  manual 3-125: XR1 prefix is 80   " 'PACT iar 00  dir 00  xr1 80'
# One page at region page 10 (hex 5000), displacement 0, of the block at 000D00.
check "2F  action 4 map entry               " '000e40  0a 01 00 00 00 00 0d 00'

# nucdactv c18bb6a4: releasing a block that has no references leaves +27 at FF
# and raises nuerr 111 unless flags bit 0x80 says not to.
check "32  underflow is reported, not hidden" 'was already at zero references - nucdactv raises nuerr code 111'
check "32  and the block is NOT deleted     " '000c1b  ff'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
