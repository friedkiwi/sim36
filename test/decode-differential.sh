#!/bin/sh
# Milestone 3 gate: the decode differential.  Five complete IPL members are
# loaded from the user-supplied volume at their link address and
# disassembled through both emulators; every instruction line must match.
# Needs SIM36_VOLUME and SIM36_REFERENCE; skips otherwise.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${SIM36_VOLUME:-}" ] || [ ! -f "${SIM36_VOLUME:-}" ]; then
    echo "decode-differential: no volume (set SIM36_VOLUME); skipped"
    exit 0
fi
if [ -z "${SIM36_REFERENCE:-}" ]; then
    echo "decode-differential: no reference emulator (set SIM36_REFERENCE); skipped"
    exit 0
fi
sim36=${SIM36:-$here/build/linux/sim36}
[ -x "$sim36" ] || sim36=$here/build/linux-make/sim36
reference_dir=$(dirname "$(printf '%s\n' "$SIM36_REFERENCE" | awk '{print $NF}')")
work=$(mktemp -d "${TMPDIR:-/tmp}/sim36-decode.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
volume=$(cd "$(dirname "$SIM36_VOLUME")" && pwd)/$(basename "$SIM36_VOLUME")
base="set terminal multiplex off
attach disk0 \"$volume\" overlay
ipl pause"
total=0; differ=0; members=0; status=0
for name in '#MSIPL' '#MSTWA' '#MSCPR' '#SVTUB' '#MSNIP'; do
    # How many instructions cover the member: disassemble generously, keep
    # the lines inside the member's own extent.
    printf '%s\nload #LIBRARY %s\ndis 1000 6000\nquit\n' "$base" "$name" >"$work/probe.sim"
    "$sim36" -c "$here/test/diffrun-base.sim" -s "$work/probe.sim" >"$work/probe.out" 2>&1
    sectors=$(sed -n 's/^loaded .* (\([0-9]*\) sectors) at 1000; IAR set$/\1/p' "$work/probe.out")
    if [ -z "$sectors" ]; then
        echo "  $name: not loaded:"; grep -v '^sim36> ' "$work/probe.out" | tail -3
        status=1; continue
    fi
    end=$((4096 + sectors * 256))
    # An instruction counts only if it lies wholly inside the member: the
    # bytes past its end are whatever else each emulator has in storage.
    n=$(grep -E '^[0-9a-f]{4}  ' "$work/probe.out" | awk -v end="$end" '
        { addr = strtonum("0x" $1); len = length($2) / 2; if (addr + len <= end) c++ } END { print c + 0 }')
    printf '%s\nload #LIBRARY %s\ndis 1000 %d\nquit\n' "$base" "$name" "$n" >"$work/member.sim"
    (cd "$reference_dir" && $SIM36_REFERENCE -c "$here/test/diffrun-base.sim" -s "$work/member.sim" 2>&1) \
        | grep -E '^[0-9a-f]{4}  ' >"$work/ref.txt"
    "$sim36" -c "$here/test/diffrun-base.sim" -s "$work/member.sim" 2>&1 \
        | grep -E '^[0-9a-f]{4}  ' >"$work/sim.txt"
    d=$(diff "$work/ref.txt" "$work/sim.txt" | grep -c '^>')
    r=$(wc -l <"$work/ref.txt")
    total=$((total + n)); differ=$((differ + d)); members=$((members + 1))
    printf '  %-8s %5d instructions  %d differ\n' "$name" "$n" "$d"
    [ "$r" -eq "$n" ] || { echo "  $name: reference printed $r lines"; status=1; }
done
echo "$members members, $total instructions, $differ differences"
[ "$differ" -eq 0 ] && [ "$members" -eq 5 ] && [ $status -eq 0 ]
