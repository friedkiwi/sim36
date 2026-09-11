#!/bin/sh
# Differential run: the same command file through the reference emulator and
# through sim36, with timestamps and temporary paths normalised, then diffed.
#
#   tools/diffrun.sh [-q] <file.sim> [more.sim ...]
#
# Environment:
#   SIM36_REFERENCE the command that runs the reference emulator, e.g.
#                   "mono /path/to/reference.exe" (required); it is run from
#                   its own directory, so relative paths inside its startup
#                   file resolve there
#   SIM36           path to the sim36 binary (default: build/linux/sim36,
#                   then build/linux-make/sim36)
#   DIFFRUN_BASE    startup file given to both with -c (default:
#                   test/diffrun-base.sim, which turns the multiplexer off so
#                   that a diff run binds no port)
#
# Exit status is 0 when every file produced identical normalised output.
set -u

here=$(cd "$(dirname "$0")/.." && pwd)
quiet=0
if [ "${1:-}" = "-q" ]; then quiet=1; shift; fi
[ $# -ge 1 ] || { echo "usage: tools/diffrun.sh [-q] <file.sim> ..." >&2; exit 2; }

: "${SIM36_REFERENCE:?set SIM36_REFERENCE to the command that runs the reference emulator}"
reference_dir=$(dirname "$(printf '%s\n' "$SIM36_REFERENCE" | awk '{print $NF}')")
if [ -z "${SIM36:-}" ]; then
    for candidate in "$here/build/linux/sim36" "$here/build/linux-make/sim36" "$here/build/gcc10/sim36"; do
        if [ -x "$candidate" ]; then SIM36=$candidate; break; fi
    done
fi
: "${SIM36:?no sim36 binary found; set SIM36}"
base=${DIFFRUN_BASE:-$here/test/diffrun-base.sim}

normalise() {
    # Prompts, product names, absolute temporary paths and clock values.
    sed -e 's/^sim> /sim36> /' \
        -e 's/^[a-z0-9]* - System\/36 reference emulator$/SIM\/36 - System\/36 emulator/' \
        -e 's#/tmp/[A-Za-z0-9._/-]*#/TMP#g' \
        -e 's#\(\.tmp\|\.s36\)\.[0-9a-f]\{32\}#\1.RANDOM#g' \
        -e 's/[0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}[T ][0-9]\{2\}:[0-9]\{2\}:[0-9]\{2\}[.0-9]*Z\{0,1\}/TIMESTAMP/g'
}

work=$(mktemp -d "${TMPDIR:-/tmp}/diffrun.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
status=0
for file in "$@"; do
    abs=$(cd "$(dirname "$file")" && pwd)/$(basename "$file")
    (cd "$reference_dir" && $SIM36_REFERENCE -c "$base" -s "$abs" 2>&1; echo "exit=$?") | normalise >"$work/ref.txt"
    (cd "$here" && "$SIM36" -c "$base" -s "$abs" 2>&1; echo "exit=$?") | normalise >"$work/sim.txt"
    if diff -u "$work/ref.txt" "$work/sim.txt" >"$work/diff.txt"; then
        [ $quiet -eq 1 ] || echo "identical  $file"
    else
        echo "DIFFERS    $file"
        cat "$work/diff.txt"
        status=1
    fi
done
exit $status
