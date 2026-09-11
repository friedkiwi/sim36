#!/usr/bin/env python3
"""Guest state for test/work-area-svcs.sh - the SVC 33/34/35 exercise, and
SVC 51's relative form.

Three images, because the anchor is inside low storage and everything else is
not:

  0BB9  queue header 46      the 24-bit guest address of the first QH block.
                             `csipl` writes this and so does EnsureTaskWorkArea;
                             writing it here is what makes the emulator keep the
                             chain the test built instead of building csipl's.
  0C00  the QH chain         two 16-byte headers and one free element
  0C40  WRK parameter lists  the first is SA21-9436 3-131's own worked example
  1000  the program

The extent the first header describes deliberately starts at ONE-BASED sector 27
- zero-based 26, the system configuration record - so that a relative read
through SVC 51 lands on bytes this suite did not write and can be checked
against the volume.

docs/s36/storage-and-concurrency.md
"""
import sys

ANCHOR = 0x0BB9
BASE, END = 0x0C00, 0x0CA0
CODE, CODE_END = 0x1000, 0x1050

img = bytearray(END - BASE)
code = bytearray(CODE_END - CODE)


def put(addr, *bytes_):
    buf, base = (code, CODE) if addr >= CODE else (img, BASE)
    for i, b in enumerate(bytes_):
        buf[addr - base + i] = b


def put16(addr, v): put(addr, (v >> 8) & 0xFF, v & 0xFF)
def put24(addr, v): put(addr, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


EYE_QH = 0xD8C8                       # csipl c1832740

# --- the QH chain, laid out as csipl lays its own two out -------------------
# Header 1: an ORDINARY base identifier, which is the case csipl never builds
# and SSP's task work area manager does - getHeap stops at the first header
# whose +5 is 254 or more (c18b8700), so only this one is allocatable.
put16(0x0C00 + 0, EYE_QH)
put24(0x0C00 + 2, 0x0C10)             # next header
put(0x0C00 + 5, 0x01)                 # base identifier
put(0x0C00 + 6, 0x00)                 # flags: no checkExtent
put24(0x0C00 + 7, 0x0C20)             # head of the free element chain
put24(0x0C00 + 11, 27)                # base sector, ONE-BASED: 0-based 26
put16(0x0C00 + 14, 64)                # the extent's length in sectors

# Header 2: csipl's own first header, base FE - the IPL transient work area.
# It terminates getHeap's walk and is reachable only by naming it.
put16(0x0C10 + 0, EYE_QH)
put24(0x0C10 + 2, 0)
put(0x0C10 + 5, 0xFE)
put24(0x0C10 + 11, 7167)              # csipl c1832748
put16(0x0C10 + 14, 1014)              # csipl c183276c, with setfd's 8191

# One free element: the whole extent, displacement 0.
put24(0x0C20 + 2, 0)                  # next
put(0x0C20 + 5, 0x01)                 # base identifier
put16(0x0C20 + 6, 0)                  # displacement
put16(0x0C20 + 8, 64)                 # sectors
put(0x0C20 + 10, 0x00)

# --- WRK parameter lists ----------------------------------------------------
# SA21-9436 3-131 verbatim: "805000 = 028100100000000200000000", which
# "unconditionally creates a 2 page (4096 byte) task work space for the task
# with a task ID of hex 0002".
put(0x0C40, 0x02, 0x81, 0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00)
# The same list with command 1, conditional creation - which has to search first.
# Use translated zero in the anchor field. nucwrk c18bb988..c18bb998 masks off
# bit 0x800000 before deciding between nuquscs and nuqscan, so this must take the
# ordinary type search just like 000000. TUPH uses this exact representation.
put(0x0C50, 0x01, 0x81, 0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x80, 0x00, 0x00)
# And with a command that is not 1, 2 or 3 at all.
put(0x0C60, 0x05, 0x81, 0x00, 0x10, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00)
# A current-task workspace used to prove that deleting and rebuilding the same
# control-block address also rebuilds translated-assign's allocation bitmap.
put(0x0C70, 0x02, 0x82, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)
put(0x0C80, 0x03, 0x82, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)
# nucwnew's input flag bit 01 asks nucbldsb to set the initial mapped-page
# count to the rounded workspace size. #TMOPN uses this exact form for its
# one-page type-CD workspace before writing through an SVC 2F mapping.
put(0x0C90, 0x02, 0x83, 0x00, 0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)

# --- the program ------------------------------------------------------------
# 33, 34 and 35 take no inline parameters, so each is three bytes - which is
# what the manual's own object codes F40033, F40034 and F40035 show.
put(0x1000, 0xF4, 0x00, 0x33)                     # allocate 16 sectors
put(0x1003, 0xF4, 0x00, 0x33)                     # WR6 = 0     -> nuersvc 109
put(0x1006, 0xF4, 0x00, 0x33)                     # too many    -> High
put(0x1009, 0xF4, 0x00, 0x34)                     # free the 16 back
put(0x100C, 0xF4, 0x00, 0x33)                     # the whole extent
put(0x100F, 0xF4, 0x00, 0x34)                     # and back again
put(0x1012, 0xF4, 0x00, 0x51, 0x02, 0x00, 0x01)   # relative get, key 01
put(0x1018, 0xF4, 0x00, 0x51, 0x02, 0x00, 0x01)   # relative, key FF
put(0x101E, 0xF4, 0x00, 0x51, 0x02, 0x00, 0x01)   # relative, an unknown key
put(0x1024, 0xF4, 0x00, 0x51, 0x02, 0x00, 0x01)   # relative, past the extent
put(0x102A, 0xF4, 0x00, 0x35)                     # WRK, the manual's example
put(0x102D, 0xF4, 0x00, 0x35)                     # WRK, conditional
put(0x1030, 0xF4, 0x00, 0x35)                     # WRK, command 5
put(0x1033, 0xF4, 0x01, 0x33)                     # 33 with Q bit 7, nothing free
put(0x1036, 0xF4, 0x00, 0x35)                     # create type-82 current-task WS
put(0x1039, 0xF4, 0x00, 0x2C)                     # consume its complete 4 KB
put(0x103C, 0xF4, 0x00, 0x35)                     # delete type-82 WS
put(0x103F, 0xF4, 0x00, 0x35)                     # recreate it (same SB address)
put(0x1042, 0xF4, 0x00, 0x2C)                     # complete 4 KB must be free again
put(0x1045, 0xF4, 0x00, 0x35)                     # WRK, flag 01 initially mapped

if len(sys.argv) != 4:
    sys.exit('usage: build-work-area-vectors.py ANCHOR.bin VECTORS.bin PROGRAM.bin')
open(sys.argv[1], 'wb').write(bytes([0x00, 0x0C, 0x00]))
open(sys.argv[2], 'wb').write(bytes(img))
open(sys.argv[3], 'wb').write(bytes(code))
