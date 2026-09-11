#!/bin/sh
# Focused SVC 50 transient 0A operation 20 test.
#
# The requests are the exact #CPTC family proven in live traces: control 82,
# key 01, and the observed +4..+7 intervals 000001E8 and 000000F4.
# It must register/re-register by task/key, immediately post condition 08, and
# must retain a native expiry deadline, but must not put an opaque callback in
# the old discrete scheduler or manufacture a workstation event.
# docs/s36/svc50-transient0a-nutix-contract-2026-09-06.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh


python3 - "$TMP/trb-1e8.bin" "$TMP/trb-f4.bin" "$TMP/code.bin" <<'PY'
import sys
open(sys.argv[1], 'wb').write(bytes.fromhex(
    '82 01 00 00 00 00 01 E8 00 00 00 00 00 00'))
open(sys.argv[2], 'wb').write(bytes.fromhex(
    '82 01 00 00 00 00 00 F4 00 00 00 00 00 00'))
# Execute against both blocks: ownership is task/key, so the second replaces
# the first despite using a different TRB address.
open(sys.argv[3], 'wb').write(bytes.fromhex(
    'F400500A2000 F400500A2000'))
PY

cat > "$TMP/run.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
ipl pause
loadfile $TMP/trb-1e8.bin 0C00
loadfile $TMP/trb-f4.bin 0D00
loadfile $TMP/code.bin 1000
# Put the issuing IPL task into the event wait that nupotcb(TB,8) should satisfy.
poke 0F04 80
poke 0F05 08
trace csp
set pxr2 00
set xr2 0C00
set iar 1000
step 1
set xr2 0D00
step 1
dump 0F00 8
dump 0C00 14
dump 0D00 14
tasklist
sched
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


check "op20 registers exact CPTC shape       " \
  'operation 20 registered CPTC timer TRB 000C00, TB 000F00, key 01, interval 488 unit(s) (8.192 ms), new registration'
check "op20 replaces same task/key           " \
  'operation 20 registered CPTC timer TRB 000D00, TB 000F00, key 01, interval 244 unit(s) (8.192 ms), replaced prior registration'
check "0x82 takes immediate post             " \
  'immediate nupotcb(TB 000F00, 08)'
check "post clears wait and readies task      " \
  'nupotb readies task block 0F00 - tb+4 80 -> 00'
check "no synthetic expiry callback           " 'now = 0, 0 pending'
check "first live interval remains guest-owned" '000c00  82 01 00 00 00 00 01 e8'
check "second interval remains guest-owned    " '000d00  82 01 00 00 00 00 00 f4'

line=$(echo "$out" | grep -E '^000f00  ' | tail -1)
echo "$line" | grep -q '000f00  e3 c2 00 09 00 00 00 fc' || {
  echo "  task is not ready after immediate timer post FAIL"; fail=$((fail + 1));
}

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
