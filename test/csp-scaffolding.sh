#!/bin/sh
# Milestone 4 gate: the control storage processor's scaffolding against the
# reference over the user-supplied volume.
#
#   1. `ipl pause`: guest low storage, the queue headers, the initial task
#      and request blocks, the system queue space and the ACE pool, with
#      the csp/ace/disk traces of the whole main storage IPL.
#   2. `diskread`: the SVC 40 path (ACE build, device dispatch, completion
#      post and release) under `trace ace` and `trace disk`, including a
#      rejected read.
#   3. The storage-SVC vectors as a full transcript (SVC 2C/2D/2F/51 and
#      the translation register rebuilds).
#   4. `conformance`: the ten CSP vectors.  The closing "N instruction(s),
#      then:" line and the SRC post are excluded: phase 1 runs until the
#      first supervisor call of an unported family (SVC 10, transfer
#      control, milestone 5), where the reference runs on.
#   5. The first 24 instructions of phase 1 under every trace class, which
#      is the window up to that call.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${SIM36_VOLUME:-}" ] || [ ! -f "${SIM36_VOLUME:-}" ]; then
    echo "csp-scaffolding: no volume (set SIM36_VOLUME); skipped"
    exit 0
fi
if [ -z "${SIM36_REFERENCE:-}" ]; then
    echo "csp-scaffolding: no reference emulator (set SIM36_REFERENCE); skipped"
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/sim36-csp.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
volume=$(cd "$(dirname "$SIM36_VOLUME")" && pwd)/$(basename "$SIM36_VOLUME")

cat >"$work/ipl-pause.sim" <<SIM
do $here/test/advanced36-nolisten.sim
attach disk0 "$volume" ro
trace csp,ace,disk
ipl pause
show cpu
show atr
dump 0800 200
dump 0B00 100
dump 0E00 40
dump 0F00 80
dump 2000 100
dump 6000 20
ace 03C0
ace queue 39
ace queue 40
sched
tu 0000
quit
SIM

cat >"$work/diskread.sim" <<SIM
do $here/test/advanced36-nolisten.sim
attach disk0 "$volume" ro
ipl pause
trace ace,disk
diskread 8191 1
iob 0600
diskread 26 2
trace csp
diskread 99999999 1
quit
SIM

python3 "$here/test/build-storage-vectors.py" "$work/vectors.bin" "$work/program.bin"
sed -n '/^cat > "\$TMP\/run.sim"/,/^EOS$/p' "$here/test/storage-svcs.sh" | sed '1d;$d' \
    | sed "s#\$PWD#$here#; s#\$SIM36_VOLUME#$volume#; s#\$TMP#$work#" >"$work/storage-svcs.sim"

cat >"$work/conformance.sim" <<SIM
do $here/test/advanced36-nolisten.sim
attach disk0 "$volume" ro
ipl pause
conformance
quit
SIM

cat >"$work/phase1.sim" <<SIM
do $here/test/advanced36-nolisten.sim
attach disk0 "$volume" ro
trace csp,svc,ace,disk,msp,sched
ipl pause
step 24
show cpu
quit
SIM

status=0
"$here/tools/diffrun.sh" "$work/ipl-pause.sim" || status=1
"$here/tools/diffrun.sh" "$work/diskread.sim" || status=1
"$here/tools/diffrun.sh" "$work/storage-svcs.sim" || status=1
"$here/tools/diffrun.sh" --ignore '^src   SRC posted: CHECK' --ignore '^  SVC [14]0' \
    --ignore '^     [0-9]* instruction(s), then:' "$work/conformance.sim" || status=1
"$here/tools/diffrun.sh" "$work/phase1.sim" || status=1
exit $status
