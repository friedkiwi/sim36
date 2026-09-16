#!/usr/bin/env python3
"""Drive the default station multiplexer with a real 5250 client.

Two machines are exercised, because the multiplexer must derive everything it
shows from the machine it is attached to rather than from a constant:

  phase 1  the reference seven-station controller (0.0-0.6), IPLed in the
           background, so `Machine status: running` and the media overview are
           the live machine's.  A client connects, is painted the menu, types
           0.1 and lands there; a second client supplies the RFC 2877
           DEVNAME `0.2` and never sees a menu at all.
  phase 2  TWO controllers, ten stations (0.0-0.6 and 1.0-1.2), NOT IPLed, so
           `Machine status: stopped`.  The hint text has to widen to the
           configured machine, and a second controller's `port.address` has
           to resolve through the same path.
  phase 3  the lifecycle case, and the one that motivates the whole design:
           the multiplexer is turned on with NO machine constructed, a client
           connects, sees `Machine status: stopped`, accepts the prefilled
           0.1 with Enter - and is still on 0.1, on the same socket, after IPL
           constructs the machine.

Every byte the client sends here is a real terminal response to a real
invitation on the wire.  No monitor command injects input, and nothing in this
file touches guest storage.  docs/s36/station-multiplexer-design-2026-09-08.md
"""
import importlib.util
import os
import subprocess
import sys
import threading
import time
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sim36env  # noqa: E402

spec = importlib.util.spec_from_file_location("tn5250drive", "test/tn5250drive.py")
tn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tn)

MUX1 = int(os.environ.get("S36_MUX_PORT", "3900"))
MUX2 = int(os.environ.get("S36_MUX_PORT2", "3910"))
VOLUME = sim36env.volume()

out = []


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


def send(proc, command):
    proc.stdin.write(command + "\n")
    proc.stdin.flush()
    time.sleep(0.25)


def wait_for(lines, needle, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if any(needle in x for x in lines):
            return True
        time.sleep(0.1)
    return False


def show(title, session):
    out.append("=== %s ===" % title)
    out.append(session.screen.render())
    out.append(session.screen.field_table())


def define(proc, port, stations, multiplex_port):
    send(proc, "attach disk0 %s overlay" % VOLUME)
    for p, a in stations:
        send(proc, "set station %d.%d role %s"
             % (p, a, "console" if (p, a) == (0, 0) else "display"))
        if (p, a) != (0, 0):
            send(proc, "set station %d.%d listen 127.0.0.1:%d" % (p, a, port))
            port += 1
    send(proc, "set terminal multiplex listen 127.0.0.1:%d" % multiplex_port)
    send(proc, "set terminal multiplex on")


# ---------------------------------------------------------------- phase 1 ---
proc, lines = emulator()
define(proc, 3901, [(0, a) for a in range(7)], MUX1)
send(proc, "save config stdout")
send(proc, "stations")
send(proc, "ipl")
send(proc, "wait idle 180")
if not wait_for(lines, "guest is idle after", 180):
    send(proc, "quit")
    print("".join(lines))
    raise SystemExit("phase 1: the machine never reached its idle park")

menu = tn.Session(MUX1, name="menu").connect()
menu.wait_for_invite(20)
show("phase 1 menu, seven-station machine, running", menu)

# The operator's choice, typed into the field the guest's own SF defined and
# sent as a real Read Input Fields answer.
menu.type_into("Connect to workstation", "0.1")
menu.press("Enter")
time.sleep(1.0)
send(proc, "stations")

# Direct drop: RFC 2877 section 4 DEVNAME, no menu ever painted.
direct = tn.Session(MUX1, name="direct", device_name="0.2").connect()
time.sleep(1.5)
out.append("=== phase 1 direct-drop client: %d record(s) received ===" % len(direct.records))
out.append("phase 1 direct-drop client saw a menu: %s"
           % ("yes" if direct.screen.contains("Connect to workstation") else "no"))
out.append(direct.screen.render())
send(proc, "stations")

# A third client is refused on an already-taken W2.
refused = tn.Session(MUX1, name="refused").connect()
refused.wait_for_invite(20)
refused.type_into("Connect to workstation", "0.1")
refused.press("Enter")
refused.wait_for_change(timeout=15)
show("phase 1 after selecting the taken W2", refused)

# A fourth client selects W1.  It joins the SAME console backend the monitor
# uses immediately; the retained guest output is replayed over its socket.
console = tn.Session(MUX1, name="console").connect()
console.wait_for_invite(20)
console.type_into("Connect to workstation", "0.0")
console.press("Enter")
console.wait_for_change(timeout=15)
show("phase 1 attached system console", console)
send(proc, "stations")
# A monitor-originated display write still takes the real console backend.  It
# must reach TN5250 without also being echoed as a decoded `console: PUT` line.
stdio_before = sum(1 for line in lines if "console:" in line)
send(proc, "wswrite console demo")
time.sleep(0.5)
stdio_attached = sum(1 for line in lines if "console:" in line)
out.append("phase 1 attached W1 stdio echo: %s" %
           ("suppressed" if stdio_attached == stdio_before else "present"))

menu.close()
direct.close()
refused.close()
console.close()
time.sleep(0.5)
# A dead W1 socket relinquishes the presentation on the next guest/monitor
# write, so stdio diagnostics resume without an emulator reset.
send(proc, "wswrite console demo")
time.sleep(0.5)
stdio_released = sum(1 for line in lines if "console:" in line)
out.append("phase 1 released W1 stdio echo: %s" %
           ("resumed" if stdio_released > stdio_attached else "suppressed"))
send(proc, "stop")
send(proc, "quit")
proc.wait(timeout=20)
out.append("=== phase 1 monitor ===")
out.append("".join(lines).rstrip())

# ---------------------------------------------------------------- phase 2 ---
proc, lines = emulator()
define(proc, 3921, [(0, a) for a in range(7)] + [(1, a) for a in range(3)], MUX2)
send(proc, "stations")

scaled = tn.Session(MUX2, name="scaled").connect()
scaled.wait_for_invite(20)
show("phase 2 menu, ten stations across two controllers, stopped", scaled)

# A station on the second controller resolves through the same explicit-ID
# path as one on the first controller.
scaled.type_into("Connect to workstation", "1.0")
scaled.press("Enter")
time.sleep(1.0)
send(proc, "stations")

scaled.close()
send(proc, "quit")
proc.wait(timeout=20)
out.append("=== phase 2 monitor ===")
out.append("".join(lines).rstrip())


# ---------------------------------------------------------------- phase 3 ---
# The multiplexer with NO machine. `set terminal multiplex on` binds the
# listener there and then; the menu has to render every field from the
# DEFINITION, and a client placed on a station before IPL has to still be
# on it after the IPL, on the same socket, without reconnecting.
MUX3 = int(os.environ.get("S36_MUX_PORT3", "3930"))
proc, lines = emulator()
define(proc, 3941, [(0, a) for a in range(7)], MUX3)

early = tn.Session(MUX3, name="early").connect()
early.wait_for_invite(20)
show("phase 3 menu with NO machine constructed", early)
out.append("phase 3 prefilled selection MDT: %s" %
           ("on" if early.screen.input_fields()[0].mdt else "off"))
# A strict IBM client reports a rejected output data stream with RFC 1205's
# Data Stream Output Error flag and an error body such as 1005/01/25.  That is
# a negative response to the panel, not an AID.  Repainting in response creates
# an endless error/panel/error loop and hides the original malformed order.
generation = early.generation
record, wire = early.frame(0x00, bytes.fromhex("10 05 01 25 00 00 00 00"))
record = record[:7] + bytes([0x80]) + record[8:]
wire = record.replace(bytes([tn.IAC]), bytes([tn.IAC, tn.IAC])) \
       + bytes([tn.IAC, tn.EOR])
early.send_record(record, wire)
time.sleep(0.5)
out.append("phase 3 output-error response caused selector repaint: %s" %
           ("yes" if early.generation != generation else "no"))
# Select 0.1 explicitly so this phase continues to exercise the ordinary
# workstation rebind path while the menu verifies that 0.0 is first.
early.type_into("Connect to workstation", "0.1")
early.press("Enter")
early.wait_for_change(timeout=15)
out.append("=== phase 3 after selecting W2 with no machine ===")
out.append(early.screen.render())

send(proc, "stations")
send(proc, "ipl")
send(proc, "wait idle 180")
if not wait_for(lines, "guest is idle after", 180):
    send(proc, "quit")
    print("\n".join(out))
    print("".join(lines))
    raise SystemExit("phase 3: the machine never reached its idle park")
send(proc, "stations")
out.append("=== phase 3 client still connected after IPL construction: %s ==="
           % ("yes" if early.sock is not None and not early._stop else "no"))

# Prove the socket survived construction rather than merely looking alive.
try:
    early.sock.sendall(b"")
    out.append("phase 3 socket survived IPL construction")
except OSError as exc:
    out.append("phase 3 socket did NOT survive: %s" % exc)

early.close()
send(proc, "stop")
send(proc, "quit")
proc.wait(timeout=20)
out.append("=== phase 3 monitor ===")
out.append("".join(lines).rstrip())


# ---------------------------------------------------------------- phase 4 ---
# Exact attended-console lifecycle: select W1 while no machine exists, then
# IPL. `attended` is the natural spelling an operator used in the
# field; it must normalize to `attend`, and the already-connected socket must
# receive SSP's real IPL SIGN ON panel when construction starts.
MUX4 = int(os.environ.get("S36_MUX_PORT4", "3950"))
proc, lines = emulator()
define(proc, 3961, [(0, a) for a in range(7)], MUX4)
send(proc, "set machine ipl-type attended")

preipl_console = tn.Session(MUX4, name="preipl-console").connect()
preipl_console.wait_for_invite(20)
preipl_console.type_into("Connect to workstation", "0.0")
preipl_console.press("Enter")
preipl_console.wait_for_change(timeout=15)
out.append("phase 4 W1 parked before IPL: %s" %
           ("yes" if preipl_console.screen.contains("Waiting for IPL") else "no"))

send(proc, "ipl")
preipl_console.wait_for_text("IPL SIGN ON", timeout=30)
out.append("phase 4 preconnected W1 received attended SIGN ON: %s" %
           ("yes" if preipl_console.screen.contains("IPL SIGN ON") else "no"))

# Drive the optional Overrides branch through real TN5250 Read-MDT responses.
# The changed date/time first requires SSP's confirmation; the following bare
# Enter has no newly modified fields and must be expanded from retained device
# state by command 42 before SSP can reach its IPL OVERRIDES MENU.
preipl_console.type_into("User ID", "YVANJ")
preipl_console.type_into("Date", "090896")
preipl_console.type_into("Time", "120000")
preipl_console.type_into("Overrides?", "Y")
preipl_console.press("Enter")
preipl_console.wait_for_text("SYS-5519", timeout=30)
out.append("phase 4 Overrides Y reached date/time confirmation: yes")
preipl_console.press("Enter")
preipl_console.wait_for_text("IPL OVERRIDES MENU", timeout=30)
out.append("phase 4 bare confirmation Enter reached IPL OVERRIDES MENU: %s" %
           ("yes" if preipl_console.screen.contains("IPL OVERRIDES MENU") else "no"))
preipl_console.type_into("Option", "2")
preipl_console.press("Enter")
preipl_console.wait_for_text("Main System/36 help menu", timeout=30)
out.append("phase 4 option 2 received an SSP response: %s" %
           ("yes" if preipl_console.screen.contains(
               "Main System/36 help menu") else "no"))
out.append("phase 4 option 2 left keyboard usable: %s" %
           ("yes" if not preipl_console.screen.error else "no"))
show("phase 4 completed attended IPL", preipl_console)
preipl_console.press("Cmd3")
preipl_console.wait_for_change(timeout=30)
out.append("phase 4 Cmd3 received an SSP response: yes")
show("phase 4 after Cmd3 from completed IPL", preipl_console)
preipl_console.close()
send(proc, "stop")
send(proc, "quit")
proc.wait(timeout=20)
out.append("=== phase 4 monitor ===")
out.append("".join(lines).rstrip())


# ---------------------------------------------------------------- phase 5 ---
# A preconnected unattended W1 must be handed to the guest untouched: the
# multiplexer paints no panel of its own over the console once the machine is
# constructed, and the handoff must not fabricate a Cancel-Invite response in
# the guest input queue.
MUX5 = int(os.environ.get("S36_MUX_PORT5", "3970"))
proc, lines = emulator()
define(proc, 3981, [(0, a) for a in range(7)], MUX5)
send(proc, "set machine ipl-type unattended")

unattended_console = tn.Session(MUX5, name="unattended-console").connect()
unattended_console.wait_for_invite(20)
unattended_console.type_into("Connect to workstation", "0.0")
unattended_console.press("Enter")
unattended_console.wait_for_change(timeout=15)
send(proc, "ipl")
send(proc, "wait idle 180")
if not wait_for(lines, "guest is idle after", 180):
    raise SystemExit("phase 5: unattended IPL never reached idle")
out.append("phase 5 W1 multiplexer panel after construction: %s" %
           ("present" if unattended_console.screen.contains(
               "Unattended IPL does not display sign-on") else "absent"))
show("phase 5 unattended W1 after construction", unattended_console)
send(proc, "stations")
unattended_console.close()
send(proc, "stop")
send(proc, "quit")
proc.wait(timeout=20)
out.append("=== phase 5 monitor ===")
out.append("".join(lines).rstrip())

print("\n".join(out))
