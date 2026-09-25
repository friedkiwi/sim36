#!/bin/sh
# SVC 52, Main Storage Relocating Loader.
#
# Every call here is issued by the MSP as an instruction, through the dispatcher
# SSP uses, and every assertion is on what the GUEST can see afterwards - bytes
# in guest storage, the instruction address, the sectors the volume was asked
# for.  Nothing asserts a value this emulator wrote and then read back.
#
# The first case is IBM's own worked example from SA21-9436 3-146, seventeen
# parameter-list bytes and two statements about what they mean, and it reads a
# real sector of the reference volume whose contents test/build-loader-vectors.py
# takes from the image directly.
#
# docs/s36/svc-relocating-loader.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh


# The loader WRITES nothing, but staging a module for it to read does: the two
# SVC 51 puts need a writable volume.  Take a private copy rather than touch the
# shared scratch image, so this suite cannot perturb an IPL run.
cp $SIM36_VOLUME "$TMP/vol.img"

python3 test/build-loader-vectors.py \
        "$TMP/vectors.bin" "$TMP/program.bin" "$TMP/stage.bin" \
        $SIM36_VOLUME "$TMP/expected.sh"
. "$TMP/expected.sh"

cat > "$TMP/run.sim" <<EOF
do $ROOT/test/advanced36-nolisten.sim
attach disk0 $TMP/vol.img rw
ipl pause
loadfile $TMP/vectors.bin 0C00
loadfile $TMP/program.bin 1000
loadfile $TMP/stage.bin 3800
trace csp
# --- stage the module and its relocation directory on the volume -----------
set pxr1 09
set xr1 27C1
set pxr2 00
set xr2 3800
set iar 1000
step 1
set pxr1 09
set xr1 27C2
set pxr2 00
set xr2 3900
set iar 1006
step 1
# --- SA21-9436 3-146's own example -----------------------------------------
set pxr2 00
set xr2 0C00
set iar 100C
step 1
dump 2000 10
# --- load to address, link == load, so nothing is relocated ----------------
set pxr2 00
set xr2 0C20
set iar 1010
step 1
dump 3000 10
# --- fetch to address, 0x2000 above the link address -----------------------
set pxr2 00
set xr2 0C40
set iar 1014
step 1
show cpu
dump 3000 10
# --- system fetch to address: the task block learns the factor -------------
set pxr2 00
set xr2 0C80
set iar 101C
step 1
# --- plain fetch: the load address is link + that factor -------------------
set pxr2 00
set xr2 0CA0
set iar 1020
step 1
# --- a memory resident overlay, which is refused ---------------------------
set pxr2 00
set xr2 0C60
set iar 1018
step 1
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


# IBM's example, and the manual's two statements about it.  "The sequential
# sector address of the module is 010342" is 66370 in decimal, and the list's
# load and link addresses are both 002000, which is why it says "relocation of
# the subroutine is not necessary".
check "52  3-146: sector 010342, 15 sectors " \
      '15 sector(s) from 1-based 66370 to 002000'
check "52  3-146: no relocation required    " \
      'load address 002000 equals link address 002000'
check "52  3-146: the volume's bytes arrive " "002000  $IBM_SECTOR"

# A module read back through the loader is the module that was staged.
check "52  load to address is byte-exact    " \
      '003000  c2 10 10 00 d4 d6 c4 e4 d3 00 12 34 00 00 00 00'

# The relocation directory on disk moved BOTH address fields by the difference
# between the load and link addresses, and left everything else alone.
check "52  the relocation directory ran     " 'relocated 2 address(es) by 2000'
check "52  both address fields moved        " \
      '003000  c2 10 30 00 d4 d6 c4 e4 d3 00 32 34 00 00 00 00'

# Fetch: 3-145 says it "passes control to the module's start control address",
# and that address moved with the module - 001040 + 2000 - so the instruction
# address the guest resumes at is 003040 and not the 001040 in the list.
check "52  fetch enters at the RELOCATED    " \
      "control passes to the module's start control address 003040"
check "52  ... start control address        " 'IAR 3040'

# The system types update the task block, and a later plain fetch uses what
# they left there: link 001000 plus a relocation factor of 2000 is 003000.
check "52  system request updates the TB    " \
      'relocation factor := 2000, loader disk address := 0927C1'
check "52  plain fetch uses the factor      " \
      'type 04 fetch - 1 sector(s) from 1-based 600001 to 003000'

# And the one path that is refused says why.
check "52  overlays refused, with a reason  " 'memory resident overlay'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
