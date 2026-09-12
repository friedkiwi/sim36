#!/usr/bin/env python3
"""Reusable raw-socket 5250 capture harness for the sign-on render last mile.

EXPERIMENTAL HARNESS (test-only; touches no emulator code). Drives the emulator's
monitor over stdin, connects a minimal RFC-1205/RFC-2877 5250 client to a station
listener, and decodes captured logical records (RFC 1205 header + 5250 command
stream) so the SIGN ON panel fields can be read.

The monitor command stream is the sole authority over what the emulator does. A
scripted capture receives exactly the commands in that script: the harness does
not silently add a boot, invite, or synthetic write. Commands through the first
`boot` or `ipl` are sent while the client retries its connection; the remaining
commands are sent after negotiation. Startup does not reliably complete the
station negotiation before the guest is booted, so waiting first creates an
ordering race and can consume the client's entire retry window. This
matters for sign-on research, where an appended `wswrite demo` makes a guest-only
capture look green.
Use test/tn5250-demo.sim for the explicit transport proof and
test/tn5250-appliance.sim for an injection-free appliance observation.

With no script, the legacy demo remains available for interactive convenience.
"""
import argparse, socket, subprocess, sys, threading, time

IAC, DO, DONT, WILL, WONT, SB, SE = 255, 253, 254, 251, 252, 250, 240
EOR = 239
OPT_BINARY, OPT_EOR, OPT_TTYPE, OPT_NEWENV = 0, 25, 24, 39

def ebcdic(b):
    try:
        return b.decode('cp037')
    except Exception:
        return repr(b)

class Client(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self.sock = None
        self.records = []
        self.attached = threading.Event()
        self.stop = False

    def run(self):
        # The emulator reads the volume before it opens its listeners. Do not
        # make command ordering double as a startup delay: retry the connection
        # itself so a script can remain the complete, honest action transcript.
        deadline = time.monotonic() + 10
        while True:
            try:
                s = socket.create_connection(('127.0.0.1', self.port), timeout=1)
                break
            except (ConnectionRefusedError, socket.timeout):
                if time.monotonic() >= deadline:
                    return
                time.sleep(0.1)
        s.settimeout(0.5)
        self.sock = s
        buf = b''
        pending = b''
        while not self.stop:
            try:
                data = s.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            pending += data
            pending, recs = self.consume(pending, s)
            self.records.extend(recs)

    def consume(self, data, s):
        """Split Telnet: handle IAC verbs/SB, undouble IAC IAC, cut on IAC EOR."""
        out, recs, i, cur = b'', [], 0, b''
        while i < len(data):
            c = data[i]
            if c == IAC:
                if i + 1 >= len(data):
                    return data[i:], recs  # incomplete
                n = data[i+1]
                if n == IAC:
                    cur += b'\xff'; i += 2; continue
                if n == EOR:
                    recs.append(cur); cur = b''; i += 2; continue
                if n in (DO, DONT, WILL, WONT):
                    if i + 2 >= len(data):
                        return data[i:], recs
                    self.reply(s, n, data[i+2]); i += 3; continue
                if n == SB:
                    end = data.find(bytes([IAC, SE]), i)
                    if end < 0:
                        return data[i:], recs
                    self.subneg(s, data[i+2:end]); i = end + 2; continue
                i += 2; continue
            cur += bytes([c]); i += 1
        # leftover partial record stays in cur -> keep as pending
        return cur, recs

    def reply(self, s, verb, opt):
        if verb == DO:
            ans = WILL if opt in (OPT_BINARY, OPT_EOR, OPT_TTYPE, OPT_NEWENV) else WONT
            s.sendall(bytes([IAC, ans, opt]))
        elif verb == WILL:
            ans = DO if opt in (OPT_BINARY, OPT_EOR) else DONT
            s.sendall(bytes([IAC, ans, opt]))

    def subneg(self, s, body):
        if not body:
            return
        opt = body[0]
        if opt == OPT_TTYPE and len(body) > 1 and body[1] == 1:  # SEND
            s.sendall(bytes([IAC, SB, OPT_TTYPE, 0]) + b'IBM-3180-2' + bytes([IAC, SE]))
        elif opt == OPT_NEWENV and len(body) > 1 and body[1] == 1:  # SEND
            s.sendall(bytes([IAC, SB, OPT_NEWENV, 0, IAC, SE]))
        self.attached.set()

class EnterAfterPutGetClient(Client):
    """Send one real Enter reply, and only in response to an invited PUT.

    This is deliberately a wire-level test client.  It neither calls the
    monitor's ``wsinput`` command nor changes emulator state.  The trigger and
    response byte strings are retained so a regression can prove that the
    client answered the guest's exact RFC-1205 Put/Get instead of racing IPL or
    manufacturing an unprompted key.
    """
    def __init__(self, port):
        super().__init__(port)
        self.enter_sent = threading.Event()
        self.trigger_record = None
        self.sent_record = None

    @staticmethod
    def _is_invited_put(record):
        if len(record) < 14:
            return False
        logical_length = int.from_bytes(record[0:2], 'big')
        return (logical_length == len(record)
                and record[2:4] == b'\x12\xa0'
                and record[6] == 4
                and record[9] == 0x03
                and (record[10:].endswith(b'\x04\x52\x00\x00')
                     or record[10:].endswith(
                         b'\x04\xf3\x00\x08\xd9\x32\x00\x80\x00\x00')))

    @staticmethod
    def _frame_gds(opcode, body):
        logical_length = 10 + len(body)
        record = bytes([
            logical_length >> 8, logical_length & 0xff,
            0x12, 0xa0, 0x00, 0x00, 0x04, 0x00, 0x00, opcode
        ]) + body
        # RFC 854 binary transparency plus RFC 885 record termination.
        wire = record.replace(b'\xff', b'\xff\xff') + bytes([IAC, EOR])
        return record, wire

    def consume(self, data, s):
        pending, records = super().consume(data, s)
        for record in records:
            if self.enter_sent.is_set() or not self._is_invited_put(record):
                continue
            self.trigger_record = record
            # RFC 1205 terminal response: cursor address, AID, modified fields.
            # There are no modified fields in this probe.
            self.sent_record, wire = self._frame_gds(0x00, b'\x00\x00\xf1')
            s.sendall(wire)
            self.enter_sent.set()
        return pending, records

def decode(rec):
    if len(rec) < 10:
        return "  <short record %d bytes>" % len(rec)
    ln = int.from_bytes(rec[0:2], 'big')
    rtype = rec[2:4].hex()
    opcode = rec[9]
    lines = ["  RFC1205: len=%d type=%s opcode=0x%02X" % (ln, rtype, opcode)]
    ds = rec[10:]
    lines.append("  5250 body: len=%d hex=%s" % (len(ds), ds.hex()))
    i = 0
    while i < len(ds):
        c = ds[i]
        if c == 0x04 and i + 1 < len(ds):  # ESC + command
            cmd = ds[i+1]
            name = {0x40: 'ClearUnit', 0x11: 'WriteToDisplay',
                    0xF3: 'WriteStructuredField'}.get(cmd, 'cmd%02X' % cmd)
            lines.append("  ESC %s (04 %02X)" % (name, cmd))
            i += 2
            if cmd == 0x11 and i + 1 < len(ds):
                lines.append("    CC1=%02X CC2=%02X" % (ds[i], ds[i+1])); i += 2
            continue
        if c == 0x11 and i + 2 < len(ds):  # SBA row col
            lines.append("  SBA row=%d col=%d" % (ds[i+1], ds[i+2])); i += 3
            continue
        j = i
        while j < len(ds) and ds[j] not in (0x04, 0x11):
            j += 1
        txt = ebcdic(ds[i:j])
        if txt.strip():
            lines.append("  TEXT %r" % txt)
        i = j
    return "\n".join(lines)

def split_at_guest_start(lines):
    """Return (startup, remainder), with the first boot/IPL in startup.

    Monitor comments and blank lines are retained so the supplied script stays
    the exact action transcript.  The split only controls when commands are
    delivered relative to TN5250 negotiation.
    """
    for i, line in enumerate(lines):
        command = line.split('#', 1)[0].strip().split()
        if command and command[0].lower() in ('boot', 'ipl'):
            return lines[:i + 1], lines[i + 1:]
    return lines, []

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', type=int, action='append', dest='ports',
                    help='station listener port; repeat to attach several '
                         'stations at once. An Advanced/36 runs W1 as the '
                         'machine\'s own background console and lends W7 to a '
                         'TFRM36 user, so a single-station capture is not the '
                         'default shape - see '
                         'docs/s36/w1-console-plus-w7-user-2026-09-07.md.')
    ap.add_argument('--script', default=None)
    ap.add_argument('--write', default='wswrite console demo')
    ap.add_argument('--exe', default=sim36env.exe())
    args = ap.parse_args()
    if not args.ports:
        args.ports = [2300]

    proc = subprocess.Popen([args.exe], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    # Drain the monitor continuously. A verbose guest trace easily fills the
    # subprocess pipe before the fixed capture window ends; waiting to read it
    # until after proc.wait deadlocks the emulator in write(2) and truncates the
    # very producer sequence this harness is meant to preserve.
    monitor_lines = []
    monitor_reader = threading.Thread(
        target=lambda: monitor_lines.extend(iter(proc.stdout.readline, '')),
        daemon=True)
    monitor_reader.start()

    def send(line):
        if proc.poll() is not None:
            return False
        try:
            proc.stdin.write(line + "\n"); proc.stdin.flush()
            return True
        except BrokenPipeError:
            return False

    if args.script:
        with open(args.script) as script_file:
            startup_commands, post_attach_commands = split_at_guest_start(
                [line.rstrip("\n") for line in script_file])
    else:
        startup_commands = ['boot']
        post_attach_commands = ['wsinvite console', args.write]

    # Start the retrying client first, but do not wait yet: deliver boot/IPL
    # while the connection/negotiation retry window is still open.
    clients = [Client(port) for port in args.ports]
    for cli in clients:
        cli.start()
    for line in startup_commands:
        if not send(line):
            break
    for port, cli in zip(args.ports, clients):
        if not cli.attached.wait(timeout=8):
            print("client on port %d did not complete 5250 negotiation" % port,
                  file=sys.stderr)
    time.sleep(1.0)
    for line in post_attach_commands:
        if not send(line):
            break
    time.sleep(2.0)

    send('quit')
    try:
        proc.wait(timeout=8)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=3)
    for cli in clients:
        cli.stop = True
    time.sleep(0.3)

    monitor_reader.join(timeout=1)
    monitor_output = ''.join(monitor_lines)
    print("=== monitor output ===")
    print(monitor_output.rstrip())

    captured = False
    for port, cli in zip(args.ports, clients):
        print("=== captured %d logical record(s) on port %d ===" %
              (len(cli.records), port))
        for n, rec in enumerate(cli.records):
            if not rec:
                continue
            captured = True
            print("--- record %d (%d bytes): %s" % (n, len(rec), rec[:16].hex()))
            print(decode(rec))
    sys.exit(0 if captured else 2)

if __name__ == '__main__':
    main()
