#!/usr/bin/env python3
"""Boot attended SSP and prove multiplexed W2 reaches a usable menu.

The CLI console supplies the attended IPL identity/date/time; W2 is selected
through the default multiplexer after IPL completion and is then driven through
SIGN ON, MAIN, and menu option 1.
"""

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
    # This is deliberately an explicit research mode, not part of the terminal
    # acceptance contract.  IBM/DW media cannot be distributed with the
    # emulator; the default path below must remain useful with a user-supplied
    # SSP image that has no DisplayWrite product installed.
    dw_research = "--dw36-research" in sys.argv[1:]
    basic_research = "--basic-research" in sys.argv[1:]
    emu = subprocess.Popen(
        sim36env.command(),
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
                                       (text, "".join(transcript[-80:])))
                changed.wait(min(left, 0.25))

    w2 = None
    try:
        command("set machine ipl-type attend")
        # S36_PORT_BASE moves every listener (multiplexer = base, stations
        # 0.1..0.6 = base+1..base+6) so several emulators can run on one host.
        port_base = int(os.environ.get("S36_PORT_BASE", "2300"))
        if port_base != 2300:
            command("set terminal multiplex listen 127.0.0.1:%d" % port_base)
            for station in range(1, 7):
                command("set station 0.%d listen 127.0.0.1:%d" % (station, port_base + station))
        if os.environ.get("S36_IPL_TRACE_CLASSES"):
            # Research knob: trace from the IPL itself (e.g. "csp flow").
            command("trace " + os.environ["S36_IPL_TRACE_CLASSES"])
        mark = len(transcript)
        command("ipl")
        command("wait idle 90")
        wait_monitor("wait: guest is idle after", after=mark)

        command("console put 6 56 YVANJ")
        command("console put 16 56 %s" % os.environ.get("S36_IPL_DATE", "090896"))
        command("console put 17 56 %s" % os.environ.get("S36_IPL_TIME", "120000"))
        if os.environ.get("S36_IPL_TRACE") == "1":
            command("trace ws")
        mark = len(transcript)
        command("console send Enter")
        wait_monitor("SYS-5519 Date or Time changed", after=mark)
        mark = len(transcript)
        command("wait idle 90")
        wait_monitor("wait: guest is idle after", after=mark)

        mark = len(transcript)
        command("console send Enter")
        command("wait idle 90")
        wait_monitor("wait: guest is idle after", after=mark)
        command("dump 08AB 1")
        wait_monitor("0008ab  55", after=mark)

        w2 = Session(port_base, name="W2").connect(timeout=25)
        w2.wait_for_text("Connect to workstation", timeout=20)
        w2.type_into("Connect to workstation", "0.1")
        w2.press("Enter")
        w2.wait_for_text("SIGN ON", timeout=60)
        w2.type_into("User ID", "YVANJ")
        # Let the sign-on display's worker task finish before answering.  The
        # worker issues the SIGN ON Put-with-Invite and then does more work
        # before it terminates and its retained request is handed to the
        # command router; an Enter that lands inside that window is queued on
        # the worker's own complete queue and dies with it (SLIC nupterm
        # c18a49f0 frees a terminating task's complete-queue elements).  A
        # human at a real terminal cannot answer within that window; the
        # headless driver can, so wait for the guest to go idle first.
        mark = len(transcript)
        command("wait idle 30")
        wait_monitor("wait: guest is idle after", timeout=35, after=mark)
        if os.environ.get("S36_SIGNON_TRACE") == "1":
            # Research knob: trace the sign-on job build itself.  The transcript
            # tail is printed if MAIN never appears so a regression in the
            # sign-on termination/initiation chain can be read from the run.
            command("trace " + os.environ.get("S36_SIGNON_TRACE_CLASSES", "csp flow ws"))
        signon_mark = len(transcript)
        w2.press("Enter")
        try:
            w2.wait_for_text("MAIN", timeout=90)
            if os.environ.get("S36_SIGNON_TRACE") == "1":
                full = os.environ.get("S36_SIGNON_TRACE_FULL") == "1"
                print("=== monitor transcript from the %s (MAIN reached) ===" %
                      ("start" if full else "sign-on Enter"), file=sys.stderr)
                print("".join(transcript[0 if full else signon_mark:]), file=sys.stderr)
        except TimeoutError:
            if os.environ.get("S36_SIGNON_TRACE") == "1":
                full = os.environ.get("S36_SIGNON_TRACE_FULL") == "1"
                print("=== monitor transcript from the %s ===" %
                      ("start" if full else "sign-on Enter"), file=sys.stderr)
                print("".join(transcript[0 if full else signon_mark:]), file=sys.stderr)
            raise
        w2.wait_for_text("Main System/36 help menu", timeout=10)
        print(w2.screen.render("=== W2 MAIN ===", fields=True))

        # Developer-only route through the installed SSP programming menus.  Like
        # the DW/36 route below, this is a research harness, not a distributable
        # acceptance dependency on IBM product media.
        if basic_research:
            basic_trace_mark = 0
            for option in ("5", "4", "4", "1"):
                mark = len(transcript)
                command("wait idle 30")
                wait_monitor("wait: guest is idle after", timeout=35, after=mark)
                generation = w2.generation
                w2.type_at(22, 3, option)
                w2.press("Enter")
                try:
                    w2.wait_for_change(timeout=90, since=generation)
                except TimeoutError:
                    print("=== monitor transcript after BASIC route option %s timed out ===" % option,
                          file=sys.stderr)
                    print("".join(transcript[mark:]), file=sys.stderr)
                    raise
                w2.settle(quiet=0.75, timeout=10)
                print(w2.screen.render("=== W2 BASIC ROUTE OPTION %s ===" % option,
                                       fields=True))

            # Start-session parameters are optional.  Submit the displayed
            # defaults as modified fields. This mirrors an interactive client
            # accepting/re-entering them and ensures the research driver sends
            # the values rather than an empty Read-MDT-Fields record.
            #
            # S36_BASIC_COMMAND=1 instead leaves the prompt with Cmd3 and types
            # the procedure command on the SBASIC menu command line.  The
            # headless driver sends blank prompt fields as spaces, which the
            # BASIC procedure rejects with BAS-0118 (parameter 6 must be ANS)
            # on the reference machine as well; the command line gives a clean
            # start.
            if os.environ.get("S36_BASIC_COMMAND") == "1":
                generation = w2.generation
                w2.press("Cmd3")
                w2.wait_for_change(timeout=90, since=generation)
                w2.settle(quiet=0.75, timeout=10)
                print(w2.screen.render("=== W2 BASIC PROMPT LEFT (Cmd3) ===", fields=True))
                mark = len(transcript)
                command("wait idle 30")
                wait_monitor("wait: guest is idle after", timeout=35, after=mark)
                w2.type_at(22, 3, os.environ.get("S36_BASIC_COMMAND_TEXT", "BASIC #LIBRARY,28"))
            else:
                w2.type_into("Name of library", "#LIBRARY")
                w2.type_into("Size of region", "28")
            if os.environ.get("S36_BASIC_TRACE") == "1":
                basic_trace_mark = len(transcript)
                for w in os.environ.get("S36_BASIC_WATCH", "").split(","):
                    w = w.strip()
                    if w:
                        command("watch " + w)
                for member in os.environ.get("S36_BASIC_TRACE_MEMBERS", "").split(","):
                    if member.strip():
                        command("trace member %s" % member.strip())
                command("trace " + os.environ.get("S36_BASIC_TRACE_CLASSES", "csp"))
                time.sleep(0.5)
            generation = w2.generation
            basic_submit_record_mark = len(w2.records)
            w2.press("Enter")
            try:
                w2.wait_for_change(timeout=90, since=generation)
                w2.settle(quiet=0.75, timeout=10)
            except TimeoutError:
                pass
            print(w2.screen.render("=== W2 BASIC AFTER DEFAULTS ===", fields=True))

            # A restored keyboard is not sufficient for a strict TN5250
            # client.  The C1 response wait must also reverse the RFC 1205
            # flow direction with an Invite (opcode 01), otherwise IBM
            # Personal Communications displays BAS-0001 but never transmits
            # Enter.  The research driver historically allowed that input and
            # therefore masked the missing adapter transition.
            basic_records = w2.records[basic_submit_record_mark:]
            if not any(len(record) >= 10 and record[9] == 0x01
                       for record in basic_records):
                raise AssertionError(
                    "BASIC prompt restored the keyboard without an RFC Invite")
            print("BASIC C1 response wait emitted RFC Invite: yes")

            # Let the job initiation settle, then decode the station-to-job
            # association the guest published on W2's TU (TU+60 owning job
            # task, TU+63 JCB) - the condition #CPSP's Cmd1 gate reads.
            direct_statement = os.environ.get("S36_BASIC_STATEMENT_DIRECT", "")
            immediate_statement = direct_statement and os.environ.get(
                "S36_BASIC_STATEMENT_IMMEDIATE") == "1"
            mark = len(transcript)
            if not immediate_statement:
                command("wait idle 120")
            try:
                if not immediate_statement:
                    wait_monitor("wait: guest is idle after", timeout=125, after=mark)
            except TimeoutError:
                # Research aid: if the guest stopped on an unserviced SVC, dump
                # the storage its XR1/XR2 named so the refused request can be
                # read from the run (e.g. the SVC 21 device-allocation element).
                # The stop usually lands while the driver is still waiting
                # for the panel, i.e. before this wait was issued, so scan the
                # whole submit transcript, not just the tail after `mark`.
                tail = "".join(transcript[basic_trace_mark:])
                stops = re.findall(r"SVC not serviced: SVC ([0-9A-F]{2}) at ([0-9A-F]{4})", tail)
                regs = re.findall(r"XR1=([0-9A-F]{6}), XR2=([0-9A-F]{6})", tail)
                m = stops[-1] if stops else None
                if m and regs:
                    xr1, xr2 = regs[-1]
                    for name, value in (("XR1", xr1), ("XR2", xr2)):
                        base = (int(value, 16) & ~0xF) - 0x20
                        if base < 0:
                            base = 0
                        mark2 = len(transcript)
                        command("dump %06X 80" % base)
                        try:
                            wait_monitor("%06x" % (base + 0x70), timeout=10, after=mark2)
                        except TimeoutError:
                            pass
                        print("=== unserviced SVC %s at %s: storage around %s %s ==="
                              % (m[0], m[1], name, value))
                        print("".join(transcript[mark2:]))
                raise
            mark = len(transcript)
            command("tu 00E870")
            wait_monitor("Cmd1", timeout=10, after=mark)
            print("=== W2 TU AFTER SUBMIT ===")
            print("".join(transcript[mark:]))
            # The BASIC panel is already the resumed interactive job.  Drive
            # its input field directly when requested; pressing Cmd1 first is
            # a separate resume-job experiment and changes the AID under test.
            if direct_statement:
                mark = len(transcript)
                if not immediate_statement:
                    command("wait idle 30")
                    wait_monitor("wait: guest is idle after", timeout=35, after=mark)
                if os.environ.get("S36_BASIC_TRACE_BEFORE_STATEMENT") == "1":
                    print("=== BASIC CSP TRACE (launch) ===")
                    print("".join(transcript[basic_trace_mark:mark]))
                print("=== W2 BEFORE DIRECT STATEMENT: invited=%s kbd_unlocked=%s last_cmd=%s ==="
                      % (w2.invited.is_set(), w2.screen.keyboard_unlocked,
                         w2.screen.last_command))
                stmt_member = os.environ.get("S36_BASIC_STATEMENT_MEMBER", "")
                if stmt_member:
                    command("trace member %s" % stmt_member)
                    command("trace isn flow ws")
                elif os.environ.get("S36_BASIC_STATEMENT_ISN") == "1":
                    command("trace member off")
                    command("trace isn flow ws")
                for setup in os.environ.get("S36_BASIC_STATEMENT_SETUP", "").split(";"):
                    setup = setup.strip()
                    if setup:
                        command(setup)
                generation = w2.generation
                w2.type_at(23, 2, direct_statement)
                w2.press("Enter", wait_invite=float(
                    os.environ.get("S36_BASIC_STATEMENT_WAIT", "5")))
                try:
                    w2.wait_for_change(timeout=60, since=generation)
                    w2.settle(quiet=0.75, timeout=10)
                except TimeoutError:
                    pass
                command("wait idle 30")
                time.sleep(1)
                print("=== BASIC CSP TRACE (direct statement) ===")
                print("".join(transcript[mark:]))
                for probe in os.environ.get("S36_BASIC_STATEMENT_PROBE", "").split(";"):
                    probe = probe.strip()
                    if probe:
                        mk = len(transcript)
                        command(probe)
                        time.sleep(0.3)
                        print("=== PROBE: %s ===" % probe)
                        print("".join(transcript[mk:]))
                print(w2.screen.render("=== W2 AFTER DIRECT STATEMENT ===", fields=True))
                statement_tail = "".join(transcript[mark:])
                if ("CHECK [program]" in statement_tail or
                        "SVC not serviced" in statement_tail or
                        "XFER not serviced" in statement_tail):
                    raise AssertionError("BASIC statement stopped on a processor or control-storage check")
                if w2.screen.contains("KBD-0099") or w2.screen.contains("BAS-1100"):
                    raise AssertionError("BASIC statement returned an invalid-key/session error")
                if not w2.invited.is_set():
                    raise AssertionError("BASIC statement did not return to an invited input prompt")

                print("RESEARCH RESULT: BASIC statement was accepted and returned to an invited prompt without a check")
                return 0
            if os.environ.get("S36_BASIC_TRACE") == "1":
                time.sleep(1)
                if os.environ.get("S36_BASIC_TRACE_CMD1") == "1":
                    # Keep tracing through the Cmd1 (Resume job) press so the
                    # command processor's SYS-7212 gate is captured live.
                    generation = w2.generation
                    w2.press("Cmd1")
                    try:
                        w2.wait_for_change(timeout=60, since=generation)
                        w2.settle(quiet=0.75, timeout=10)
                    except TimeoutError:
                        pass
                    command("wait idle 30")
                    time.sleep(1)
                    print("=== BASIC CSP TRACE (through Cmd1) ===")
                    print("".join(transcript[basic_trace_mark:]))
                    print(w2.screen.render("=== W2 BASIC AFTER CMD1 (traced) ===", fields=True))
                    mark = len(transcript)
                    command("tu 00E870")
                    wait_monitor("Cmd1", timeout=10, after=mark)
                    print("=== W2 TU AFTER CMD1 ===")
                    print("".join(transcript[mark:]))
                    statement = os.environ.get("S36_BASIC_STATEMENT", "")
                    if statement:
                        # Research: type one BASIC statement on the BAS-0001
                        # prompt line and press Enter, then show what the
                        # machine did with it (screen, trace, TU decode).
                        mark = len(transcript)
                        command("wait idle 30")
                        try:
                            wait_monitor("wait: guest is idle after", timeout=35, after=mark)
                        except TimeoutError:
                            pass
                        print("=== W2 BEFORE STATEMENT: invited=%s kbd_unlocked=%s last_cmd=%s ==="
                              % (w2.invited.is_set(), w2.screen.keyboard_unlocked,
                                 w2.screen.last_command))
                        stmt_member = os.environ.get("S36_BASIC_STATEMENT_MEMBER", "")
                        if stmt_member:
                            command("trace member %s" % stmt_member)
                            command("trace isn flow")
                        elif os.environ.get("S36_BASIC_STATEMENT_ISN") == "1":
                            command("trace member off")
                            command("trace isn flow ws")
                        mark = len(transcript)
                        generation = w2.generation
                        w2.type_at(23, 2, statement)
                        w2.press("Enter", wait_invite=float(os.environ.get("S36_BASIC_STATEMENT_WAIT", "5")))
                        try:
                            w2.wait_for_change(timeout=60, since=generation)
                            w2.settle(quiet=0.75, timeout=10)
                        except TimeoutError:
                            pass
                        command("wait idle 30")
                        time.sleep(1)
                        print("=== BASIC CSP TRACE (statement) ===")
                        print("".join(transcript[mark:]))
                        for probe in os.environ.get("S36_BASIC_STATEMENT_PROBE", "").split(";"):
                            probe = probe.strip()
                            if probe:
                                mk = len(transcript)
                                command(probe)
                                time.sleep(0.3)
                                print("=== PROBE: %s ===" % probe)
                                print("".join(transcript[mk:]))
                        print(w2.screen.render("=== W2 AFTER STATEMENT ===", fields=True))
                        mark = len(transcript)
                        command("tu 00E870")
                        wait_monitor("Cmd1", timeout=10, after=mark)
                        print("=== W2 TU AFTER STATEMENT ===")
                        print("".join(transcript[mark:]))
                    return 0
                print("=== BASIC CSP TRACE ===")
                print("".join(transcript[basic_trace_mark:]))
                return 0
            generation = w2.generation
            w2.press("Cmd1")
            try:
                w2.wait_for_change(timeout=120, since=generation)
                w2.settle(quiet=0.75, timeout=10)
            except TimeoutError:
                pass
            command("show status")
            time.sleep(1)
            monitor = "".join(transcript)
            print(w2.screen.render("=== W2 BASIC AFTER CMD1 ===", fields=True))
            print("=== MONITOR TAIL ===")
            print("".join(transcript[-80:]))
            if "SVC not serviced" in monitor or "CHECK [program]" in monitor:
                raise AssertionError("BASIC launch stopped on a processor check")
            print("RESEARCH RESULT: BASIC start-session defaults and Cmd1 resume were "
                  "accepted without a processor check")
            return 0

        # Optional focused reproducer for the DisplayWrite path. Keep this out
        # of the compact default suite: it is a developer diagnostic that drives
        # the same real 5250 records as the ordinary MAIN proof, then prints every
        # resulting panel and the monitor tail at the former SVC-35 stop.
        if dw_research:
            application_response_seen = False
            # Optional physical-storage probe for research images.  Keep the
            # address supplied by the investigator: no product layout or DW
            # storage address is baked into the reusable harness.
            dw_watches = os.environ.get("S36_DW_WATCHES",
                                        os.environ.get("S36_DW_WATCH", ""))
            for dw_watch in (w.strip() for w in dw_watches.split(",")):
                if dw_watch:
                    command("watch " + dw_watch)
            if os.environ.get("S36_DW_TRACE") == "1":
                trace_mark = len(transcript)
                command("trace " + os.environ.get(
                    "S36_DW_TRACE_CLASSES", "ws output csp isn disk"))
                command("trace workstation W2 lifecycle on 2048")
                command("trace member %s" % os.environ.get("S36_DW_TRACE_MEMBER", "WDDG"))
                wait_monitor("workstation 0.1 lifecycle trace ON", timeout=10,
                             after=trace_mark)
            research_break = os.environ.get("S36_DW_BREAK", "").strip()
            if research_break:
                command("breakm " + research_break)
            for option, title in (("9", "OFFICE PRODUCTS"),
                                  ("1", "DISPLAYWRITE/36"),
                                  ("1", "WORK WITH DOCUMENTS")):
                mark = len(transcript)
                command("wait idle 30")
                wait_monitor("wait: guest is idle after", timeout=35, after=mark)
                generation = w2.generation
                try:
                    w2.type_at(22, 3, option)
                    w2.press("Enter")
                    if title == "WORK WITH DOCUMENTS" and research_break:
                        command("wait 5")
                        wait_monitor("s elapsed", timeout=10, after=mark)
                        command("show cpu")
                        wait_monitor("reason: breakpoint at", timeout=10,
                                     after=mark)
                        break_steps = int(os.environ.get("S36_DW_BREAK_STEPS", "0"))
                        if break_steps:
                            command("step %d" % break_steps)
                        probe_real = os.environ.get("S36_DW_PROBE_REAL", "").strip()
                        if probe_real:
                            matches = [probe_real]
                        else:
                            probe_logical = os.environ.get("S36_DW_PROBE_LOGICAL", "3B00")
                            command("addrmap direct %s read" % probe_logical)
                            wait_monitor("resolves to real", timeout=10, after=mark)
                            with changed:
                                probe_text = "".join(transcript[mark:])
                            matches = re.findall(r"resolves to real ([0-9A-F]{6})",
                                                 probe_text)
                            if not matches:
                                raise AssertionError("addrmap did not report a real address")
                        dump_mark = len(transcript)
                        command("dump %s 100" % matches[-1])
                        wait_monitor(matches[-1].lower(), timeout=10,
                                     after=dump_mark)
                        print("".join(transcript[mark:]), file=sys.stderr)
                        return 0
                    w2.wait_for_change(timeout=90, since=generation)
                    w2.settle(quiet=0.75, timeout=10)
                    if title == "WORK WITH DOCUMENTS":
                        w2.wait_for_text("WORK WITH DOCUMENTS", timeout=90)
                        application_response_seen = True
                        w2.settle(quiet=0.75, timeout=10)
                except TimeoutError:
                    print("=== MONITOR TAIL AT %s ===" % title, file=sys.stderr)
                    if title == "WORK WITH DOCUMENTS" and os.environ.get("S36_DW_TRACE") == "1":
                        print("".join(transcript[trace_mark:]), file=sys.stderr)
                    else:
                        print("".join(transcript[-120:]), file=sys.stderr)
                    if os.environ.get("S36_PANIC_ON_ROUTE_FAILURE") == "1":
                        mark = len(transcript)
                        command("panic")
                        command("DisplayWrite route stopped while entering %s" % title)
                        command("Attended IPL, sign on W2, MAIN 9, OFCPROD 1, TEXT 1")
                        wait_monitor("panic dump created:", timeout=30, after=mark)
                        print("".join(transcript[mark:]), file=sys.stderr)
                    raise
                print(w2.screen.render("=== W2 %s ===" % title, fields=True))
            kbd_seen = w2.screen.find("KBD-0099")
            if w2.screen.find("TXT-0051"):
                # TXT-0051 explicitly offers options 2 and 3 in the input field
                # on either the full additional-information panel or the compact
                # Input-Output form. Drive its recovery/initialization branch further;
                # retaining kbd_seen ensures a later repaint cannot hide the
                # controller defect that produced KBD-0099 first.
                generation = w2.generation
                try:
                    w2.type_at(22, 13, "3")
                except LookupError:
                    # Some SSP paths present the response line as the first
                    # writable field on rows 22-24 rather than the one-byte
                    # Option field of the expanded-help panel.
                    response = next((f for f in w2.screen.fields
                                     if not f.bypass and f.row >= 22), None)
                    if response is None:
                        raise
                    w2.type_at(response.row, response.col, "3")
                w2.press("Enter")
                try:
                    w2.wait_for_change(timeout=90, since=generation)
                    w2.settle(quiet=0.75, timeout=10)
                except TimeoutError:
                    pass
                print(w2.screen.render("=== W2 TXT-0051 OPTION 3 ===", fields=True))
            command("show status")
            if os.environ.get("S36_DW_TRACE") == "1":
                command("trace workstation W2 lifecycle show")
            time.sleep(1)
            monitor = "".join(transcript)
            print("=== MONITOR TAIL ===")
            print("".join(transcript[trace_mark:] if os.environ.get("S36_DW_TRACE") == "1"
                          else transcript[-120:]))
            if "SVC not serviced" in monitor or "CHECK [program]" in monitor:
                if os.environ.get("S36_PANIC_ON_ROUTE_FAILURE") == "1":
                    mark = len(transcript)
                    command("panic")
                    command("DisplayWrite research route stopped on a processor check")
                    command("Attended IPL, W2 sign-on, MAIN 9, OFCPROD 1, TEXT 1, TXT-0051 option 3")
                    wait_monitor("panic dump created:", timeout=30, after=mark)
                    print("".join(transcript[mark:]), file=sys.stderr)
                raise AssertionError("DisplayWrite stopped on a processor check")
            if kbd_seen or w2.screen.find("KBD-0099"):
                if os.environ.get("S36_PANIC_ON_ROUTE_FAILURE") == "1":
                    mark = len(transcript)
                    command("panic")
                    command("DisplayWrite emitted KBD-0099 after Work with documents")
                    command("Attended IPL, sign on W2, MAIN 9, OFCPROD 1, TEXT 1")
                    wait_monitor("panic dump created:", timeout=30, after=mark)
                    print("".join(transcript[mark:]), file=sys.stderr)
                raise AssertionError("DisplayWrite left the keyboard in KBD-0099 error state")
            if not application_response_seen:
                raise AssertionError("DisplayWrite did not produce its application response")
            print("RESEARCH RESULT: optional DW/36 route reached its application "
                  "screen without a processor check or keyboard error")
            return 0

        mark = len(transcript)
        command("wait idle 30")
        wait_monitor("wait: guest is idle after", timeout=35, after=mark)
        generation = w2.generation
        if os.environ.get("S36_IPL_TRACE") == "1":
            command("trace ws csp")
        w2.type_at(22, 3, "1")
        w2.press("Enter")
        try:
            w2.wait_for_text("MENU COMMAND", timeout=90)
        except TimeoutError:
            print(w2.screen.render("=== W2 AFTER OPTION 1 TIMEOUT ===", fields=True),
                  file=sys.stderr)
            print("=== monitor tail after option 1 timeout ===", file=sys.stderr)
            print("".join(transcript[-240:]), file=sys.stderr)
            raise
        w2.wait_for_change(timeout=10, since=generation)
        w2.settle(quiet=0.5, timeout=10)
        result = w2.screen.render("=== W2 MENU COMMAND ===", fields=True)
        print(result)
        if os.environ.get("S36_IPL_TRACE") == "1":
            print("=== W2 RECORD TRACE ===")
            print("\n".join(w2.trace[-160:]))
            for number, record in list(enumerate(w2.records))[-20:]:
                print("record %d opcode=%02X body=%s" %
                      (number, record[9], record[10:].hex()))
        if w2.screen.find("Main System/36 help menu"):
            raise AssertionError("MAIN accepted option 1 but did not change panels")
        print("PASS: attended IPL completed; W2 reached MAIN and navigated option 1 "
              "to MENU COMMAND")
        return 0
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
            print("".join(transcript[-100:]), file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main())
