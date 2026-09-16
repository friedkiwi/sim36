#!/usr/bin/env python3
"""Prove listeners outlive lazy machine construction and reset."""
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


proc = subprocess.Popen(
    sim36env.command(),
    cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT, text=True, bufsize=1)
lines = []
changed = threading.Condition()


def reader():
    for line in iter(proc.stdout.readline, ""):
        with changed:
            lines.append(line)
            changed.notify_all()


threading.Thread(target=reader, daemon=True).start()


def command(value):
    proc.stdin.write(value + "\n")
    proc.stdin.flush()


def wait_for(value, mark=0, timeout=120):
    deadline = time.time() + timeout
    with changed:
        while value not in "".join(lines[mark:]):
            left = deadline - time.time()
            if left <= 0:
                raise TimeoutError("monitor did not report %r\n%s" %
                                   (value, "".join(lines[-100:])))
            changed.wait(min(left, 0.25))


w2 = None
try:
    # The default command file has completed, but no Machine exists. The
    # Station 0.1 can already be selected and parked before IPL.
    w2 = Session(2300, name="pre-IPL W2").connect(timeout=20)
    w2.wait_for_text("Connect to workstation", timeout=20)
    w2.type_into("Connect to workstation", "0.1")
    w2.press("Enter")
    w2.wait_for_text("Waiting for IPL", timeout=20)
    print("multiplexer accepted W2 before IPL")

    mark = len(lines)
    command("ipl")
    command("wait idle 120")
    wait_for("wait: guest is idle after", mark)
    w2.wait_for_text("SIGN ON", timeout=60)
    print("preconnected W2 reached SIGN ON after lazy construction")

    # Construction settings are latched until reset.
    mark = len(lines)
    command("set machine task-work-area 61")
    wait_for("definition is latched by the current IPL", mark, 10)

    generation = w2.generation
    mark = len(lines)
    command("reset --yes")
    wait_for("reset complete; machine stopped and configuration editable", mark, 20)
    command("set machine task-work-area 61")
    command("ipl")
    command("wait idle 120")
    wait_for("wait: guest is idle after", mark)
    w2.wait_for_change(timeout=60, since=generation)
    w2.wait_for_text("SIGN ON", timeout=60)
    if w2.sock is None or w2._stop:
        raise AssertionError("W2 socket did not survive reset and reconstruction")
    print("same W2 socket survived reset and reached SIGN ON after reconstruction")
finally:
    if w2 is not None:
        w2.close()
    if proc.poll() is None:
        command("stop")
        command("quit")
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            proc.kill()
