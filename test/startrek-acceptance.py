#!/usr/bin/env python3
"""Drive the tape-installed STARTREK workflow through a real 5250 station."""

import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import sim36env  # noqa: E402
from tn5250drive import Session  # noqa: E402


def main():
    tape = os.environ["STARTREK_TAPE"]
    library_ready = bool(os.environ.get("STARTREK_SKIP_INSTALL"))
    port = int(os.environ.get("S36_PORT_BASE", "26300"))
    requested_volume = os.environ.get("STARTREK_PRIVATE_VOLUME")
    private_dir = None if requested_volume else tempfile.mkdtemp(
        prefix="sim36-startrek-volume-")
    private_volume = requested_volume or os.path.join(private_dir, "as36-private.img")
    if not (requested_volume and os.environ.get("STARTREK_REUSE_VOLUME") and
            os.path.exists(private_volume)):
        shutil.copyfile(sim36env.volume(), private_volume)
    configs = []
    transcript = []
    changed = threading.Condition()

    def collect(child):
        for line in child.stdout:
            if os.environ.get("STARTREK_ECHO_MONITOR"):
                print("MONITOR: " + line, end="", flush=True)
            with changed:
                transcript.append(line)
                changed.notify_all()

    def launch():
        machine_config = sim36env.default_config(
            "startrek-machine.sim.in", tape=os.path.abspath(tape),
            volume=private_volume, disk_mode="rw")
        configs.append(machine_config)
        child = subprocess.Popen(sim36env.command(machine_config), cwd=ROOT,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, universal_newlines=True,
                                 bufsize=1)
        threading.Thread(target=collect, args=(child,), daemon=True).start()
        return child

    proc = launch()

    def monitor(line):
        proc.stdin.write(line + "\n")
        proc.stdin.flush()

    def wait_monitor(text, timeout=90, after=0):
        deadline = time.monotonic() + timeout
        with changed:
            while not any(text in line for line in transcript[after:]):
                left = deadline - time.monotonic()
                if left <= 0:
                    raise TimeoutError("monitor did not report %r\n%s" %
                                       (text, "".join(transcript[-200:])))
                changed.wait(min(left, 0.25))

    def monitor_text(after):
        with changed:
            return "".join(transcript[after:])

    sessions = []
    try:
        def boot():
            monitor("set machine ipl-type unattend")
            monitor("set terminal multiplex listen 127.0.0.1:%d" % port)
            for station in range(2, 7):
                monitor("set station 0.%d listen 127.0.0.1:%d" %
                        (station, port + station))
            if os.environ.get("STARTREK_TRACE"):
                monitor("trace " + os.environ["STARTREK_TRACE"])
            if os.environ.get("STARTREK_TRACE_MEMBER"):
                monitor("trace member " + os.environ["STARTREK_TRACE_MEMBER"])
            if os.environ.get("STARTREK_BREAK"):
                monitor("breakm " + os.environ["STARTREK_BREAK"])
            boot_mark = len(transcript)
            monitor("ipl")
            monitor("wait idle 90")
            wait_monitor("wait: guest is idle after", after=boot_mark)

        def sign_on(users=None):
            if users is None:
                users = ((0, "YVANJ", None),
                         (2, "YVANJ2", "TRKSTB" if library_ready else None))
            for station, user, library in users:
                session = Session(port, name="W%d" % (station + 1)).connect(timeout=25)
                sessions.append(session)
                session.wait_for_text("Connect to workstation", timeout=20)
                session.type_into("Connect to workstation", "0.%d" % station)
                session.press("Enter")
                session.wait_for_text("SIGN ON", timeout=60)
                session.type_into("User ID", user)
                if library:
                    session.type_into("Library", library)
                idle_mark = len(transcript)
                monitor("wait idle 30")
                wait_monitor("wait: guest is idle after", timeout=35, after=idle_mark)
                session.press("Enter")
                session.wait_for_text("Main System/36 help menu", timeout=90)

        def restart_guest():
            nonlocal proc
            for session in sessions:
                session.close()
            sessions.clear()
            monitor("quit")
            proc.wait(timeout=10)
            proc = launch()
            boot()
            sign_on()

        boot()
        sign_on()

        display = sessions[1]
        if not os.environ.get("STARTREK_SKIP_INSTALL"):
            display.type_at(22, 3, "BLDLIBR TRKSTB,5000,,,DISCFILE,TC,,,,REWIND")
            display.press("Enter")
            display.wait_for_text("BLDLIBR procedure is running", timeout=120)
            display.wait_for_text("Main System/36 help menu", timeout=600)
            library_ready = True

        probe = os.environ.get("STARTREK_PROBE_COMMAND")
        probe_menu = os.environ.get("STARTREK_PROBE_MENU")
        if probe_menu:
            for choice in probe_menu.split(","):
                generation = display.generation
                display.type_at(22, 3, choice)
                display.press("Enter")
                display.wait_for_change(timeout=30, since=generation)
                display.settle(quiet=0.5, timeout=10)
            for response in os.environ.get("STARTREK_PROBE_MENU_RESPONSES", "").split("|"):
                if not response:
                    continue
                generation = display.generation
                for field in response.split(","):
                    row, column, value = field.split(":", 2)
                    display.type_at(int(row), int(column), value)
                display.press("Enter")
                display.wait_for_change(timeout=60, since=generation)
                display.settle(quiet=0.5, timeout=10)
            print(display.screen.render("=== STARTREK MENU PROBE ===", fields=True))
            return
        if probe:
            generation = display.generation
            display.type_at(22, 3, probe)
            display.press("Enter")
            deadline = time.monotonic() + 30
            while display.generation == generation and time.monotonic() < deadline:
                time.sleep(0.05)
            display.settle(quiet=0.5, timeout=10)
            time.sleep(1)
            if os.environ.get("STARTREK_PROBE_ATTENTION") == "1":
                display.attention(system_request=True)
                display.wait_for_text("INQUIRY OPTIONS", timeout=60)
                display.settle(quiet=0.5, timeout=10)
            for response in os.environ.get("STARTREK_PROBE_RESPONSES", "").split("|"):
                if not response:
                    continue
                generation = display.generation
                if response == "ENTER":
                    display.press("Enter")
                elif response.startswith("KEY:"):
                    display.press(response[4:])
                else:
                    match = re.fullmatch(r"(\d+):(\d+):(.*)", response)
                    if match:
                        display.type_at(int(match.group(1)), int(match.group(2)),
                                        match.group(3))
                    else:
                        display.type_at(23, 3, response)
                    display.press("Enter")
                display.wait_for_change(timeout=60, since=generation)
                display.settle(quiet=0.5, timeout=10)
                time.sleep(1)
            expected = os.environ.get("STARTREK_PROBE_EXPECT")
            if expected:
                display.wait_for_text(expected, timeout=float(
                    os.environ.get("STARTREK_PROBE_TIMEOUT", "300")))
            print(display.screen.render("=== STARTREK PROBE ===", fields=True))
            return

        def submit(statement, running, timeout):
            mark = len(transcript)
            dismissed_output_queue = False
            display.type_at(22, 3, statement)
            display.press("Enter")
            display.wait_for_text(running, timeout=120)
            deadline = time.monotonic() + timeout
            next_report = time.monotonic() + 30
            while True:
                current_monitor = monitor_text(mark)
                if "CHECK [" in current_monitor or "storage protection" in current_monitor:
                    monitor("show cpu")
                    monitor("tasklist all")
                    monitor("modules active")
                    monitor("mapstate current")
                    time.sleep(0.5)
                    raise AssertionError("%s stopped in the emulator\n%s" %
                                         (statement, monitor_text(mark)[-24000:]))
                with display.lock:
                    returned = display.screen.contains("Main System/36 help menu")
                    output_queue = display.screen.contains("**COMPLETE**")
                    display_image = display.screen.render(
                        "=== %s: %s ===" % (display.name, statement), fields=True)
                if returned:
                    break
                if output_queue and not dismissed_output_queue:
                    dismissed_output_queue = True
                    display.press("Cmd7")
                    continue
                now = time.monotonic()
                if now >= deadline:
                    raise TimeoutError("%s did not return to MAIN\n%s" %
                                       (statement, display_image))
                if now >= next_report:
                    status_mark = len(transcript)
                    monitor("show status")
                    time.sleep(0.2)
                    status = monitor_text(status_mark)
                    print(status, end="", flush=True)
                    if "machine stopped" in status:
                        monitor("show cpu")
                        monitor("tasklist current")
                        monitor("modules active")
                        monitor("modules loaded")
                        monitor("mapstate current")
                        monitor("dis 11E0 32")
                        monitor("addrmap xr1 FFFF read")
                        monitor("addrmap xr2 FFFF read")
                        monitor("dump 70C1D0 96")
                        monitor("dump 00F240 64")
                        monitor("dump 00A000 512")
                        monitor("dis A070 160")
                        monitor("dump 00A980 256")
                        monitor("dis A980 192")
                        monitor("dump 00D800 1024")
                        monitor("dump 10D800 1536")
                        time.sleep(0.2)
                        pb_match = re.search(r"\bpb ([0-9A-Fa-f]{6})\b",
                                             monitor_text(mark))
                        if pb_match:
                            monitor("dump %s 128" % pb_match.group(1))
                        iar_match = re.search(r"SVC [0-9A-Fa-f]{2} at ([0-9A-Fa-f]{4})",
                                              monitor_text(mark))
                        if iar_match:
                            iar = int(iar_match.group(1), 16)
                            monitor("whereis")
                            monitor("dis %04X 96" % max(0, iar - 48))
                        match = re.search(r"IOB ([0-9A-Fa-f]{6})", monitor_text(mark))
                        if match:
                            monitor("dump %s 64" % match.group(1))
                        time.sleep(0.5)
                        raise RuntimeError("machine stopped during %s\n%s" %
                                           (statement, monitor_text(mark)))
                    print(display_image, flush=True)
                    with sessions[0].lock:
                        print(sessions[0].screen.render(
                            "=== W1 DURING %s ===" % statement, fields=True),
                            flush=True)
                    next_report = now + 30
                time.sleep(0.1)
            text = monitor_text(mark)
            if "CHECK [" in text or "storage protection" in text:
                raise AssertionError("%s stopped in the emulator\n%s" %
                                     (statement, text[-12000:]))

        if not os.environ.get("STARTREK_SKIP_INSTALL"):
            submit("FORMAT CREATE,STREKFM,TRKSTB,STREKFM,TRKSTB,22,,HALT,NOPRINT",
                   "FORMAT procedure is running", 600)
            # The disposable volume persists the guest changes, while a fresh
            # emulator/SSP process resets transient scheduler and workstation job
            # state between compilers.
            restart_guest()
            display = sessions[1]
        if not os.environ.get("STARTREK_SKIP_COMPILE"):
            submit("RPGC STREK,TRKSTB,NODSM,CRT,NOXREF,0,NONEP,TRKSTB,,,,"
                   "NOHALT,REPLACE,LINK,NOOBJECT,,GEN,40,,NOMRO",
                   "RPGC procedure is running", 900)
        game_mark = len(transcript)
        display.type_at(22, 3, "STREK")
        display.press("Enter")
        display.wait_for_text("INTER-GALACTIC MEMORANDUM", timeout=120)
        display.wait_for_text("PRESS ENTER TO CONTINUE", timeout=30)
        display.press("Enter")
        display.wait_for_text("STARSHIP ENTERPRISE", timeout=60)
        display.wait_for_text("ENTER YOUR COMMAND", timeout=60)
        with display.lock:
            command = display.screen.field_at(21, 26)
            if command is None or not command.is_input:
                raise AssertionError("game command field is not unprotected")
            if not any(field.protected for field in display.screen.fields):
                raise AssertionError("game display exposed no protected field")

        display.type_at(21, 26, "5")
        display.press("Enter")
        display.wait_for_text("NUMBER OF UNITS FOR THE SHIELDS", timeout=60)
        with display.lock:
            shield = display.screen.field_at(23, 39)
            if shield is None or not shield.is_input or not shield.numeric_only:
                raise AssertionError("shield input is not a numeric input field")
        display.type_at(23, 39, "500")
        display.press("Enter")
        display.wait_for_text("SHIELD ENERGY", timeout=60)
        display.wait_for_text("ENTER YOUR COMMAND", timeout=60)
        # Combat can emit one or more additional invited formats after the
        # shield reply.  KG is the RPG input indicator for Cmd7 on every
        # format; repeat the key at each invitation until the program's LR
        # path returns the workstation job to MAIN.
        for _ in range(6):
            generation = display.generation
            display.press("Cmd7")
            display.wait_for_change(timeout=60, since=generation)
            display.settle(quiet=0.25, timeout=10)
            with display.lock:
                if display.screen.contains("Main System/36 help menu"):
                    break
        else:
            raise TimeoutError("Cmd7 did not terminate STREK\n%s" %
                               display.screen.render(fields=True))
        game_text = monitor_text(game_mark)
        if "CHECK [" in game_text or "storage protection" in game_text:
            raise AssertionError("STREK stopped in the emulator\n%s" %
                                 game_text[-12000:])

        print("PASS: restored, compiled, and played STARTREK through native SSP tools")
    except Exception:
        for session in sessions:
            try:
                print(session.screen.render(
                    "=== %s AT FAILURE ===" % session.name, fields=True),
                    file=sys.stderr)
                print("\n".join(session.trace[-200:]), file=sys.stderr)
            except Exception as diagnostic_error:
                print("unable to render %s: %s" %
                      (getattr(session, "name", "session"), diagnostic_error),
                      file=sys.stderr)
        with changed:
            print("=== SIM36 MONITOR TAIL ===", file=sys.stderr)
            print("".join(transcript[-400:]), file=sys.stderr)
        raise
    finally:
        for session in sessions:
            session.close()
        if proc.poll() is None:
            try:
                monitor("quit")
                proc.wait(timeout=5)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                proc.terminate()
                proc.wait(timeout=5)
        for config in configs:
            try:
                os.unlink(config)
            except OSError:
                pass
        if private_dir:
            shutil.rmtree(private_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
