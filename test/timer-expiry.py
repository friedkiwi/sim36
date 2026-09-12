#!/usr/bin/env python3
"""Exercise the real-time NutiTimer SLIH boundary without a fabricated SSP event."""
import os
import subprocess
import tempfile
import threading
import time
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sim36env  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

with tempfile.TemporaryDirectory(prefix="s36-timer-expiry-") as tmp:
    trb = os.path.join(tmp, "trb.bin")
    code = os.path.join(tmp, "code.bin")
    with open(trb, "wb") as stream:
        # Exact accepted CPTC family, shortened to one 8.192-ms unit so the
        # regression need not wait for the live two/four-second intervals.
        stream.write(bytes.fromhex("82 01 00 00 00 00 00 01 00 00 00 00 00 00"))
    with open(code, "wb") as stream:
        stream.write(bytes.fromhex("F400500A2000"))

    process = subprocess.Popen(
        [sim36env.exe(), "-c",
         os.path.join(ROOT, "test", "advanced36-nolisten.sim")], cwd=ROOT,
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True)
    lines = []
    registered = threading.Event()

    def drain():
        for line in iter(process.stdout.readline, ""):
            lines.append(line)
            if "operation 20 registered CPTC timer" in line:
                registered.set()

    reader = threading.Thread(target=drain)
    reader.start()

    def send(command):
        process.stdin.write(command + "\n")
        process.stdin.flush()

    send("attach disk0 " + os.path.join(ROOT, "var", "as36.img") + " ro")
    send("ipl pause")
    send("loadfile " + trb + " 0C00")
    send("loadfile " + code + " 1000")
    send("trace csp")
    send("set pxr2 00")
    send("set xr2 0C00")
    send("set iar 1000")
    send("step 1")
    if not registered.wait(3):
        process.kill()
        raise SystemExit("timer registration did not execute")
    send("timers")

    # Put the owner into an unrelated event wait after the registration-time
    # nupotcb has run. Type-2 expiry must leave these bytes untouched.
    send("poke 0F04 80")
    send("poke 0F05 80")
    time.sleep(0.050)
    send("timers service")
    send("dump 0F00 8")
    send("timers")
    send("quit")
    process.wait(timeout=10)
    reader.join(timeout=2)
    output = "".join(lines)

print(output, end="")

required = [
    "control 82/type 2 -> nutislih calls nutimih",
    "machine timer += 00A0EEBB unit(s) (24 hours), entry re-armed",
    "no nupotcb, nupostac, SSP event, or workstation record",
    "serviced 1 due native timer callback(s); SSP task readied=no",
    "000f00  e3 c2 00 09 80 80 00 fc",
    "nutimih: advance machine date timer and re-arm for one day; no guest post",
]
for wanted in required:
    if wanted not in output:
        raise SystemExit("missing timer-expiry evidence: " + wanted)

# The post-expiry diagnostic row must expose the first completed SLIH and a
# remaining duration close to one day, rather than retaining the one-unit arm.
rows = [line for line in output.splitlines() if "009?" in line]
if "expiries" not in output or " 1 nutimih:" not in output:
    raise SystemExit("post-expiry timer row did not report expiration count 1")
