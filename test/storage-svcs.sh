#!/bin/sh
# SVC 2C, 2D, 2F and 51, exercised against values derived by hand from
# SA21-9436 and from the reference's decoded routines - a handler that
# compiles proves nothing.
#
# The state is real guest storage (test/build-storage-vectors.py) and every
# call is issued by the MSP as an instruction, through the dispatcher SSP
# uses.  The first vector is the manual's own worked example for MAP, 3-125,
# byte for byte.
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh
python3 test/build-storage-vectors.py "$TMP/vectors.bin" "$TMP/program.bin"

# The volume is pinned READ-ONLY: these vectors only read, and one of them
# asserts that a put to a read-only volume is refused.
cat > "$TMP/run.sim" <<EOS
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
ipl pause
loadfile $TMP/vectors.bin 0C00
loadfile $TMP/program.bin 1000
trace csp
# --- SVC 2F through a direct PACT above 1 MB -------------------------------
# Every untranslated PACT bit below 0x80 is an address bit.  Give the list at
# 10:0D40 a distinct action from the low-storage list and prove it is selected.
poke 100D40 61
set pxr1 80
set xr1 0000
set pxr2 10
set xr2 0D40
set iar 1000
step 1
show cpu
poke 100D40 00
# --- SVC 2F, SA21-9436 3-125's own example --------------------------------
set pxr1 80
set xr1 0000
set pxr2 00
set xr2 0D40
set iar 1000
step 1
show cpu
# --- SVC 2F action 9: map this program at region page 6 --------------------
set xr2 0D48
set iar 1003
step 1
dump 0E40 8
show atr
dump 0D50 3
# --- SVC 2F action 5: inherit the caller's addressability ------------------
set xr2 0D58
set iar 1006
step 1
dump 0E48 8
dump 0E50 8
dump 0D60 3
dump 0D6B 3
# --- SVC 2C and 2D over the work space at 0D00 -----------------------------
set pxr1 00
set xr1 0D00
set wr6 0100
set iar 1009
step 1
show cpu
set pxr1 00
set xr1 0D00
set wr6 0100
set iar 100C
step 1
show cpu
set pxr1 80
set xr1 0000
set xr2 0D00
set wr6 0100
set iar 100F
step 1
set pxr1 00
set xr1 0D00
set wr6 0040
set iar 1012
step 1
show cpu
# --- SVC 51 ----------------------------------------------------------------
# One-based sector 27 is the unit definition table at zero-based 26.
set pxr1 00
set xr1 001B
set pxr2 00
set xr2 2000
set iar 1015
step 1
dump 2000 10
sector 26
set iar 101C
step 1
set xr1 0DF2
set xr2 2200
set iar 1023
step 1
dump 2200 10
set xr1 001B
set iar 102A
step 1
set xr1 001B
set iar 1031
step 1
# Reusing a control-block address or changing its live extent must not retain
# an allocator sized for the former block.  The existing four-page heap has
# displacement zero partially occupied; shrink the live SB to one page and
# verify SVC 2C starts a fresh, bounded allocator at zero.
poke 0D10 00 01
set pxr1 00
set xr1 0D00
set wr6 0040
set iar 1038
step 1
quit
EOS

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"

check "2F  manual 3-125: XR1 = 805000        " 'XR1 5000'
check "2F  manual 3-125: XR1 prefix is 80    " 'PACT iar 00  dir 00  xr1 80'
check "2F  direct PACT prefix reaches above 1 MB" 'register selector 0 is outside 1-10'
check "2F  action 4 searches tb+43          " 'no control block of type 81 is on the chain at 0F2B'
check "2F  action 9 map entry                " '000e40  06 02 00 00 00 00 0c 00'
check "2F  action 9 takes map reference      " 'SVC 2F action 9: activePP - control block 000C00 use count +27 = 1'
check "2F  action 9 answers 803000           " '000d50  80 30 00'
check "2F  action 9 reaches the ATRs         " 'ATR  0: 0000 0001 0002 0003 FFFF FFFF 0002 0003'
check "2F  action 5 maps caller's module     " '000e48  03 02 00 00 00 00 0c 40'
check "2F  action 5 maps caller's map entry  " '000e50  05 01 00 03 00 00 0d 00'
check "2F  inherited PB takes map reference  " 'SVC 2F inherited map: activePP - control block 000C40 use count +27 = 1'
check "2F  inherited SB takes map reference  " 'SVC 2F inherited map: activePP - control block 000D00 use count +27 = 1'
check "2F  action 5 uses SB+16 size          " 'map entry 2 at 0E50 - pages 5..5 of the region are block 000D00'
check "2F  action 5 answers 801800           " '000d60  80 18 00'
check "2F  action 5 answers 802800           " '000d6b  80 28 00'
check "2F  a storage block stays protected   " '0 mapped/high-water page(s)) - its pages stay protected'
check "2C  first assign is 800000            " 'assigned 256 bytes at displacement 0000 of the work space at 000D00 -> XR1 = 800000'
check "2C  growth immediately rebuilds ATRs " 'rebuilt task 000F00 ATRs after high-water growth (NuXlateHeap::getHeap -> nucratr c18cb218..c18cb23c)'
check "2C  second assign is 800100           " 'assigned 256 bytes at displacement 0100 of the work space at 000D00 -> XR1 = 800100'
check "2C  success is PSR Equal              " 'IAR 100C  ARR 0000  XR1 0000  XR2 0D58  PSR 01'
check "2D  free returns the space            " 'freed 256 bytes at displacement 0000'
check "2C  the freed space is reused         " 'assigned 64 bytes at displacement 0000'
check "51  get reads two sectors             " 'get 2 sector(s) at 26 (address 00001B + key 00) to guest 002000'
check "51  and they are sector 26            " '002000  01 ff ff ff ff 00 01 00 01 26 0f 89 10 80 10 00'
check "51  indirect XR1                      " 'get 1 sector(s) at 26 (address 00001B + key 00) to guest 002200'
check "51  relative is refused               " 'RELATIVE task work area address'
check "51  JCBWSWA without a JCB             " 'nuerr code 106'
check "51  put to a read-only volume         " 'put REFUSED'
check "2C  changed SB extent resets allocator" 'work space at storage block 000D00 changed size from 8192 to 2048 bytes; discarded stale allocator state'
check "2C  reset allocator remains in bounds " 'assigned 64 bytes at displacement 0000 of the work space at 000D00 -> XR1 = 800000'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
