#!/bin/sh
# SVC 33 TWAL, SVC 34 DTWAL, SVC 35 WRK - and SVC 51's relative form, which is
# the refusal this family exists to lift.
#
# Every value asserted here is derived by hand: from SA21-9436 3-128..3-131
# (read from the page images, because the text layer truncates all four entries),
# and from NuEmul::nutwal, nudtwal, nucwrk, nucwnew and the NuTwaHeap routines.
# The relative read also verifies NuTwaClearAction: after the extent is returned,
# its first sector must be zero rather than the distribution image's old content.
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

python3 test/build-work-area-vectors.py "$TMP/anchor.bin" "$TMP/vectors.bin" "$TMP/program.bin"

cat > "$TMP/run.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
# NuTwaClearAction is part of allocation/deallocation, so this fixture needs a
# writable device view. Overlay keeps the distribution image untouched.
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/anchor.bin 0BB9
loadfile $TMP/vectors.bin 0C00
loadfile $TMP/program.bin 1000
trace csp
# --- SVC 33: 16 sectors out of the 64-sector extent of base 01 -------------
# allocateHeap carves from the END of the element (c18b8a34), so the answer is
# displacement 48 and the element shrinks to 48 sectors in place.
set wr6 0010
set iar 1000
step 1
show cpu
dump 0C20 16
# --- SVC 33 with WR6 = 0: nutwal's own first check --------------------------
set wr6 0000
set iar 1003
step 1
# --- SVC 33 for more than is left: High, not a refusal ----------------------
set wr6 03E8
set iar 1006
step 1
show cpu
# --- SVC 34: give the 16 back, and watch them coalesce ----------------------
set pxr2 01
set xr2 0030
set wr6 0010
set iar 1009
step 1
dump 0C20 16
# --- SVC 33 for the whole extent: an exact fit dequeues the element ---------
set wr6 0040
set iar 100C
step 1
show cpu
dump 0C07 3
# --- SVC 34 with the chain empty: a new element appears ---------------------
set pxr2 01
set xr2 0000
set wr6 0040
set iar 100F
step 1
# --- SVC 51, RELATIVE: base 01 offset 0 -> one-based 27 -> zero-based 26 ----
set pxr1 01
set xr1 0000
set pxr2 00
set xr2 2000
set iar 1012
step 1
dump 2000 10
sector 26
# --- SVC 51 relative, key FF: diskAddr refuses it before the lookup ---------
set pxr1 FF
set xr1 0000
set iar 1018
step 1
# --- SVC 51 relative, a key no QH block carries -----------------------------
set pxr1 07
set xr1 0000
set iar 101E
step 1
# --- SVC 51 relative, an offset past the extent -----------------------------
set pxr1 01
set xr1 0040
set iar 1024
step 1
# --- SVC 35, SA21-9436 3-131's own parameter list ---------------------------
set pxr1 00
set xr1 0C40
set iar 102A
step 1
show cpu
# --- SVC 35, conditional creation with TUPH's translated-zero anchor --------
set pxr1 00
set xr1 0C50
set iar 102D
step 1
# --- SVC 35, a command that is not 1, 2 or 3 --------------------------------
set pxr1 00
set xr1 0C60
set iar 1030
step 1
# --- SVC 33 with Q bit 7, when nothing is free ------------------------------
# Re-derived premise: the SVC 35 rework changed how much of the sandbox extent
# the earlier steps consume, so 0x40 sectors ARE free here now. The claim under
# test is the Q-bit-7 wait refusal, so ask for more than the extent ever held.
set wr6 F000
set iar 1033
step 1
show cpu
# --- host-side translated-assign metadata dies with nucdelsb ---------------
set pxr1 00
set xr1 0C70
set iar 1036
step 1
set wr6 1000
set iar 1039
step 1
set pxr1 00
set xr1 0C80
set iar 103C
step 1
set pxr1 00
set xr1 0C70
set iar 103F
step 1
set wr6 1000
set iar 1042
step 1
show cpu
# --- SVC 35 flag 01: the complete new workspace is initially mapped --------
set pxr1 00
set xr1 0C90
set iar 1045
step 1
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


# SVC 33. The manual: "Index register 2: Relative disk address. This is a one
# byte base identifier and a two byte sector displacement address"; nutwal puts
# the base identifier in rb+11, which IS XR2's PACT prefix.
check "33  allocates from the end          " 'SVC 33: allocated 16 sector(s) -> XR2 = 010030, base identifier 01 displacement 0030 (1-based sector 75)'
check "33  the answer is in XR2, Equal     " 'IAR 1003  ARR 0000  XR1 0000  XR2 0030  PSR 01'
check "33  the base identifier is XR2 prefix" 'PACT iar 00  dir 00  xr1 00  xr2 01'
check "33  a failure leaves XR2 alone, High" 'IAR 1009  ARR 0000  XR1 0000  XR2 0030  PSR 04'
check "33  the free element shrank to 48   " '000c20  00 00 00 00 00 01 00 00 00 30 00 00 00 00 00 00'
check "33  WR6 = 0 is nuersvc code 109     " 'SVC 33: WR6 is zero, and nutwal calls nuersvc with code 109'
check "33  not available is High           " 'SVC 33: 1000 sector(s) NOT allocated - High.'
check "33  ...and says why it is empty     " 'no free element of that many sectors on any allocatable QH block'

# SVC 34. No PSR output - the manual prints none and nudtwal writes none - so
# the observable is the chain.
check "34  frees and coalesces backwards   " 'SVC 34: freed 16 sector(s) at relative 010030'
check "34  the element is 64 sectors again " '000c20  00 00 00 00 00 01 00 00 00 40 00 00 00 00 00 00'
check "34  onto an empty chain             " 'SVC 34: freed 64 sector(s) at relative 010000'

# An exact fit dequeues the element rather than leaving a zero-length one:
# allocateHeap c18b8a10.
check "33  an exact fit takes it all       " 'SVC 33: allocated 64 sector(s) -> XR2 = 010000'
check "33  ...and unchains the element     " '000c07  00 00 00'

# SVC 51's relative form - the refusal this whole family lifts.
check "51  relative resolves               " 'SVC 51: relative 010000 - base identifier 01, offset 0000 - resolves to 1-based sector 27'
check "51  and reads the right sector      " 'get 1 sector(s) at 26 (address 00001B + key 00) to guest 002000'
# Returning the extent runs NuTwaClearAction before it is made available again.
check "51  returned sectors were cleared   " '002000  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00'
check "51  key FF is nuerabt code 108      " 'key FF is not an extent'
check "51  an unknown key says so          " 'no QH block on queue header 46 carries base identifier 07'
check "51  an offset past the extent       " 'offset 64 is past the 64-sector extent'

# SVC 35. The parameter list is SA21-9436 3-131's own, and the manual's sentence
# about it is the assertion: "a 2 page (4096 byte) task work space for the task
# with a task ID of hex 0002".
check "35  the manual's list decodes       " 'command 2 (unconditional create), type 81 (task work space), size 4096 byte(s) = 2 page(s), flags 00, task id 0002'
check "35  creates the work space          " 'SVC 35: created a 2-page task work space of type 81'
# Re-derived: the create now goes through the one nucbldsb, whose nutwl trace is
# the allocation evidence; the old text pinned the duplicate builder's wording.
check "35  ...backed by a TWA allocation   " 'swap area of 18 sector(s) at task work area relative sector 0 (nutwl, c18bbfb8)'
check "35  the storage block comes back in XR1" 'IAR 102D  ARR 0000  XR1 FF70  XR2 2000  PSR 01'
# Re-derived: conditional create is IMPLEMENTED (the search is SVC 2F action 4's
# own settled walk). In this sandbox task id 0002 does not exist, so the create
# is queued nowhere and says so - the honest sandbox answer, and the trace to pin.
check "35  conditional create implemented " 'command 1 (conditional create), type 81'
check "35  translated-zero anchor searches " 'raw anchor 800000 is translated zero'
check "35  a missing task id is said aloud " 'names no task block on queue header 39'
check "35  an invalid command is 998       " 'is not 1, 2 or 3 - nucwrk calls nuersvc with code 998'
check "35  deletes exhausted workspace     " 'deleted the task work space of type 82'
check "2C  rebuilt workspace is empty again" 'IAR 1045  ARR 0000  XR1 0000  XR2 2000  PSR 01'
check "35  flag 01 maps the initial page    " 'type 83, flags 42, +16 = 1, +18 = +42 = 1'

# Q bit 7's wait is decoded but refused: nutwal's wait arm (c18927a4) is nugwaitc
# with general-wait mask 0x4000 (getHeap's 0x00FF4000 failure code AND 0xFFFF,
# c189279c), and the ONLY poster is extendHeap's SSP task (NuEmul[0xFF8] gate),
# which this machine does not model - so the wait can never end. High, the
# manual's own "not available", is answered instead of deadlocking. The trace
# names the mask and the extendHeap dependency, so the refusal is precise.
check "33  Q bit 7's wait is named, not faked" 'Q bit 7 set takes nutwal'"'"'s unconditional wait arm (c18927a4) - nugwaitc with general-wait mask 4000'
check "33  ...and cites the extendHeap poster" 'the only poster is extendHeap'"'"'s SSP task'

# ---------------------------------------------------------------------------
# A second run with NOTHING pre-loaded, so the emulator builds queue header 46
# itself - csipl c18326ec..c18327dc, transcribed. This is the machine's state at
# IPL, and the two things it asserts are the two that matter: base FE resolves,
# and SVC 33 cannot allocate, because both of csipl's headers are past the bound
# getHeap applies at c18b8700.
# ---------------------------------------------------------------------------
cat > "$TMP/ipl.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME overlay
ipl pause
loadfile $TMP/program.bin 1000
trace csp
set pxr1 FE
set xr1 0000
set pxr2 00
set xr2 2000
set iar 1012
step 1
dump 0BB9 3
set wr6 0010
set iar 1000
step 1
show cpu
quit
EOF

ipl=$("$SIM36" -c "$TMP/ipl.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$ipl"

iplcheck() {
  if echo "$ipl" | grep -qF "$2"; then
    echo "  $1 PASS"; pass=$((pass + 1))
  else
    echo "  $1 FAIL  (looked for: $2)"; fail=$((fail + 1))
  fi
}

iplcheck "csipl's own two QH blocks are built" 'task work area: queue header 46 -> QH FFF0 base FE sector 7167 for 1014 sector(s), all free -> QH FF70 base FF sector 8191'
iplcheck "queue header 46 points at them   " '000bb9  00 ff f0'
iplcheck "base FE resolves to sector 7167  " 'resolves to 1-based sector 7167'
iplcheck "and the read lands on 7166       " 'get 1 sector(s) at 7166 (address 001BFF + key 00) to guest 002000'
iplcheck "at IPL nothing is allocatable    " 'SVC 33: 16 sector(s) NOT allocated - High.'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
