#!/usr/bin/env python3
"""Boot unattended SSP and prove multiplexed W2 reaches a usable program.

The client selects station 0.1 through the default multiplexer on port 2300. It
never stimulates the unattended CLI console: the SSP/native workstation
lifecycle must complete IPL and arm sign-on by itself.
"""

import os
import re
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
    dw_research = "--dw36-research" in sys.argv[1:]
    cnfig_research = "--cnfigssp-research" in sys.argv[1:]
    cnfig_apply_research = "--cnfigssp-apply-research" in sys.argv[1:]
    config = os.environ.get("S36_CONFIG",
                            sim36env.default_config())
    temporary_config = None
    research_disk = os.environ.get("S36_RESEARCH_DISK", "").strip()
    if research_disk:
        research_disk_mode = os.environ.get("S36_RESEARCH_DISK_MODE", "overlay")
        if research_disk_mode not in ("overlay", "rw"):
            raise ValueError("S36_RESEARCH_DISK_MODE must be overlay or rw")
        with open(config) as source:
            definition = source.read()
        definition, replacements = re.subn(
            r"^attach disk0 .*$",
            "attach disk0 %s %s" % (os.path.abspath(research_disk),
                                     research_disk_mode),
            definition, count=1, flags=re.MULTILINE)
        if replacements != 1:
            raise AssertionError("research configuration has no disk0 attachment")
        temporary_config = tempfile.NamedTemporaryFile(
            mode="w", prefix="sim36-dw-", suffix=".sim", delete=False)
        temporary_config.write(definition)
        temporary_config.close()
        config = temporary_config.name
    emu = subprocess.Popen(
        sim36env.command(config),
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, bufsize=1)
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
                                       (text, "".join(transcript[-120:])))
                changed.wait(min(left, 0.25))

    w2 = None
    try:
        if dw_research and os.environ.get("S36_DW_TRACE_EARLY") == "1":
            command("trace " + os.environ.get(
                "S36_DW_TRACE_CLASSES", "ws output csp isn disk"))
            command("trace member %s" %
                    os.environ.get("S36_DW_TRACE_MEMBER", "WDDG"))
        command("set machine ipl-type unattend")
        # S36_PORT_BASE moves every listener (multiplexer = base, stations
        # 0.1..0.6 = base+1..base+6) so several emulators can run on one host.
        port_base = int(os.environ.get("S36_PORT_BASE", "2300"))
        if port_base != 2300:
            command("set terminal multiplex listen 127.0.0.1:%d" % port_base)
            for station in range(1, 7):
                command("set station 0.%d listen 127.0.0.1:%d" % (station, port_base + station))
        mark = len(transcript)
        command("ipl")
        command("wait idle 90")
        wait_monitor("wait: guest is idle after", after=mark)
        command("dump 08AB 1")
        initial_08ab = os.environ.get("S36_INITIAL_08AB", "55").lower()
        wait_monitor("0008ab  %s" % initial_08ab, after=mark)

        if os.environ.get("S36_ATTACH_TRACE") == "1":
            command("trace " + os.environ.get(
                "S36_ATTACH_TRACE_CLASSES", "ws csp isn"))
            command("trace member " + os.environ.get(
                "S36_ATTACH_TRACE_MEMBER", "CPTS"))

        station_name = os.environ.get("S36_STATION", "0.1")
        w2 = Session(port_base, name=station_name).connect(timeout=25)
        w2.wait_for_text("Connect to workstation", timeout=20)
        w2.type_into("Connect to workstation", station_name)
        w2.press("Enter")
        progress_aid = os.environ.get("S36_IPL_PROGRESS_AID", "").strip()
        if progress_aid:
            w2.wait_for_text("IPL is in progress", timeout=60)
            w2.press(progress_aid)
        w2.wait_for_text("SIGN ON", timeout=60)
        # The cold unattended machine's pre-transfer idle is 08AB=1D. The
        # negotiated action-0 bind and guest sign-on setup, not elapsed host
        # time, advance it to the Slonky booted-idle value 55.
        state_mark = len(transcript)
        command("wait idle 30")
        wait_monitor("wait: guest is idle after", timeout=35, after=state_mark)
        command("dump 08AB 1")
        active_08ab = os.environ.get("S36_ACTIVE_08AB", "55").lower()
        wait_monitor("0008ab  %s" % active_08ab, timeout=10,
                     after=state_mark)
        w2.type_into("User ID", "YVANJ")
        if (cnfig_research or cnfig_apply_research) and station_name == "0.0":
            w2.type_into("Date", os.environ.get("S36_IPL_DATE", "090896"))
            w2.type_into("Time", os.environ.get("S36_IPL_TIME", "120000"))
        signon_mark = len(transcript)
        w2.press("Enter")
        if (cnfig_research or cnfig_apply_research) and station_name == "0.0":
            # This configured image uses the operator sign-on panel and asks
            # for acknowledgement when its saved date needs changing.
            w2.settle(quiet=0.5, timeout=5)
            if w2.screen.find("SYS-5519"):
                w2.press("Enter")
        try:
            w2.wait_for_text("MAIN", timeout=90)
        except TimeoutError:
            if os.environ.get("S36_ATTACH_TRACE") == "1":
                print("=== monitor transcript from the sign-on Enter ===", file=sys.stderr)
                print("".join(transcript[signon_mark:]), file=sys.stderr)
                print("=== W2 record trace ===", file=sys.stderr)
                print("\n".join(w2.trace[-40:]), file=sys.stderr)
            raise
        w2.wait_for_text("Main System/36 help menu", timeout=10)

        if cnfig_research or cnfig_apply_research:
            # Private SSP-media regression for CNFIGSSP's device-code pages.
            # The final Cmd5 used to expose a deferred-input/work-space
            # high-water mismatch as a level-5 storage-protection check in
            # #WDDG.
            def submit(value, expected):
                idle_mark = len(transcript)
                command("wait idle 30")
                wait_monitor("wait: guest is idle after", timeout=35,
                             after=idle_mark)
                generation = w2.generation
                if value is not None:
                    if w2.screen.find("Main System/36 help menu"):
                        w2.type_at(22, 3, value)
                    else:
                        w2.type_into("Option", value)
                w2.press("Enter")
                w2.wait_for_text(expected, timeout=90)
                w2.wait_for_change(timeout=10, since=generation)
                w2.settle(quiet=0.5, timeout=10)
                print(w2.screen.render("=== CNFIGSSP %s ===" % expected,
                                       fields=True))

            command("trace csp")
            submit("CNFIGSSP", "CONFIGURATION")
            if cnfig_apply_research:
                submit("12", "CONFIGURATION MEMBER DEFINITION")
                apply_member = os.environ.get("S36_CNFIG_APPLY_MEMBER", "").strip()
                if apply_member:
                    idle_mark = len(transcript)
                    command("wait idle 30")
                    wait_monitor("wait: guest is idle after", timeout=35,
                                 after=idle_mark)
                    w2.type_into("Member name", apply_member)
                    w2.type_into("Library name", os.environ.get(
                        "S36_CNFIG_APPLY_LIBRARY", "#CNFGLIB"))
                    w2.press("Enter")
                    w2.wait_for_text("CHANGE MASTER CONFIGURATION", timeout=90)
                    w2.settle(quiet=0.5, timeout=10)
                    print(w2.screen.render(
                        "=== CNFIGSSP CHANGE MASTER CONFIGURATION ===",
                        fields=True))
                    if w2.screen.find("System does not support more than"):
                        raise AssertionError(
                            "CNFIGSSP rejected the member against the advertised hardware")
                    print("PASS: CNFIGSSP accepted %s in %s for master configuration" %
                          (apply_member, os.environ.get(
                              "S36_CNFIG_APPLY_LIBRARY", "#CNFGLIB")))
                    return 0
                print("PASS: CNFIGSSP option 12 reached member selection without looping in #CIRN")
                return 0
            submit("3", "CONFIGURATION MEMBER DEFINITION")
            submit("5", "CONFIGURATION MEMBER DESCRIPTION")
            submit(None, "CONFIGURATION MEMBER")
            submit("1", "DISPLAY STATION")
            submit("1", "PRINTER DEFINITION")
            submit(None, "WORK STATION DEFINITION")
            for page in range(16):
                generation = w2.generation
                w2.press("Cmd5")
                w2.wait_for_change(timeout=90, since=generation)
                w2.settle(quiet=0.3, timeout=5)
                if any("CHECK [program]" in line for line in transcript):
                    raise AssertionError(
                        "CNFIGSSP device-code page %d caused a processor check" %
                        (page + 1))
            print(w2.screen.render("=== W2 CNFIGSSP DEVICE CODES ===",
                                   fields=True))
            print("PASS: CNFIGSSP cycled all device-code pages without a processor check")
            return 0

        if dw_research:
            # Private research oracle only. DisplayWrite media is not
            # distributable and is never part of the default acceptance path.
            mark = (0 if os.environ.get("S36_DW_TRACE_EARLY") == "1"
                    else len(transcript))
            if (os.environ.get("S36_DW_TRACE") == "1" and
                    os.environ.get("S36_DW_TRACE_EARLY") != "1"):
                command("trace " + os.environ.get(
                    "S36_DW_TRACE_CLASSES", "ws output csp isn disk"))
                command("trace member %s" %
                        os.environ.get("S36_DW_TRACE_MEMBER", "WDDG"))
            # Optional private probes for reverse engineering against the local
            # licensed image.  Keep addresses outside the checked-in harness:
            # task/workspace allocation varies between SSP levels.
            for probe in os.environ.get("S36_DW_WATCHES", "").split(","):
                probe = probe.strip()
                if probe:
                    command("watch " + probe)
            # Resolve application addresses while the signed-on W2 task owns the
            # current address space, then arm physical-storage watches before the
            # procedure route runs. This catches indexed writers which static
            # operand scans cannot attribute.
            for logical in os.environ.get("S36_DW_WATCH_LOGICAL", "").split(","):
                logical = logical.strip()
                if not logical:
                    continue
                map_mark = len(transcript)
                command("addrmap direct %s read" % logical)
                wait_monitor("resolves to real", timeout=10, after=map_mark)
                with changed:
                    map_text = "".join(transcript[map_mark:])
                mapped = re.findall(r"resolves to real ([0-9A-F]{6})", map_text)
                if not mapped:
                    raise AssertionError("addrmap did not resolve logical %s" % logical)
                command("watch %s" % mapped[-1])
            research_break = os.environ.get("S36_DW_BREAK", "").strip()
            research_break_raw = os.environ.get("S36_DW_BREAK_RAW", "").strip()
            try:
                # Enter through the shipped procedures.  Invoking TEXTDOC as a
                # MAIN command bypasses OFCPROD/TEXT setup and is not equivalent
                # to the operator route used on the reference machine.
                for option, title in (("9", "OFFICE PRODUCTS"),
                                      ("1", "DISPLAYWRITE/36"),
                                      ("1", "WORK WITH DOCUMENTS")):
                    # Arm application breakpoints only for the final route. A
                    # shared system member such as #HFPU may also render the two
                    # menus, in which case arming it above the loop stops the
                    # wrong invocation and prevents the client from advancing.
                    if title == "WORK WITH DOCUMENTS" and (research_break or
                                                            research_break_raw):
                        if research_break_raw:
                            raw = research_break_raw.split(None, 1)
                            command("break " + raw[0])
                            if len(raw) == 2:
                                command("break member " + raw[1])
                        else:
                            command("breakm " + research_break)
                    idle_mark = len(transcript)
                    command("wait idle 30")
                    wait_monitor("wait: guest is idle after", timeout=35,
                                 after=idle_mark)
                    generation = w2.generation
                    w2.type_at(22, 3, option)
                    w2.press("Enter")
                    if title == "WORK WITH DOCUMENTS" and (research_break or
                                                            research_break_raw):
                        command("wait 5")
                        wait_monitor("s elapsed", timeout=10, after=mark)
                        command("show cpu")
                        wait_monitor("reason: breakpoint at", timeout=10,
                                     after=mark)
                        for _ in range(int(os.environ.get(
                                "S36_DW_BREAK_SKIP", "0"))):
                            skip_mark = len(transcript)
                            command("step 1")
                            command("start")
                            command("wait 10")
                            wait_monitor("10s elapsed", timeout=15,
                                         after=skip_mark)
                            command("show cpu")
                            wait_monitor("reason: breakpoint at", timeout=10,
                                         after=skip_mark)
                        break_watch = os.environ.get(
                            "S36_DW_BREAK_WATCH_LOGICAL", "").strip()
                        if break_watch:
                            map_mark = len(transcript)
                            command("addrmap direct %s read" % break_watch)
                            wait_monitor("resolves to real", timeout=10,
                                         after=map_mark)
                            with changed:
                                map_text = "".join(transcript[map_mark:])
                            mapped = re.findall(
                                r"resolves to real ([0-9A-F]{6})", map_text)
                            if not mapped:
                                raise AssertionError(
                                    "addrmap did not resolve logical %s" % break_watch)
                            command("watch %s" % mapped[-1])
                            command("break clear")
                            command("start")
                            # Continue through the ordinary panel-change checks;
                            # the watch remains armed across subsequent members.
                            w2.wait_for_change(timeout=float(
                                os.environ.get("S36_DW_TIMEOUT", "90")),
                                since=generation)
                            w2.settle(quiet=0.75, timeout=10)
                            print(w2.screen.render("=== W2 %s ===" % title,
                                                   fields=True))
                            continue
                        break_steps = int(os.environ.get("S36_DW_BREAK_STEPS", "0"))
                        if break_steps:
                            command("step %d" % break_steps)
                        probe_real = os.environ.get("S36_DW_PROBE_REAL", "").strip()
                        if probe_real:
                            matches = [probe_real]
                        else:
                            probe_logical = os.environ.get("S36_DW_PROBE_LOGICAL", "3B00")
                            probe_kind = os.environ.get("S36_DW_PROBE_KIND", "direct")
                            command("addrmap %s %s read" %
                                    (probe_kind, probe_logical))
                            wait_monitor(" real ", timeout=10, after=mark)
                            with changed:
                                probe_text = "".join(transcript[mark:])
                            matches = re.findall(
                                r"(?:resolves to real|-> real) ([0-9A-F]{6})",
                                probe_text)
                            if not matches:
                                raise AssertionError("addrmap did not report a real address")
                        dump_mark = len(transcript)
                        probe_length = os.environ.get(
                            "S36_DW_PROBE_LENGTH", "100")
                        command("dump %s %s" % (matches[-1], probe_length))
                        command("show cpu")
                        wait_monitor("%s" % matches[-1].lower(), timeout=10,
                                     after=dump_mark)
                        wait_monitor("stopped=True", timeout=10,
                                     after=dump_mark)
                        print("".join(transcript[mark:]), file=sys.stderr)
                        return 0
                    w2.wait_for_change(timeout=float(
                        os.environ.get("S36_DW_TIMEOUT", "90")),
                        since=generation)
                    w2.settle(quiet=0.75, timeout=10)
                    if title != "WORK WITH DOCUMENTS":
                        w2.wait_for_text(title, timeout=10)
                    print(w2.screen.render("=== W2 %s ===" % title,
                                           fields=True))
            except (TimeoutError, LookupError):
                if os.environ.get("S36_PANIC_ON_ROUTE_FAILURE") == "1":
                    panic_mark = len(transcript)
                    command("panic")
                    command("Menu route did not reach Work with Documents")
                    command("Unattended IPL, W2 sign-on, MAIN 9, OFCPROD 1, TEXT 1")
                    wait_monitor("panic dump created:", timeout=30, after=panic_mark)
                    print("".join(transcript[panic_mark:]), file=sys.stderr)
                if os.environ.get("S36_DW_TRACE") == "1":
                    print("".join(transcript[mark:]), file=sys.stderr)
                raise
            w2.settle(quiet=0.75, timeout=10)
            dw_response = os.environ.get("S36_DW_RESPONSE", "").strip()
            if dw_response:
                response_generation = w2.generation
                # TXT-0051 is an SSP inquiry with a real one-character Option
                # field. Earlier terminal defects lost this field and produced
                # KBD-0099; drive the guest's offered 2/3 response here.
                w2.type_at(22, 13, dw_response)
                w2.press("Enter")
                w2.wait_for_change(timeout=float(
                    os.environ.get("S36_DW_TIMEOUT", "90")),
                    since=response_generation)
                response_idle = len(transcript)
                command("wait idle 120")
                wait_monitor("wait: guest is idle after", timeout=125,
                             after=response_idle)
                w2.settle(quiet=0.75, timeout=10)
                print(w2.screen.render(
                    "=== W2 WORK WITH DOCUMENTS RESPONSE %s ===" % dw_response,
                    fields=True))
                if os.environ.get("S36_DW_RETRY_AFTER_RESPONSE") == "1":
                    retry_generation = w2.generation
                    w2.type_at(22, 3, "1")
                    w2.press("Enter")
                    w2.wait_for_change(timeout=float(
                        os.environ.get("S36_DW_TIMEOUT", "90")),
                        since=retry_generation)
                    retry_idle = len(transcript)
                    command("wait idle 120")
                    wait_monitor("wait: guest is idle after", timeout=125,
                                 after=retry_idle)
                    w2.settle(quiet=0.75, timeout=10)
                    print(w2.screen.render(
                        "=== W2 WORK WITH DOCUMENTS RETRY ===", fields=True))
            if w2.screen.find("KBD-0099"):
                raise AssertionError("DisplayWrite emitted KBD-0099")
            if w2.screen.find("SYS-1887"):
                if os.environ.get("S36_DW_TRACE") == "1":
                    print("".join(transcript[mark:]), file=sys.stderr)
                if os.environ.get("S36_PANIC_ON_ROUTE_FAILURE") == "1":
                    mark = len(transcript)
                    command("panic")
                    command("DW/36 Work with Documents reported SYS-1887 No task dump taken")
                    command("Unattended IPL, W2 sign-on, MAIN 9, OFCPROD 1, TEXT 1")
                    wait_monitor("panic dump created:", timeout=30, after=mark)
                    print("".join(transcript[mark:]), file=sys.stderr)
                raise AssertionError("DisplayWrite task failed with SYS-1887")
            if os.environ.get("S36_DW_TRACE") == "1":
                print("".join(transcript[mark:]), file=sys.stderr)
            if os.environ.get("S36_DW_PANIC") == "1":
                panic_mark = len(transcript)
                command("panic")
                command("DW/36 stopped at its post-IPL table-initialization response")
                command("Unattended IPL, W2 sign-on, MAIN 9, OFCPROD 1, TEXT 1")
                wait_monitor("panic dump created:", timeout=30, after=panic_mark)
                print("".join(transcript[panic_mark:]), file=sys.stderr)
                return 0
            print("RESEARCH RESULT: unattended IPL reached the private DW/36 "
                  "Work with Documents program panel without a keyboard or processor error")
            return 0

        mark = len(transcript)
        command("wait idle 30")
        wait_monitor("wait: guest is idle after", timeout=35, after=mark)
        generation = w2.generation
        # Keep the workstation trace on for the first MAIN read: this is the
        # regression for #WDDG's non-zero, guest-selected work-page allocation.
        # It caught the decimal-43 versus hexadecimal-0x43 field-offset bug.
        command("trace ws")
        w2.type_at(22, 3, "1")
        w2.press("Enter")
        w2.wait_for_text("MENU COMMAND", timeout=90)
        w2.wait_for_text("Display a user menu", timeout=10)
        w2.wait_for_change(timeout=10, since=generation)
        # Reference drift: this check was written against a workspace pointer
        # of 800040 (see the reference's session notes); the reference emulator
        # itself now reports 800640 for the first MAIN read on this volume and
        # fails its own check.  SIM/36 prints the same line as the reference.
        wait_monitor("workspace 800640 retains logical block displacement 0640;",
                     timeout=10, after=mark)
        if w2.screen.find("Main System/36 help menu"):
            raise AssertionError("MAIN accepted option 1 but did not change panels")
        print(w2.screen.render("=== W2 MENU COMMAND ===", fields=True))

        # Return through the guest's Save/Restore Screen path, then enter the
        # programming branch of the real MAIN menu.
        w2.press("Cmd3")
        w2.wait_for_text("Main System/36 help menu", timeout=90)
        w2.type_at(22, 3, "5")
        w2.press("Enter")
        w2.wait_for_text("PROGRAM", timeout=90)
        w2.settle(quiet=0.5, timeout=10)
        print(w2.screen.render("=== W2 PROGRAM MENU ===", fields=True))

        # PROGRAM option 3 invokes SEU, a real SSP utility rather than another
        # help-menu description. Reaching its panel proves command acceptance,
        # procedure dispatch, program loading, and interactive program output.
        w2.type_at(22, 3, "3")
        w2.press("Enter")
        w2.wait_for_text("SEU", timeout=90)
        w2.settle(quiet=0.5, timeout=10)
        print(w2.screen.render("=== W2 SEU ===", fields=True))
        print("PASS: unattended IPL completed; W2 navigated MAIN -> MENU COMMAND "
              "-> MAIN -> PROGRAM and started SEU")
        return 0
    except Exception:
        print("=== monitor tail ===", file=sys.stderr)
        print("".join(transcript if (os.environ.get("S36_DW_TRACE_EARLY") == "1" or
                                      os.environ.get("S36_ATTACH_TRACE") == "1")
                      else transcript[-240:]), file=sys.stderr)
        raise
    finally:
        if w2 is not None:
            w2.close()
        if emu.poll() is None:
            command("stop")
            command("quit")
            try:
                emu.wait(timeout=15)
            except subprocess.TimeoutExpired:
                emu.kill()
        if emu.returncode not in (None, 0):
            print("=== monitor tail ===", file=sys.stderr)
            print("".join(transcript[-120:]), file=sys.stderr)
        if temporary_config is not None:
            os.unlink(temporary_config.name)


if __name__ == "__main__":
    sys.exit(main())
