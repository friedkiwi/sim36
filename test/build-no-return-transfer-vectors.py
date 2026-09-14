#!/usr/bin/env python3
"""Guest vectors for the synchronous SVC 10 no-return transfer test.

The caller's program request area begins at request-block +64.  The target
compares the first two bytes of its own request area with the caller's marker;
its final branch therefore observes the supervisor's transfer semantics rather
than host-side bookkeeping.
"""

import sys

BASE, END = 0x0C00, 0x0F00
CODE, CODE_END = 0x1000, 0x1040
TARGET, TARGET_END = 0x2000, 0x2040
blocks = bytearray(END - BASE)
code = bytearray(CODE_END - CODE)
target = bytearray(TARGET_END - TARGET)


def put(addr, *values):
    if addr >= TARGET:
        image, base = target, TARGET
    elif addr >= CODE:
        image, base = code, CODE
    else:
        image, base = blocks, BASE
    for offset, value in enumerate(values):
        image[addr - base + offset] = value


def put16(addr, value):
    put(addr, value >> 8, value & 0xFF)


def put24(addr, value):
    put(addr, value >> 16, (value >> 8) & 0xFF, value & 0xFF)


EYE_PB, EYE_RB = 0xD7C2, 0xD9C2

# The caller and its initial request block.  pb+63 declares one 16-byte
# program request area, whose first two bytes carry a recognizable marker.
put16(0x0C00, EYE_PB)
put(0x0C0C, 2)
put16(0x0C12, 2)
put16(0x0C14, 2)
put(0x0C3F, 1)

put16(0x0E00, EYE_RB)
put(0x0E02, 12)
put24(0x0E29, 0x0C00)
put(0x0E40, 0xAF, 0x01, 0x12, 0x34)

# An already-resident target, found through the sector-5 hash bucket.  It also
# asks for one unit of program request area.
put16(0x0C40, EYE_PB)
put(0x0C44, 1)
put(0x0C4C, 4)
put16(0x0C52, 1)
put16(0x0C54, 1)
put24(0x0C70, 5)
put(0x0C74, 0)
put(0x0C78, 0)
put(0x0C7F, 1)
put24(0x0D00 + 4 * (5 & 7) + 1, 0x0C40)

# Transfer-control entry and the no-return SVC 10.
put24(0x0D40, 5)
put(0x0D43, 8, 0)
put(0x1000, 0xF4, 0x00, 0x10, 0x0D, 0x40, 0x00)

# JC always,$2018 leaves the load-member header fields at +10..13 zero.  The
# target there executes L XR1,$0B9B; CLC 01(XR1),$2030; BC Equal,$2028.  After
# five stepped instructions, IAR 2028 proves it read AF01 from its own request
# area; without the carry-forward it falls through to 2025.
put(0x2000, 0xF2, 0x87, 0x15)
put(0x2018, 0x35, 0xA1, 0x0B, 0x9B)
put(0x201C, 0x8D, 0x01, 0x01, 0x20, 0x30)
put(0x2021, 0xC0, 0x01, 0x20, 0x28)
put(0x2030, 0xAF, 0x01)

open(sys.argv[1], "wb").write(blocks)
open(sys.argv[2], "wb").write(code)
open(sys.argv[3], "wb").write(target)
