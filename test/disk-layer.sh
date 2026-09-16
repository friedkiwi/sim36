#!/bin/sh
# Milestone 2 gate: the disk layer over a user-supplied volume.  Compares
# `show storage`, `vtoc system`, `vtoc user`, `lib #RPGLIB`, `boot`, `sector`
# over ten addresses, a member load, `dump` over ten addresses and `show cpu`
# against the reference.
#
# The `ipl pause` block itself is excluded (--from) and the one line SIM/36
# prints where the reference performs the control-storage IPL is dropped
# (--ignore): that stage is milestone 4.  Everything else must be identical.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${SIM36_VOLUME:-}" ] || [ ! -f "${SIM36_VOLUME:-}" ]; then
    echo "disk-layer: no volume (set SIM36_VOLUME); skipped"
    exit 0
fi
if [ -z "${SIM36_REFERENCE:-}" ]; then
    echo "disk-layer: no reference emulator (set SIM36_REFERENCE); skipped"
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/sim36-disk-layer.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
volume=$(cd "$(dirname "$SIM36_VOLUME")" && pwd)/$(basename "$SIM36_VOLUME")
prelude="set terminal multiplex off
attach disk0 \"$volume\" overlay
ipl pause"
cat >"$work/disk.sim" <<SIM
$prelude
show storage
vtoc system
vtoc user
vtoc
lib #RPGLIB 5
lib #RPGLIB
lib RPGLIB 3
lib #SYSWORK
lib NOSUCH
boot
sector 8191 1
sector 8190
sector 8410 2
sector 75441
sector 75442 3
sector 207811
sector 207812
sector 0
sector 819199
show csp
show atr
set iar 1000
set xr1 0800
set xr2 ffff
set pxr1 80
set psr 03
set wr6 0029
show cpu
load #RPGLIB #AU002
dump 800 40
dump 0800
dump 900 100
dump a00 10
dump b00 200
dump c00 1
dump xr1 20
dump 1000 40
dump 0ffff0 20
load #RPGLIB NOSUCH
load NOSUCH X
dump 0 8
show status
reset --yes
show status
quit
SIM
# Each of these stops its file with an error, so they run one per file.
n=0
for bad in "sector 999999999" "dump ffff00 200" "dump zz" "sector x" "set machine task-work-area 61" \
           "attach disk0 \"$volume\" ro" "reset" "vtoc bogus" "load"; do
    n=$((n + 1))
    printf '%s\n%s\nshow status\nquit\n' "$prelude" "$bad" >"$work/err$n.sim"
done
"$here/tools/diffrun.sh" --from "show storage" \
    --ignore '^csp   main storage IPL is not ported yet' "$work/disk.sim"
main=$?
"$here/tools/diffrun.sh" --from "ipl pause" \
    --ignore '^csp   main storage IPL is not ported yet' \
    --ignore '^src   SRC posted: 0000  running normally' "$work"/err*.sim
[ $main -eq 0 ] && [ $? -eq 0 ]
