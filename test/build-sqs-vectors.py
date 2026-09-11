#!/usr/bin/env python3
"""Guest program for test/system-queue-space.sh - the SVC 06 / SVC 07 exercise.

Two instructions is all this needs: Assign and Free Assigned Areas, each at its
own address so the suite can step to either one with the registers it wants.
Everything else the calls read - XR1, the PACT prefix and WR6 - the monitor sets
directly, exactly as SA21-9436 3-79 and 3-80 describe the inputs.

  1000  F4 00 06   Assign, no Q bits: XR1 in is the length, XR1 out the address
  1003  F4 00 07   Free Assigned Areas: XR1 is the address, WR6 the length

docs/s36/system-queue-space.md
"""
import sys

CODE = 0x1000
code = bytearray(0x10)

code[0x0:0x3] = bytes([0xF4, 0x00, 0x06])
code[0x3:0x6] = bytes([0xF4, 0x00, 0x07])

open(sys.argv[1], 'wb').write(code)
