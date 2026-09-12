#!/usr/bin/env python3
"""Research drive: sign YVANJ on at W2 after an unattended IPL, answering the
sign-on Read MDT Fields the way a real 5250 display does - SBA + data per
field with trailing nulls suppressed - instead of the driver's full-length
fields.  Reproduces a panic dump taken on 2026-09-10
(SYS-5552 Library <8 nulls> not found).

Set S36_NULL_SUPPRESS=0 to send the driver's usual full-length answer.
"""

import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import sim36env  # noqa: E402
from tn5250drive import Session, aid_code  # noqa: E402


def main():
    config = sim36env.default_config()
    proc = subprocess.Popen(sim36env.command(config),
                            cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True, bufsize=1)
    transcript = []
    changed = threading.Condition()

    def collect():
        for line in proc.stdout:
            with changed:
                transcript.append(line)
                changed.notify_all()
    threading.Thread(target=collect, daemon=True).start()

    def command(line):
        proc.stdin.write(line + "\n")
        proc.stdin.flush()

    def wait_monitor(text, timeout=90, after=0):
        deadline = time.time() + timeout
        with changed:
            while True:
                if any(text in l for l in transcript[after:]):
                    return
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError("monitor did not report %r\n%s" %
                                       (text, "".join(transcript[-80:])))
                changed.wait(min(left, 0.25))

    w2 = None
    try:
        command("set machine ipl-type unattend")
        # S36_PORT_BASE moves every listener so several emulators can share a
        # host (another session's instance on 2300 silently answers our driver).
        port_base = int(os.environ.get("S36_PORT_BASE", "2300"))
        if port_base != 2300:
            command("set terminal multiplex listen 127.0.0.1:%d" % port_base)
            for station in range(1, 7):
                command("set station 0.%d listen 127.0.0.1:%d" % (station, port_base + station))
        mark = len(transcript)
        command("ipl")
        command("wait idle 90")
        wait_monitor("wait: guest is idle after", after=mark)

        for pre in os.environ.get("S36_PRE_COMMANDS", "").split(";"):
            if pre.strip():
                command(pre.strip())
                time.sleep(0.5)
        w2 = Session(port_base, name="W2").connect(timeout=25)
        w2.wait_for_text("Connect to workstation", timeout=20)
        w2.type_into("Connect to workstation", "W2")
        w2.press("Enter")
        w2.wait_for_text("SIGN ON", timeout=60)
        mark = len(transcript)
        command("wait idle 30")
        wait_monitor("wait: guest is idle after", timeout=35, after=mark)
        print(w2.screen.render("=== W2 SIGN ON ===", fields=True))

        w2.type_into("User ID", "YVANJ")
        if os.environ.get("S36_TRACE_CLASSES"):
            command("trace " + os.environ["S36_TRACE_CLASSES"])
        for member in os.environ.get("S36_TRACE_MEMBERS", "").split(","):
            if member.strip():
                command("trace member " + member.strip())
        mark = len(transcript)
        if os.environ.get("S36_NULL_SUPPRESS", "1") == "1":
            # Real-display answer to Read MDT Fields: SBA + data for every
            # MDT field, trailing nulls stripped (libtn5250 send_field).
            w2.wait_for_invite(15)
            s = w2.screen
            row, col = s.cursor
            body = bytearray([row & 0xFF, col & 0xFF, aid_code("Enter")])
            for f in s.input_fields():
                if not f.mdt:
                    continue
                data = (f._pending if f._pending is not None
                        else bytes(s.buf[s._off(r, c)] for r, c in f.positions()))
                data = data.rstrip(b"\x00")
                body += bytes([0x11, f.row & 0xFF, f.col & 0xFF]) + data
            raw = os.environ.get("S36_RAW_BODY", "").strip()
            if raw:
                # Replay a captured wire body verbatim (cursor, AID, fields).
                body = bytearray(bytes.fromhex(raw))
            record, wire = w2.frame(0x00, bytes(body))
            print("sending %d-byte record: %s" % (len(record), record.hex()))
            with w2.lock:
                w2.invited.clear()
                w2.screen.keyboard_unlocked = False
                w2.send_record(record, wire)
                w2.screen.last_read = None
                for f in s.input_fields():
                    f._pending = None
                    f.mdt = False
        else:
            rec = w2.press("Enter")
            print("sending %d-byte record: %s" % (len(rec), rec.hex()))
        try:
            w2.wait_for_text("MAIN", timeout=90)
            print(w2.screen.render("=== W2 AFTER ENTER ===", fields=True))
            print("PASS: MAIN reached")
        except TimeoutError:
            print(w2.screen.render("=== W2 AFTER ENTER (no MAIN) ===", fields=True))
            print("FAIL: MAIN not reached")
        command("wait idle 30")
        wait_monitor("wait: guest is idle after", timeout=35, after=mark)
        for extra in os.environ.get("S36_POST_COMMANDS", "").split(";"):
            if extra.strip():
                m = len(transcript)
                command(extra.strip())
                time.sleep(1)
        print("=== monitor transcript from the %s ===" %
              ("start" if os.environ.get("S36_FULL_TRANSCRIPT") == "1" else "Enter"))
        print("".join(transcript[0 if os.environ.get("S36_FULL_TRANSCRIPT") == "1" else mark:]))
    finally:
        if w2 is not None:
            w2.close()
        command("quit")
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    main()
