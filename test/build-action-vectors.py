#!/usr/bin/env python3
"""Guest code for test/action-controller.sh - SVC 0B with every decoded mask.

Nothing here is guest state: it is one program, run as the IPL task (task
block 0F00, request block 0E00, both left by `ipl pause`), issuing SVC 0B once
per mask that NuSetAction::executeRequest (c18ab2f0) switches on, plus the two
that are not in that switch (0x79, inline in nusvc; 0x29, SSP R3's device-status
equate), plus one the switch defaults on (0x33).  The program ends in SVC 1E so
the only task waits and nudspchA takes its no-task exit, which is where the
model reports the posts it could not run.  docs/s36/svc-task-event-queue.md §0B
"""
import sys

CODE = 0x1000
code = bytearray(0x100)


def put(addr, *bytes_):
    for i, b in enumerate(bytes_):
        code[addr - CODE + i] = b


# SVC is F4 00 <R> <inline...>; SVC 0B carries one inline parameter.
MASKS = [0x79, 0x11, 0x65, 0x2D, 0x29, 0x01, 0x17, 0x19, 0x1B, 0x6B, 0x33, 0x01]
at = CODE
for m in MASKS:
    put(at, 0xF4, 0x00, 0x0B, m)
    at += 4
put(at, 0xF4, 0x00, 0x1E, 0x02)             # 1E  wait - nothing else is ready

open(sys.argv[1], 'wb').write(code)
print('%04X' % (at + 4))                     # the IAR after the final SVC 1E
