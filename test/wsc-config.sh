#!/bin/sh
# The monitor changes the SSP work-station configuration input, not a runtime
# QH53 object.  This regression guard checks the exact W7 fields #SVTUB reads.
set -e
cd "$(dirname "$0")/.."
. test/probe-common.sh
# This suite compares exact values rather than searching a transcript.
check() {
    if [ "$2" = "$3" ]; then
        echo "  PASS  $1"; pass=$((pass + 1))
    else
        echo "  FAIL  $1 (got '$2', expected '$3')"; fail=$((fail + 1))
    fi
}

cp "$SIM36_VOLUME" "$TMP/volume.img"

cat > "$TMP/config.sim" <<EOF
set machine model advanced36
set machine ipl-type unattend
set machine ipl-source disk
attach disk0 volume.img rw
set station 0.0 role console
set station 0.0 device-code 11
ipl pause
EOF

run_command() {
    printf '%s\nquit\n' "$1" | "$SIM36" -c "$TMP/config.sim"
}


out=$(run_command 'wsconfig set W7 message-session 1234')
check "command reports the selected WSC input" \
      "$(echo "$out" | grep -o 'cfg+4=80, alternate QH53 key=1234' | tail -1)" \
      "cfg+4=80, alternate QH53 key=1234"

# W7 begins at byte 0x202BC0.  #SVTUB reads selector +4 and alternate key +11..12.
fields=$(od -An -tx1 -j $((0x202bc4)) -N 9 "$TMP/volume.img" | tr -d ' \n')
check "message-session writes cfg+4 and cfg+11..12" "$fields" "8006ffff00fc501234"

out=$(run_command 'wsconfig set W7 physical')
check "physical command reports the inverse selection" \
      "$(echo "$out" | grep -o 'cfg+4=C0, alternate QH53 key=0000' | tail -1)" \
      "cfg+4=C0, alternate QH53 key=0000"
fields=$(od -An -tx1 -j $((0x202bc4)) -N 9 "$TMP/volume.img" | tr -d ' \n')
check "physical restores selector and clears alternate key" "$fields" "c006ffff00fc500000"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
