#!/bin/sh
# Focused guest-instruction test for transient 0A, operation 40,
# representation 8. The result is time-dependent, so the assertions validate
# zoned-decimal shape and calendar ranges rather than racing the wall clock.
# docs/s36/hfpu-post-cpon-timer-route-2026-09-06.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh


# TRB at 0C00: representation byte 08, byte +1 and result area seeded with A5
# so the test proves the exact write surface (+2..+13) and preservation of +0/+1.
python3 - "$TMP/trb.bin" "$TMP/code.bin" <<'PY'
import sys
open(sys.argv[1], 'wb').write(bytes([0x08, 0xA5] + [0xA5] * 12 + [0x5A, 0x5A]))
# SVC q=00, 50, transient 0A, inline operation 40 00
open(sys.argv[2], 'wb').write(bytes.fromhex('F400500A4000'))
PY

cat > "$TMP/run.sim" <<EOF
do $ROOT/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
ipl pause
loadfile $TMP/trb.bin 0C00
loadfile $TMP/code.bin 1000
trace csp
set pxr2 00
set xr2 0C00
set iar 1000
step 1
show cpu
dump 0C00 16
residency transient 0A
sched
tasklist
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


check "0A/40 body is recognized              " 'transient 0A (nutix): XR2 TRB 000C00, operation 40, representation 8'
check "call returns past six-byte SVC         " 'IAR 1006'
check "transient area is synchronously idle   " 'emulator body implemented; transient area busy no; queue depth 0'
check "trace states no scheduling side effect " 'no task post, wait, or scheduling side effect'
check "host scheduler receives no event        " 'now = 0, 0 pending'
check "same IPL task remains current            " '* 0F00  0009'

line=$(echo "$out" | grep -E '^000c00  ' | tail -1)
python3 - "$line" <<'PY'
import re, sys
line = sys.argv[1]
m = re.match(r'^000c00\s+((?:[0-9a-f]{2}\s+){15}[0-9a-f]{2})(?:\s|$)', line)
if not m:
    raise SystemExit('timer result dump missing or malformed: ' + repr(line))
b = [int(x, 16) for x in m.group(1).split()]
assert b[0:2] == [0x08, 0xA5], b
assert b[14:16] == [0x5A, 0x5A], b
assert all(0xF0 <= x <= 0xF9 for x in b[2:14]), b
pair = lambda at: (b[at] & 15) * 10 + (b[at + 1] & 15)
hour, minute, second = pair(2), pair(4), pair(6)
month, day, year = pair(8), pair(10), pair(12)
assert 0 <= hour <= 23, hour
assert 0 <= minute <= 59, minute
assert 0 <= second <= 59, second
assert 1 <= month <= 12, month
assert 1 <= day <= 31, day
assert 0 <= year <= 99, year
print('  zoned HHMMSS and MMDDYY ranges          PASS')
print('  exact write surface +2..+13             PASS')
PY
pass=$((pass + 2))

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
