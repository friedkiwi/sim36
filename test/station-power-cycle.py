#!/usr/bin/env python3
"""A TN5250 client hanging up is the display being switched off, and the next
client on that station is the display being switched on again.

Three phases, one emulator each:
  1  per-station listener: SIGN ON, hang up, reconnect -> a fresh SIGN ON
  2  station multiplexer:  the same through the menu
  3  signed-on session:    sign on to MAIN, hang up, reconnect -> SIGN ON
                           (the session was terminated, not resumed)
docs/s36/station-power-cycle-2026-09-11.md
"""
import importlib.util, os, subprocess, sys, threading, time
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sim36env  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
os.chdir(os.path.join(HERE, ".."))
spec = importlib.util.spec_from_file_location("tn5250drive", os.path.join(HERE, "tn5250drive.py"))
tn = importlib.util.module_from_spec(spec); spec.loader.exec_module(tn)

VOLUME = sim36env.volume()
BASE = int(os.environ.get("S36_POWER_PORT", "4000"))   # 4000..4019 by default
out = []
def log(s=""): out.append(s)


def emulator():
    proc = subprocess.Popen([sim36env.exe()],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    lines = []
    def reader():
        for line in iter(proc.stdout.readline, ""):
            lines.append(line)
    threading.Thread(target=reader, daemon=True).start()
    return proc, lines


def send(proc, cmd, settle=0.3):
    proc.stdin.write(cmd + "\n"); proc.stdin.flush(); time.sleep(settle)


def wait_for(lines, needle, timeout, after=0):
    dl = time.monotonic() + timeout
    while time.monotonic() < dl:
        if any(needle in x for x in lines[after:]): return True
        time.sleep(0.2)
    return False


def soft(s, text, timeout):
    try:
        s.wait_for_text(text, timeout=timeout); return True
    except TimeoutError:
        return False


def machine(mux_port, station_port):
    proc, lines = emulator()
    send(proc, "attach disk0 %s overlay" % VOLUME)
    send(proc, "set station 0.0 role console")
    for n in (1, 2, 3):
        send(proc, "set station 0.%d role display" % n)
        send(proc, "set station 0.%d listen 127.0.0.1:%d" % (n, station_port + n))
    if mux_port:
        send(proc, "set terminal multiplex listen 127.0.0.1:%d" % mux_port)
        send(proc, "set terminal multiplex on")
    else:
        send(proc, "set terminal multiplex off")
    send(proc, "set machine ipl-type unattend")
    send(proc, "ipl")
    send(proc, "wait idle 180")
    if not wait_for(lines, "guest is idle after", 200):
        raise SystemExit("IPL never reached idle\n" + "".join(lines))
    send(proc, "trace ws")
    return proc, lines


def connect(name, mux_port, station_port):
    if mux_port:
        s = tn.Session(mux_port, name=name).connect(timeout=25)
        s.wait_for_invite(20)
        s.type_into("Connect to workstation", "0.1")
        s.press("Enter")
    else:
        s = tn.Session(station_port + 1, name=name).connect(timeout=25)   # W2 = 0.1
    return s


def finish(proc, lines, phase):
    send(proc, "stations", 1.0)
    send(proc, "stop"); send(proc, "quit")
    proc.wait(timeout=30)
    log("=== phase %d monitor ===" % phase)
    log("".join(lines).rstrip())


def power_cycle(phase, mux_port, station_port, sign_on_first):
    proc, lines = machine(mux_port, station_port)
    c1 = connect("phase%d-first" % phase, mux_port, station_port)
    painted = soft(c1, "SIGN ON", 60)
    log("phase %d first client SIGN ON: %s" % (phase, "yes" if painted else "no"))
    if painted and sign_on_first:
        c1.type_into("User ID", "YVANJ")
        c1.press("Enter")
        at_main = soft(c1, "MAIN", 90)
        log("phase %d first client signed on to MAIN: %s" % (phase, "yes" if at_main else "no"))
    mark = len(lines)
    c1.close()
    powered_off = wait_for(lines, "display powered off", 20, after=mark)
    log("phase %d monitor reported the power-off: %s" % (phase, "yes" if powered_off else "no"))
    # Let the guest digest the failed operation before the terminal comes back.
    send(proc, "wait idle 30")
    wait_for(lines, "guest is idle after", 40, after=mark)
    c2 = connect("phase%d-second" % phase, mux_port, station_port)
    painted2 = soft(c2, "SIGN ON", 60)
    log("phase %d second client SIGN ON: %s" % (phase, "yes" if painted2 else "no"))
    log("phase %d second client sees MAIN instead: %s"
        % (phase, "yes" if c2.screen.contains("Main System/36") else "no"))
    if not painted2:
        log("=== phase %d second client screen (%d record(s)) ===" % (phase, len(c2.records)))
        log(c2.screen.render())
    if painted2:
        c2.press("Enter")
        try:
            c2.wait_for_change(timeout=30)
        except TimeoutError:
            pass
        log("phase %d second client's Enter was answered: %s"
            % (phase, "yes" if c2.screen.contains("SYS-5520") else "no"))
    c2.close()
    finish(proc, lines, phase)


phases = os.environ.get("S36_POWER_PHASES", "123")
if "1" in phases: power_cycle(1, 0, BASE, False)
if "2" in phases: power_cycle(2, BASE + 10, BASE + 10, False)
if "3" in phases: power_cycle(3, 0, BASE + 5, True)
print("\n".join(out))
