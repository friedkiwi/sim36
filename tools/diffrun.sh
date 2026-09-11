#!/bin/sh
# Differential run: the same command file through the reference emulator and
# through sim36, with timestamps and temporary paths normalised, then diffed.
#
#   tools/diffrun.sh [-q] [--from <command>] [--ignore <regex>]... <file.sim> ...
#
#   --from <command>  compare only from the echoed line "sim36> <command>"
#                     onward (both transcripts are cut at the same marker)
#   --ignore <regex>  drop lines matching the regex from both transcripts
#                     before diffing; every use is printed so the exclusion
#                     is visible in the gate output
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
from=''
ignores=''
while [ $# -gt 0 ]; do
    case "$1" in
        -q) quiet=1; shift ;;
        --from) from=$2; shift 2 ;;
        --ignore) ignores="$ignores
$2"; shift 2 ;;
        --) shift; break ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
[ $# -ge 1 ] || { echo "usage: tools/diffrun.sh [-q] [--from <command>] [--ignore <regex>]... <file.sim> ..." >&2; exit 2; }
if [ -n "$from" ]; then echo "diffrun: comparing from 'sim36> $from' onward"; fi
printf '%s\n' "$ignores" | while IFS= read -r rx; do
    [ -n "$rx" ] && echo "diffrun: ignoring lines matching /$rx/"
done

: "${SIM36_REFERENCE:?set SIM36_REFERENCE to the command that runs the reference emulator}"
reference_dir=$(dirname "$(printf '%s\n' "$SIM36_REFERENCE" | awk '{print $NF}')")
if [ -z "${SIM36:-}" ]; then
    for candidate in "$here/build/linux/sim36" "$here/build/linux-make/sim36" "$here/build/gcc10/sim36"; do
        if [ -x "$candidate" ]; then SIM36=$candidate; break; fi
    done
fi
: "${SIM36:?no sim36 binary found; set SIM36}"
base=${DIFFRUN_BASE:-$here/test/diffrun-base.sim}

cut_and_filter() {
    if [ -n "$from" ]; then
        awk -v marker="sim36> $from" 'found || $0 == marker { found = 1; print }'
    else
        cat
    fi | {
        if [ -s "$work/ignores" ]; then
            grep -v -f "$work/ignores" || true
        else
            cat
        fi
    }
}

normalise() {
    # Prompts, product names, absolute temporary paths and clock values.
    sed -e 's/^sim> /sim36> /' \
        -e 's/^[a-z0-9]* - System\/36 reference emulator$/SIM\/36 - System\/36 emulator/' \
        -e 's#/tmp/[A-Za-z0-9._/-]*#/TMP#g' \
        -e 's#\(\.tmp\|\.s36\)\.[0-9a-f]\{32\}#\1.RANDOM#g' \
        -e 's/[0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}[T ][0-9]\{2\}:[0-9]\{2\}:[0-9]\{2\}[.0-9]*Z\{0,1\}/TIMESTAMP/g' \
    | cut_and_filter
}

work=$(mktemp -d "${TMPDIR:-/tmp}/diffrun.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
printf '%s\n' "$ignores" | sed '/^$/d' > "$work/ignores"
status=0
for file in "$@"; do
    abs=$(cd "$(dirname "$file")" && pwd)/$(basename "$file")
    (cd "$reference_dir" && $SIM36_REFERENCE -c "$base" -s "$abs" 2>&1; echo "exit=$?") | normalise >"$work/ref.txt"
    (cd "$here" && "$SIM36" -c "$base" -s "$abs" 2>&1; echo "exit=$?") | normalise >"$work/sim.txt"
    if [ ! -s "$work/ref.txt" ] || [ ! -s "$work/sim.txt" ]; then
        echo "EMPTY      $file (a transcript has no lines to compare; check --from)"
        status=1
    elif diff -u "$work/ref.txt" "$work/sim.txt" >"$work/diff.txt"; then
        [ $quiet -eq 1 ] || echo "identical  $file"
    else
        echo "DIFFERS    $file"
        cat "$work/diff.txt"
        status=1
    fi
done
exit $status
