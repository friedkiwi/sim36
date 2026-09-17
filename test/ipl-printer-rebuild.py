#!/usr/bin/env python3
"""Boot the shipped printer topology through SSP's file-rebuild completion."""

import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import sim36env  # noqa: E402
from tn5250drive import Session  # noqa: E402


def main():
    config = sim36env.default_config("default-printer-machine.sim.in",
                                     attach_mode=os.environ.get("S36_ATTACH_MODE", "overlay"))
    emu = subprocess.Popen(
        sim36env.command(config), cwd=ROOT, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    transcript = []
    changed = threading.Condition()

    def collect():
        for line in iter(emu.stdout.readline, ""):
            with changed:
                transcript.append(line)
                changed.notify_all()

    threading.Thread(target=collect, daemon=True).start()

    def command(line):
        emu.stdin.write(line + "\n")
        emu.stdin.flush()

    def wait_monitor(text, timeout=90, after=0):
        deadline = time.time() + timeout
        with changed:
            while text not in "".join(transcript[after:]):
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError("monitor did not report %r\n%s" %
                                       (text, "".join(transcript[-160:])))
                changed.wait(min(left, 0.25))

    w3 = None
    try:
        command("set machine ipl-type unattend")
        port_base = int(os.environ.get("S36_PORT_BASE", "2300"))
        if port_base != 2300:
            command("set terminal multiplex listen 127.0.0.1:%d" % port_base)
            for station in range(2, 7):
                command("set station 0.%d listen 127.0.0.1:%d" %
                        (station, port_base + station))
        if os.environ.get("S36_PRINTER_IPL_TRACE"):
            command("trace " + os.environ["S36_PRINTER_IPL_TRACE"])
        if os.environ.get("S36_PRINTER_IPL_MEMBER"):
            command("trace member " + os.environ["S36_PRINTER_IPL_MEMBER"])
        mark = len(transcript)
        watches = [a.strip() for a in os.environ.get("S36_PRINTER_IPL_WATCH", "").split(",") if a.strip()]
        if watches:
            command("ipl pause")
            wait_monitor("IPL paused before instruction 1", timeout=30, after=mark)
            for address in watches:
                command("watch " + address)
            command("start")
        else:
            command("ipl")
        # Match the reported ordering: W3 binds while unattended IPL and its
        # rebuild are running.  A fast successful run can replace the progress
        # panel before the client polls it, so the non-racy oracle is the real
        # guest SIGN ON panel.  An instruction count or idle state never passes.
        w3 = Session(port_base, name="W3").connect(timeout=25)
        w3.wait_for_text("Connect to workstation", timeout=20)
        w3.type_into("Connect to workstation", "0.2")
        w3.press("Enter")
        w3.wait_for_text("SIGN ON", timeout=int(os.environ.get("S36_PRINTER_IPL_TIMEOUT", "180")))
        if "CHECK [" in "".join(transcript[mark:]):
            raise AssertionError("SSP stopped on a processor check during printer rebuild")
        # QH51 is SLIC's printer-unit chain.  Its three-byte head at 0BCD must
        # name the D7E4 (PU) block #SVTUB built from command-82 discovery; this
        # keeps the regression on guest/SLIC semantics rather than the older
        # synthetic printer monitor seam.
        printer_mark = len(transcript)
        command("dump 0BCD 3")
        wait_monitor("000bcd", timeout=10, after=printer_mark)
        printer_report = "".join(transcript[printer_mark:])
        qh51 = re.search(r"000bcd\s+([0-9a-f]{2})\s+([0-9a-f]{2})\s+([0-9a-f]{2})", printer_report, re.I)
        if qh51 is None or qh51.groups() == ("00", "00", "00"):
            raise AssertionError("SSP reached SIGN ON without a printer on QH51\n" + printer_report)
        if os.environ.get("S36_PRINTER_IPL_TRACE_FULL"):
            print("".join(transcript))
        print("PASS: shipped PB-printer topology completed SSP 7.5 file rebuild and W3 reached SIGN ON")
        return 0
    except Exception:
        print("=== printer IPL monitor tail ===", file=sys.stderr)
        lines = transcript if os.environ.get("S36_PRINTER_IPL_TRACE_FULL") else transcript[-240:]
        print("".join(lines), file=sys.stderr)
        if w3 is not None:
            print(w3.screen.render("=== W3 ===", fields=True), file=sys.stderr)
        raise
    finally:
        if w3 is not None:
            w3.close()
        if emu.poll() is None:
            try:
                command("quit")
                emu.wait(timeout=10)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                emu.kill()
                emu.wait()
        try:
            os.unlink(config)
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
