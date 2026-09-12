#!/usr/bin/env python3
"""Attach a 5250 station and answer its Put/Get invites, so an operator can use a
real tn5250 client on another station.

WHY THIS EXISTS. Sign-on currently only progresses if W7's invited Put/Get is
answered within a fraction of a second of arriving; a human pressing Enter takes
seconds and misses it. That is an emulator defect, not a property of the machine
- see docs/s36/terminal-response-must-be-instant-2026-09-07.md and
docs/s36/ordering-sensitivity-is-an-ipl-stall-2026-09-07.md, which trace it to
the machine stalling during IPL so that the keystroke stands in for an
IPL-completion event we never generate.

This is therefore SCAFFOLDING around a known defect, not a feature. When the
stall is fixed, a human at both stations must see the same panels and this script
should stop being needed. Do not build anything on it.

It replies only to a genuine RFC-1205 Put/Get - a record that really ends in
Read Input Fields - and never manufactures an unprompted key.

    python3 test/tn5250-responder.py 2306

Leave it running; Ctrl-C to stop.
"""
import argparse, importlib.util, os, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location(
    'tn5250_capture', os.path.join(HERE, 'tn5250-capture.py'))
cap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cap)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('port', type=int, nargs='?', default=2306,
                    help='station listener port (default 2306 = W7)')
    args = ap.parse_args()

    client = cap.EnterAfterPutGetClient(args.port)
    client.start()
    if not client.attached.wait(timeout=30):
        print('no 5250 negotiation on port %d - is the emulator listening?'
              % args.port, file=sys.stderr)
        return 2
    print('attached to port %d; answering invited Put/Gets. Ctrl-C to stop.'
          % args.port)
    seen = 0
    try:
        while True:
            time.sleep(0.25)
            if len(client.records) != seen:
                seen = len(client.records)
                print('  %d record(s) from the guest; AID sent: %s'
                      % (seen, client.enter_sent.is_set()))
    except KeyboardInterrupt:
        client.stop = True
        print('\nstopped after %d record(s)' % len(client.records))
    return 0


if __name__ == '__main__':
    sys.exit(main())
