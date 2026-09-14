#!/usr/bin/env python3
"""Drive a MAIN command to a screen or trace milestone and reject failures."""

import os
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
    proc = subprocess.Popen(sim36env.command(sim36env.default_config()), cwd=ROOT,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
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
            while not any(text in line for line in transcript[after:]):
                left = deadline - time.time()
                if left <= 0:
                    raise TimeoutError("monitor did not report %r\n%s" %
                                       (text, "".join(transcript[-120:])))
                changed.wait(min(left, 0.25))

    sessions = []
    try:
        command("set machine ipl-type unattend")
        if os.environ.get("S36_LIST_TRACE") == "1":
            command("trace csp")
        port_base = int(os.environ.get("S36_PORT_BASE", "2300"))
        if port_base != 2300:
            command("set terminal multiplex listen 127.0.0.1:%d" % port_base)
            for station in range(1, 7):
                command("set station 0.%d listen 127.0.0.1:%d" %
                        (station, port_base + station))
        mark = len(transcript)
        command("ipl")
        command("wait idle 90")
        wait_monitor("wait: guest is idle after", after=mark)

        # Bring up the system console first.  On unattended IPL it is W1's
        # connection that completes workstation initialization; attaching
        # W2 first can legitimately leave it on "IPL is in progress".
        for number, user in ((1, "YVANJ"), (2, "YVANJ2")):
            workstation = "W%d" % number
            session = Session(port_base, name=workstation).connect(timeout=25)
            sessions.append(session)
            session.wait_for_text("Connect to workstation", timeout=20)
            session.type_into("Connect to workstation", workstation)
            session.press("Enter")
            session.wait_for_text("SIGN ON", timeout=60)
            session.type_into("User ID", user)
            mark = len(transcript)
            command("wait idle 30")
            wait_monitor("wait: guest is idle after", timeout=35, after=mark)
            session.press("Enter")
            session.wait_for_text("Main System/36 help menu", timeout=90)

        target = os.environ.get("S36_LIST_STATION", "W2").upper()
        session = sessions[int(target[1:]) - 1]

        mark = len(transcript)
        command("wait idle 30")
        wait_monitor("wait: guest is idle after", timeout=35, after=mark)
        if os.environ.get("S36_LIST_TRACE") == "1":
            command("trace isn flow csp")
            command("trace member " + os.environ.get("S36_LIST_TRACE_MEMBER", "$MAIN"))
            for watch in os.environ.get("S36_LIST_WATCH", "").split(","):
                if watch.strip():
                    command("watch " + watch.strip())
        statement = os.environ.get("S36_LIST_COMMAND", "LISTLIBR ALL,SOURCE,#LIBRARY,USER,NOPAGE")
        expected_screen = os.environ.get("S36_LIST_EXPECT_SCREEN", "BASICSMP")
        expected_monitor = os.environ.get("S36_LIST_EXPECT_MONITOR", "")
        rejected = tuple(value for value in os.environ.get("S36_LIST_REJECT", "").split("|") if value)
        session.type_at(22, 3, statement)
        check_mark = len(transcript)
        session.press("Enter")

        deadline = time.monotonic() + 120
        saw_listing = False
        while time.monotonic() < deadline:
            with session.lock:
                saw_listing |= bool(expected_screen) and session.screen.contains(expected_screen)
                saw_rejected = any(session.screen.contains(value) for value in rejected)
            with changed:
                text = "".join(transcript[check_mark:])
            saw_listing |= bool(expected_monitor) and expected_monitor in text
            if ("CHECK [program]" in text or "storage protection" in text or saw_rejected or
                    any(value in text for value in rejected)):
                print(session.screen.render("=== LIST ALL ===", fields=True), file=sys.stderr)
                with changed:
                    lines = transcript[:]
                if os.environ.get("S36_LIST_TRACE") == "1":
                    trace_member = os.environ.get("S36_LIST_TRACE_MEMBER", "$MAIN")
                    print("".join(line for line in lines
                                  if "nucmclr" in line or trace_member in line or
                                  "SVC 22" in line or "nupterm" in line),
                          file=sys.stderr)
                hit = next((n for n, line in enumerate(lines)
                            if "LEVEL 5: main-storage-program" in line), len(lines))
                print("".join(lines[max(0, hit - 120):hit + 20]), file=sys.stderr)
                raise AssertionError("LIST ALL stopped on a processor check")
            if saw_listing:
                time.sleep(float(os.environ.get("S36_LIST_SETTLE", "30")))
                with changed:
                    text = "".join(transcript[check_mark:])
                with session.lock:
                    saw_rejected = any(session.screen.contains(value) for value in rejected)
                if ("CHECK [program]" in text or "storage protection" in text or saw_rejected or
                        any(value in text for value in rejected)):
                    raise AssertionError("LIST ALL stopped after displaying source")
                break
            time.sleep(0.05)
        if not saw_listing:
            raise TimeoutError("LIST ALL did not display BASICSMP\n%s" %
                               session.screen.render(fields=True))

        # Optional post-screen key sequence.  This keeps application-level
        # regressions tied to the real panel that accepts the keys instead of
        # merely proving that the command's loader was entered.
        for key in (k.strip() for k in os.environ.get("S36_LIST_KEYS", "").split(",") if k.strip()):
            generation = session.generation
            session.press(key)
            deadline = time.monotonic() + 5
            while session.generation == generation and time.monotonic() < deadline:
                time.sleep(0.05)
            session.settle(quiet=0.2, timeout=5)
            with changed:
                text = "".join(transcript[check_mark:])
            if "CHECK [program]" in text or "CHECK [CSP/MSP/channel]" in text or "storage protection" in text:
                raise AssertionError("%s caused a processor check\n%s\n%s" %
                                     (key, session.screen.render(fields=True), text[-12000:]))
        if "S36_LIST_COMMAND" in os.environ:
            print("PASS: %s reached its expected output without a processor check" % statement)
        else:
            print("PASS: LIST ALL displayed source without a processor check")
    finally:
        for session in sessions:
            session.close()
        if proc.poll() is None:
            command("quit")
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    main()
