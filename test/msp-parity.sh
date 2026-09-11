#!/bin/sh
# Milestone 3 gate: the MSP monitor surface against the reference over
# phase 1 read from the user-supplied volume: selftest, dis, break, watch,
# poke, patch, findmem, addrmap, flow and isn traces, and the msp trace of
# phase 1 from its first instruction.
#
# The trace window is bounded by the first supervisor call phase 1 issues
# (instruction 9, SVC 0F): until the control storage processor is ported
# (milestones 4 and 5) SIM/36 refuses it where the reference services it,
# so the second file compares only up to that point.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${SIM36_VOLUME:-}" ] || [ ! -f "${SIM36_VOLUME:-}" ]; then
    echo "msp-parity: no volume (set SIM36_VOLUME); skipped"
    exit 0
fi
if [ -z "${SIM36_REFERENCE:-}" ]; then
    echo "msp-parity: no reference emulator (set SIM36_REFERENCE); skipped"
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/sim36-msp.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
volume=$(cd "$(dirname "$SIM36_VOLUME")" && pwd)/$(basename "$SIM36_VOLUME")
# Phase 1: the boot record and the fifteen sectors after it.
dd if="$volume" of="$work/phase1.bin" bs=256 skip=8191 count=16 status=none
cat >"$work/msp.sim" <<SIM
set terminal multiplex off
attach disk0 "$volume" overlay
ipl pause
selftest
loadfile $work/phase1.bin 1000
dis 1000 12
dis
set iar 1400
dis
dis 1000 3
dis 1433 2
dis 1000 0
break 1004 hello world
break 1008
break list
break clear 1008
break clear 1008
break list
break clear
break list
break
poke 2000 ab cd
poke 2002 ef
poke 2004 abc
poke
findmem abcdef
findmem
addrmap xr1 10
addrmap direct 1000 write
addrmap iar
addrmap xr2 ffff
addrmap bogus
addrmap
set pxr1 80
addrmap xr1 10
set pxr1 00
watch 2000 2
poke 2000 ff
poke 1fff 11 22 33
watch off
watch
poke 2000 ee
patch list
patch 1000 mem 2000 12
patch 1000 wr 6 1234
patch 1000 xr1 2222
patch 1000 bogus 1 2
patch 1000
patch list
break 1769 first
set iar 1000
step 5
step 1
step 2
show cpu
patch off
patch
break clear
trace flow
step 3
trace isn
step 1
trace off
break member x
break member off
trace member x
trace member off
watch 0800 1
show cpu
set iar 1000
trace msp,svc
step 8
show cpu
trace off
set iar 1000
step
step 0x7
show cpu
quit
SIM
printf 'set terminal multiplex off\nattach disk0 "%s" overlay\nipl pause\nstep zz\n' "$volume" >"$work/err1.sim"
printf 'set terminal multiplex off\nattach disk0 "%s" overlay\nipl pause\nstep 0\n' "$volume" >"$work/err2.sim"
printf 'set terminal multiplex off\nattach disk0 "%s" overlay\nipl pause\nstep 1 2\n' "$volume" >"$work/err3.sim"
printf 'set terminal multiplex off\nattach disk0 "%s" overlay\nipl pause\nwatch zz\n' "$volume" >"$work/err4.sim"
printf 'set terminal multiplex off\nattach disk0 "%s" overlay\nipl pause\nbreak zz\n' "$volume" >"$work/err5.sim"
printf 'set terminal multiplex off\nattach disk0 "%s" overlay\nipl pause\npoke 2000 zz\n' "$volume" >"$work/err6.sim"
printf 'set terminal multiplex off\nattach disk0 "%s" overlay\nipl pause\ndis zz\n' "$volume" >"$work/err7.sim"
printf 'set terminal multiplex off\nattach disk0 "%s" overlay\nipl pause\nset iar 12345\n' "$volume" >"$work/err8.sim"
printf 'set terminal multiplex off\nattach disk0 "%s" overlay\nipl pause\nloadfile /nonexistent/x.bin 1000\n' "$volume" >"$work/err9.sim"
"$here/tools/diffrun.sh" --from selftest "$work/msp.sim"
main=$?
"$here/tools/diffrun.sh" --from "ipl pause" \
    --ignore '^csp   main storage IPL is not ported yet' \
    --ignore '^src   SRC posted: 0000  running normally' "$work"/err*.sim
[ $main -eq 0 ] && [ $? -eq 0 ]
