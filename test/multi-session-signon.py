#!/usr/bin/env python3
"""Sign three workstations on concurrently.

The third sign-on's input workspace starts beyond the first 2 KiB page.  This
guards the full translated-block displacement used by deferred command-42
Read Input Fields delivery (a truncation to the in-page offset causes
SYS-5552 on W3).
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
from tn5250drive import Session  # noqa: E402


def main():
    config = sim36env.default_config()
    proc = subprocess.Popen(sim36env.command(config), cwd=ROOT,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, universal_newlines=True,
                            bufsize=1)
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
                                       (text, "".join(transcript[-80:])))
                changed.wait(min(left, 0.25))

    def wait_for_main(session, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with session.lock:
                if session.screen.contains("Main System/36 help menu"):
                    return
                if session.screen.contains("SYS-5552"):
                    raise AssertionError("%s sign-on reported SYS-5552" %
                                         session.name)
            time.sleep(0.05)
        raise TimeoutError("%s never reached MAIN" % session.name)

    sessions = []
    try:
        command("set machine ipl-type unattend")
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

        for number, user in enumerate(("YVANJ", "YVANJ2", "YVANJ3"), 1):
            workstation = "W%d" % number
            session = Session(port_base, name=workstation).connect(timeout=25)
            sessions.append(session)
            session.wait_for_text("Connect to workstation", timeout=20)
            session.type_into("Connect to workstation", workstation)
            session.press("Enter")
            session.wait_for_text("SIGN ON", timeout=60)
            session.type_into("User ID", user)

            # Let the worker which painted SIGN ON terminate before its input
            # is answered; a headless client can otherwise outrun SSP here.
            mark = len(transcript)
            command("wait idle 30")
            wait_monitor("wait: guest is idle after", timeout=35, after=mark)
            session.press("Enter")
            try:
                wait_for_main(session)
            except (AssertionError, TimeoutError):
                print("=== %s SCREEN ===" % workstation, file=sys.stderr)
                print(session.screen.render(fields=True), file=sys.stderr)
                print("=== MONITOR TAIL ===", file=sys.stderr)
                print("".join(transcript[-120:]), file=sys.stderr)
                raise
            print("%s signed on as %s" % (workstation, user))

        print("PASS: W1, W2, and W3 reached MAIN concurrently")
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
