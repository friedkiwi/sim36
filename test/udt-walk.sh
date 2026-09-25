#!/bin/sh
# The control processor's unit definition table walk, asserted byte for byte.
#
# The walk reads the device records at 0-based sector 26 and derives a few
# dozen bytes of guest low storage from them.  Every value below was computed
# by hand from the record contents and the decoded arms, so a walk that runs
# and writes something else is caught here rather than three thousand
# instructions later.  The volume is pinned READ-ONLY: `ipl pause` performs
# the control storage IPL and starts the MSP without executing an
# instruction, so nothing needs to write.
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

cat > "$TMP/run.sim" <<EOS
do $ROOT/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
trace csp
ipl pause
dump 080B 1
dump 0820 3
dump 0849 2
dump 0850 2
dump 086B 2
dump 0870 10
dump 0898 6
dump 08B0 3
dump 08B6 1
dump 08BC 2
dump 08BF 8
dump 08DB 5
dump 08E0 1
dump 097F 2
dump 0A7B 9
quit
EOS

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"

check "walk: 12 records, 466 bytes           " 'UDT: 12 record(s) walked, 466 bytes'
check "system entry: 0850 = 89, 0851 = 1 disk" '000850  89 01'
check "system entry: 084A = configure[7]     " '000849  45 31'
check "system entry: 08BF..08C2 = name[5..8] " '0008bf  e2 d5 61 f1'
check "system entry: 0898..089D = name[9..14]" '000898  f0 f5 f9 f0 d4 c7'
check "system entry: 08B0 bit 40 stays clear " 'configure[8]=80 -> 08B0 bit 40 clear'
check "system entry: 08E0 hosted M36 bit      " '0008e0  80'
check "class 40: 080B = 9F                   " '00080b  9f'
check "class C0: 097F / 0980 = 10 00         " '00097f  10 00'
check "class C0: 0849 |= configure[0] = 45   " '000849  45'
check "class arms: 08B0 08B1 08B2            " '0008b0  08 30 c0'
check "LAN unit 7: 08DB..08DF                " '0008db  01 00 75 00 00'
check "LAN unit 7: 086B |= 01                " '00086b  01'
check "comm: 0870..0879, units 1 and 2       " '000870  01 4a ec 00 15 01 4a ec 00 15'
check "disk: 0851 = 1 unit                   " 'UDT: record 8 at +282, id A7'
check "disk: record+3 = 01 selects no slot   " 'record+3 = 01 -> no address slot'
check "disk: 0A7B..0A83 stay zero            " '000a7b  00 00 00 00 00 00 00 00 00'
check "tape: 08BC = 0A (08 | 02)             " '0008bc  0a 89'
check "tape: 08B6 stays zero, one unit only  " '0008b6  00'
check "diskette: 08B0 bit 04 cleared         " 'diskette (c1832008): 08B0 &= ~04'
check "ignored: MSP STORAGE (id 20)          " 'id 20 class FF, 26 bytes -> ignored'
check "ignored: CSP STORAGE (id 10)          " 'id 10 class FF, 24 bytes -> ignored'
check "ignored: 3487 console (id C2)         " 'id C2 class C0, 43 bytes -> ignored (B3..D1 have no arm)'
check "08C3 = getMaxDevices, and IS written  " 'UDT: 08C3 = 64'
check "08C3 holds 40 hex in low storage      " '0008bf  e2 d5 61 f1 40 00 00 00'
check "skipped: the comm arm's stack byte    " 'NOT OR-ed at c1831f68'
check "skipped: 0820..0822 stay zero         " '000820  00 00 00'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
