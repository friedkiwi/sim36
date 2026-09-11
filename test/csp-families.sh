#!/bin/sh
# Milestone 5 gate: the CSP families against the reference, as full transcripts.
#
#   1. The whole main storage IPL from `ipl pause` to the first work-station
#      supervisor call (SVC 43 command 82 at instruction 33434, milestone 6),
#      under every trace class, followed by the task, module, queue-space
#      and ATR-file inspection commands.
#   2. The vector suites' own command files, run through both emulators and
#      compared line for line: the program-block, work-area, dispatcher,
#      action-controller, resource, loader, print-buffer and system-queue-space
#      suites.  SVC 2E reports the host clock and is excluded.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "${SIM36_VOLUME:-}" ] || [ ! -f "${SIM36_VOLUME:-}" ]; then
    echo "csp-families: no volume (set SIM36_VOLUME); skipped"
    exit 0
fi
if [ -z "${SIM36_REFERENCE:-}" ]; then
    echo "csp-families: no reference emulator (set SIM36_REFERENCE); skipped"
    exit 0
fi
work=$(mktemp -d "${TMPDIR:-/tmp}/sim36-fam.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
volume=$(cd "$(dirname "$SIM36_VOLUME")" && pwd)/$(basename "$SIM36_VOLUME")
status=0

cat >"$work/frontier.sim" <<SIM
do $here/test/advanced36-nolisten.sim
attach disk0 "$volume" overlay
trace csp,svc,ace,disk,sched
ipl pause
step 33434
show cpu
tasklist
tasklist current
whereis
modules
mapstate
sqsstate
show ptt
actions
timers
quit
SIM
"$here/tools/diffrun.sh" "$work/frontier.sim" || status=1

# Extract each suite's builder call and first command file, with the suite's
# own substitutions applied, and compare the transcript.
for suite in program-block-svcs work-area-svcs task-dispatcher action-controller resource-svcs \
             relocating-loader print-buffer system-queue-space; do
    python3 - "$here" "$volume" "$work" "$suite" <<'PY'
import re, subprocess, sys
here, volume, work, suite = sys.argv[1:5]
s = open(f"{here}/test/{suite}.sh").read()
env = {"PWD": here, "SIM36_VOLUME": volume, "TMP": work}
def subst(t):
    return re.sub(r"\$(\w+)", lambda m: env.get(m.group(1), m.group(0)), t)
# builder lines: "python3 test/build-...py args" possibly inside end=$(...)
for m in re.finditer(r"python3 test/(build-[a-z-]+\.py)([^\n)]*)", s):
    cmd = ["python3", f"{here}/test/{m.group(1)}"] + subst(m.group(2)).split()
    out = subprocess.run(cmd, capture_output=True, text=True, cwd=here).stdout.strip()
    if out: env["end"] = out
m = re.search(r'cat > "\$TMP/([a-z]+)\.sim" <<EOF\n(.*?)\nEOF\n', s, re.S)
open(f"{work}/{suite}.sim", "w").write(subst(m.group(2)) + "\n")
PY
    "$here/tools/diffrun.sh" --ignore 'SVC 2E: time of day' --ignore '^IAR 100C  ARR 0000  XR1 0092' \
        "$work/$suite.sim" || status=1
done
exit $status
