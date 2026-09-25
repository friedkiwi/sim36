#!/bin/sh
# The native checkpoint: save a constructed machine, mutate it, load the
# checkpoint back and prove main storage, the architectural registers and the
# execution counter are restored exactly.
set -u
cd "$(dirname "$0")/.."
. test/gate-common.sh

snapshot="$TMP/machine.s36"
cat >"$TMP/snapshot.sim" <<EOF
do $ROOT/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
ipl
wait idle 30
stop
show cpu
poke F0000 5A
snapshot save $snapshot
set iar 1234
poke F0000 00
show cpu
snapshot load $snapshot
show cpu
dump F0000 1
quit
EOF

out=$("$SIM36" -c "$TMP/snapshot.sim" 2>&1)
rcheck() {   # rcheck <name> <basic-regex>
    if echo "$out" | grep -q "$2"; then
        echo "ok - $1"; pass=$((pass + 1))
    else
        echo "not ok - $1"; fail=$((fail + 1))
    fi
}

rcheck "native checkpoint is saved" "snapshot: saved .* (constructed)"
rcheck "test mutation reached the live machine" "IAR 1234"
rcheck "checkpoint reconstructs a machine" "snapshot: restored constructed machine"
rcheck "main storage was restored byte-exactly" "0f0000  5a"
restored=$(echo "$out" | grep '^IAR ' | tail -1)
if echo "$restored" | grep -q '^IAR 1C11 '; then
    echo "ok - architectural IAR was restored"; pass=$((pass + 1))
else
    echo "not ok - architectural IAR was restored ($restored)"; fail=$((fail + 1))
fi
saved_count=$(echo "$out" | sed -n 's/^stopped=.*instructions=\([0-9][0-9]*\).*/\1/p' | head -1)
restored_count=$(echo "$out" | sed -n 's/^stopped=.*instructions=\([0-9][0-9]*\).*/\1/p' | tail -1)
if [ -n "$saved_count" ] && [ "$saved_count" = "$restored_count" ] &&
   echo "$out" | grep -q "restored constructed machine at IAR 1C11 after $saved_count guest instruction"; then
    echo "ok - execution counter was restored"; pass=$((pass + 1))
else
    echo "not ok - execution counter was restored (saved=$saved_count restored=$restored_count)"; fail=$((fail + 1))
fi

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
