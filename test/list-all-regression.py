#!/usr/bin/env python3
"""Drive a MAIN command to a screen or trace milestone and reject failures."""

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
    template = os.environ.get("S36_LIST_CONFIG", "default-machine.sim.in")
    config = sim36env.default_config(template, attach_mode="overlay")
    proc = subprocess.Popen(sim36env.command(config), cwd=ROOT,
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

    sessions = {}
    try:
        command("set machine ipl-type unattend")
        trace_spec = os.environ.get("S36_LIST_TRACE_SPEC", "")
        if trace_spec:
            command("trace " + trace_spec)
        elif os.environ.get("S36_LIST_TRACE") == "1":
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
        station_numbers = [int(value) for value in
                           os.environ.get("S36_LIST_STATIONS", "0,1").split(",")]
        for station, user in zip(station_numbers, ("YVANJ", "YVANJ2")):
            workstation = "W%d" % (station + 1)
            session = Session(port_base, name=workstation).connect(timeout=25)
            sessions[workstation] = session
            session.wait_for_text("Connect to workstation", timeout=20)
            session.type_into("Connect to workstation", "0.%d" % station)
            session.press("Enter")
            session.wait_for_text("SIGN ON", timeout=60)
            session.type_into("User ID", user)
            mark = len(transcript)
            command("wait idle 30")
            wait_monitor("wait: guest is idle after", timeout=35, after=mark)
            session.press("Enter")
            session.wait_for_text("Main System/36 help menu", timeout=90)

        target = os.environ.get("S36_LIST_STATION", "W2").upper()
        session = sessions[target]

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
        if os.environ.get("S36_LIST_CATALOG_MENU") == "1":
            statement = "MAIN 2 -> SYSSESN 5 -> LIBRFILE 1 -> LIBRARY 3 -> LIBRLIST 3"
            check_mark = len(transcript)
            for choice, panel in (("2", "Perform general system activities"),
                                  ("5", "Work with files, libraries, or folders"),
                                  ("1", "Work with libraries"),
                                  ("3", "List library information"),
                                  ("3", "Name of entry to be listed")):
                session.type_at(22, 3, choice)
                generation = session.generation
                session.press("Enter")
                deadline = time.monotonic() + 30
                while session.generation == generation and time.monotonic() < deadline:
                    time.sleep(0.05)
                session.settle(quiet=0.2, timeout=5)
                session.wait_for_text(panel, timeout=30)
        else:
            session.type_at(22, 3, statement)
            check_mark = len(transcript)
            session.press("Enter")

        deadline = time.monotonic() + float(os.environ.get("S36_LIST_TIMEOUT", "120"))
        saw_listing = False
        while time.monotonic() < deadline:
            with session.lock:
                saw_listing |= bool(expected_screen) and session.screen.contains(expected_screen)
                saw_rejected = any(session.screen.contains(value) for value in rejected)
            with changed:
                text = "".join(transcript[check_mark:])
            saw_listing |= bool(expected_monitor) and expected_monitor in text
            if ("CHECK [" in text or "storage protection" in text or saw_rejected or
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
                if ("CHECK [" in text or "storage protection" in text or saw_rejected or
                        any(value in text for value in rejected)):
                    print(session.screen.render("=== LIST ALL ===", fields=True), file=sys.stderr)
                    with changed:
                        lines = transcript[:] if os.environ.get("S36_LIST_TRACE_FULL") == "1" else transcript[-160:]
                    print("".join(lines), file=sys.stderr)
                    raise AssertionError("LIST ALL stopped after displaying source")
                break
            time.sleep(0.05)
        if not saw_listing:
            with changed:
                tail = "".join(transcript[-160:])
            raise TimeoutError("command did not reach expected output %r\n%s\n%s" %
                               (expected_screen, session.screen.render(fields=True), tail))

        # CNFIGSSP's master-configuration print is deliberately driven from
        # W1 while W3 is also signed on.  This is a controller-correlation
        # regression: an accepted W1 AID must not complete (or be routed
        # through) W3's retained PUT-with-invite.  Reaching PRINT MENU alone
        # is insufficient; the second Enter must start and drain the printer
        # job.
        if os.environ.get("S36_LIST_CNFIG_PRINT") == "1":
            session.type_into("Option", "4")
            generation = session.generation
            session.press("Enter")
            try:
                session.wait_for_text("PRINT MENU", timeout=30)
            except TimeoutError as exc:
                with changed:
                    tail = "".join(transcript[-240:])
                raise TimeoutError(
                    "CNFIGSSP did not reach PRINT MENU\n%s\n%s" %
                    (session.screen.render(fields=True), tail)) from exc
            session.wait_for_change(timeout=10, since=generation)
            session.settle(quiet=0.3, timeout=5)
            if os.environ.get("S36_LIST_CNFIG_TRACE") == "1":
                command(os.environ.get("S36_LIST_CNFIG_TRACE_COMMAND", "trace isn flow csp"))
                trace_member = os.environ.get("S36_LIST_CNFIG_TRACE_MEMBER", "#CPTC")
                if trace_member:
                    command("trace member " + trace_member)
            # Debug aid: monitor commands to run before the print is
            # started (for example a watchpoint or a trace member).
            for debug_command in os.environ.get("S36_LIST_CNFIG_BEFORE_PRINT_COMMANDS", "").split("|"):
                if debug_command.strip():
                    command(debug_command.strip())
            check_mark = len(transcript)
            session.press("Enter")
            expected_monitor = "printer 0.1:"
            deadline = time.monotonic() + float(
                os.environ.get("S36_LIST_TIMEOUT", "120"))
            while time.monotonic() < deadline:
                with changed:
                    text = "".join(transcript[check_mark:])
                if "CHECK [" in text or "storage protection" in text:
                    # Debug aid: dump the checked task's frame, region
                    # program block and job control block.
                    if os.environ.get("S36_LIST_CHECK_DUMPS") == "1":
                        dump_mark = len(transcript)
                        command("tasklist current")
                        wait_monitor("request-block chain", timeout=10, after=dump_mark)
                        time.sleep(0.5)
                        with changed:
                            listing = "".join(transcript[dump_mark:])
                        for label, pattern in (("pb", r"pb ([0-9A-F]{6})"),
                                               ("jcb", r"JCB pointer ([0-9A-F]{6})"),
                                               ("rb", r"request blk ([0-9A-F]{6})")):
                            found = re.search(pattern, listing)
                            if found:
                                command("dump %s 160" % found.group(1))
                        for debug_command in os.environ.get("S36_LIST_CHECK_COMMANDS", "").split("|"):
                            if debug_command.strip():
                                command(debug_command.strip())
                        time.sleep(1)
                        with changed:
                            text = "".join(transcript[check_mark:])
                    if os.environ.get("S36_LIST_TRACE_FULL") == "1":
                        print(text, file=sys.stderr)
                    raise AssertionError(
                        "CNFIGSSP print stopped on a processor check\n%s\n%s" %
                        (session.screen.render(fields=True), text[-12000:]))
                if expected_monitor in text:
                    break
                time.sleep(0.05)
            else:
                command("stations")
                command("tasklist")
                command("tasklist current")
                command("whereis")
                command("ace queue 30")
                command("wsscan")
                for debug_command in os.environ.get("S36_LIST_FAILURE_COMMANDS", "").split("|"):
                    if debug_command.strip():
                        command(debug_command.strip())
                time.sleep(1)
                with changed:
                    tail = "".join(transcript[-200:])
                    workstation_trace = "".join(transcript[check_mark:])
                    if os.environ.get("S36_LIST_TRACE_FULL") == "1":
                        print("".join(transcript), file=sys.stderr)
                raise TimeoutError(
                    "CNFIGSSP print did not reach printer 0.1\n%s\n%s\n%s" %
                    (session.screen.render(fields=True),
                     workstation_trace[-200000:], tail))

            after_printer_trace = os.environ.get("S36_LIST_AFTER_PRINTER_TRACE_COMMAND", "")
            if after_printer_trace:
                command(after_printer_trace)
                after_printer_member = os.environ.get("S36_LIST_AFTER_PRINTER_TRACE_MEMBER", "")
                if after_printer_member:
                    command("trace member " + after_printer_member)

        # Printer regressions must prove the whole spool-writer lifecycle,
        # not merely the first control record.  The original HISTORY gate
        # passed as soon as it saw `34 C4 01` (vertical position to line 1),
        # even though SPWRT then lost its SVC-42 completion and slept forever.
        printer_drain = os.environ.get("S36_LIST_EXPECT_PRINTER_DRAIN", "")
        if printer_drain:
            drain_deadline = time.monotonic() + float(
                os.environ.get("S36_LIST_DRAIN_TIMEOUT", os.environ.get("S36_LIST_TIMEOUT", "120")))
            inventory = ""
            match = None
            while time.monotonic() < drain_deadline:
                inventory_mark = len(transcript)
                command("stations")
                wait_monitor("print record(s)", timeout=10, after=inventory_mark)
                with changed:
                    inventory = "".join(transcript[inventory_mark:])
                match = re.search(
                    r"^  " + re.escape(printer_drain) +
                    r"\s+device[^\n]*\n(?:[^\n]*\n)*?\s+out (\d+) print record\(s\) "
                    r"(\d+) byte\(s\), \d+ dropped; \d+ startup response\(s\), "
                    r"(\d+) job\(s\) ended,",
                    inventory, re.MULTILINE)
                # A specific report can be required on the printer, followed
                # by its own end of job: the first small spool job to drain
                # must not satisfy the gate on behalf of the real report.
                expected_text = os.environ.get("S36_LIST_EXPECT_PRINTER_TEXT", "")
                text_ended = True
                if expected_text:
                    with changed:
                        printed = "".join(transcript[check_mark:])
                    at = printed.find("printer " + printer_drain + ": " + expected_text)
                    if at < 0:
                        at = printed.find(expected_text)
                    # SSP closes an entry with a page break; the writer then
                    # ends or waits for more work without a printer Clear, so
                    # the emulator's end-of-job (a Clear) is optional here.
                    text_ended = at >= 0 and ("printer " + printer_drain + ": [end of job]" in printed[at:] or
                                              "printer " + printer_drain + ": [page break]" in printed[at:])
                if match is not None:
                    records, output_bytes, jobs = map(int, match.groups())
                    if records > 1 and output_bytes > 3 and jobs > 0 and text_ended:
                        break
                time.sleep(0.25)
            if match is None:
                raise AssertionError("printer %s was absent from station inventory\n%s" %
                                     (printer_drain, inventory))
            records, output_bytes, jobs = map(int, match.groups())
            if records <= 1 or output_bytes <= 3 or jobs == 0 or not text_ended:
                for debug_command in os.environ.get("S36_LIST_DRAIN_FAILURE_COMMANDS", "").split("|"):
                    if debug_command.strip():
                        command(debug_command.strip())
                if os.environ.get("S36_LIST_DRAIN_FAILURE_COMMANDS", ""):
                    time.sleep(1)
                if os.environ.get("S36_LIST_TRACE_FULL") == "1":
                    with changed:
                        print("".join(transcript), file=sys.stderr)
                raise AssertionError(
                    "printer %s did not drain its spool file: %d record(s), %d byte(s), %d job(s) ended, expected "
                    "text %r ended=%s\n%s" %
                    (printer_drain, records, output_bytes, jobs,
                     os.environ.get("S36_LIST_EXPECT_PRINTER_TEXT", ""), text_ended, inventory))

        # CATALOG's help panel is split across two input pages. This drives
        # the reported form shape: the first Enter returns ALL/F1 as modified
        # fields while the empty output-file field is omitted; the second
        # returns NAME. The controller must expand that omitted null-filled
        # field as EBCDIC blanks.
        if os.environ.get("S36_LIST_CATALOG_FORM") == "1":
            session.type_at(8, 58, "ALL")
            session.type_at(10, 67, "F1")
            session.press("Enter")
            session.wait_for_text("To list entries alphabetically", timeout=30)
            session.type_at(14, 67, "NAME")
            check_mark = len(transcript)
            session.press("Enter")
            mark = len(transcript)
            command("wait idle 60")
            wait_monitor("wait: guest is idle after", timeout=65, after=mark)
            with changed:
                text = "".join(transcript[check_mark:])
            with session.lock:
                saw_rejected = any(session.screen.contains(value) for value in rejected)
                screen = session.screen.render("=== CATALOG RESULT ===", fields=True)
            if ("CHECK [program]" in text or "CHECK [CSP/MSP/channel]" in text or
                    saw_rejected or any(value in text for value in rejected)):
                raise AssertionError("CATALOG form failed\n%s\n%s" % (screen, text[-12000:]))

            page_target = os.environ.get("S36_LIST_CATALOG_PAGE_TARGET", "")
            if page_target:
                reached_target = False
                for page in range(100):
                    with session.lock:
                        if session.screen.contains(page_target):
                            reached_target = True
                            break
                        # CATALOG returns here after the final page.  Library
                        # contents vary between test volumes, so reaching the
                        # end cleanly is stronger than requiring one name.
                        if session.screen.contains("List library information"):
                            break
                    check_mark = len(transcript)
                    try:
                        session.press("Enter")
                    except TimeoutError as exc:
                        with changed:
                            text = "".join(transcript[check_mark:])
                        raise AssertionError("CATALOG stopped before page containing %r\n%s\n%s" %
                                             (page_target, session.screen.render(fields=True), text[-12000:])) from exc
                    mark = len(transcript)
                    command("wait idle 30")
                    wait_monitor("wait: guest is idle after", timeout=35, after=mark)
                    with changed:
                        text = "".join(transcript[check_mark:])
                    if "CHECK [program]" in text or "CHECK [CSP/MSP/channel]" in text:
                        raise AssertionError("CATALOG page %d caused a processor check\n%s\n%s" %
                                             (page + 1, session.screen.render(fields=True), text[-12000:]))
                else:
                    raise AssertionError("CATALOG did not finish within 100 pages\n%s" %
                                         session.screen.render(fields=True))

                if reached_target:
                    check_mark = len(transcript)
                    session.press("Enter")
                    mark = len(transcript)
                    command("wait idle 30")
                    wait_monitor("wait: guest is idle after", timeout=35, after=mark)
                    with changed:
                        text = "".join(transcript[check_mark:])
                    if "CHECK [program]" in text or "CHECK [CSP/MSP/channel]" in text:
                        raise AssertionError("Enter after %s caused a processor check\n%s\n%s" %
                                             (page_target, session.screen.render(fields=True), text[-12000:]))

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
        if "S36_LIST_COMMAND" in os.environ or os.environ.get("S36_LIST_CATALOG_MENU") == "1":
            print("PASS: %s reached its expected output without a processor check" % statement)
        else:
            print("PASS: LIST ALL displayed source without a processor check")
        if os.environ.get("S36_LIST_TRACE_FULL") == "1":
            with changed:
                print("".join(transcript))
    finally:
        for session in sessions.values():
            session.close()
        if proc.poll() is None:
            command("quit")
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
        try:
            os.unlink(config)
        except OSError:
            pass


if __name__ == "__main__":
    main()
