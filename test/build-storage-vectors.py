#!/usr/bin/env python3
"""Guest state for test/storage-svcs.sh - the SVC 2C/2D/2F/51 exercise.

Everything this writes is a control block at an address chosen only so that
it is clear of phase 1 and of guest low storage.  Two images are written -
0x0C00..0x0EFF of control blocks and lists, and the program at 0x1000 -
because guest 0x0F00 between them is the IPL task block, and overwriting it
detaches the request block from the task.

  0C00  program block "PB"   the caller's own module, load page 2, 2 pages
  0C40  program block "PB2"  another module, load page 8, 4 pages
  0C80  request block "RB2"  the caller's, per SVC 2F action 5: it runs PB2
                             and carries one map table entry of its own
  0D00  storage block "SB"   a work space of 4 pages, for SVC 2C and 2D
  0D40  map parameter lists
  0E00  request block "RB"   the IPL task's, which the IPL builds - only the
                             fields the register spill does not touch are set
  1000  the program

The first parameter list is SA21-9436 3-125's own worked example, byte for
byte.
"""
import sys

BASE = 0x0C00
END = 0x0F00
CODE = 0x1000
CODE_END = 0x1040
img = bytearray(END - BASE)
code = bytearray(CODE_END - CODE)


def put(addr, *bytes_):
    buf, base = (code, CODE) if addr >= CODE else (img, BASE)
    for i, b in enumerate(bytes_):
        buf[addr - base + i] = b


def put16(addr, v):
    put(addr, (v >> 8) & 0xFF, v & 0xFF)


def put24(addr, v):
    put(addr, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


EYE_PB, EYE_RB, EYE_SB = 0xD7C2, 0xD9C2, 0xE2C2

# --- program blocks ---------------------------------------------------------
put16(0x0C00, EYE_PB)
put(0x0C0C, 2)                 # +12 load page: the module is at 0x1000
put16(0x0C12, 2)               # +18 pages
put16(0x0C14, 2)               # +20 pages ready
put(0x0C3F, 0)                 # +63 request block units floor

put16(0x0C40, EYE_PB)
put(0x0C4C, 8)                 # +12 load page 8 -> 0x4000
put16(0x0C52, 4)               # +18 four pages
put(0x0C7F, 0)

# --- the caller's request block, and its own map table ----------------------
put16(0x0C80, EYE_RB)
put(0x0C82, 8)                 # +2 length, in 16-byte units: 128 bytes
put(0x0CA8, 1)                 # +40 one map table entry
put24(0x0CA9, 0x0C40)          # +41 its program block
# its map table is at rb + 64 + pb[63]*16 = 0x0CC0
put(0x0CC0, 16, 2)             # region pages 16..17
put16(0x0CC2, 3)               # of the object's pages 3..4
put24(0x0CC5, 0x0D00)          # which is the storage block

# --- the storage block SVC 2C assigns from ----------------------------------
put16(0x0D00, EYE_SB)
put(0x0D04, 0x81)              # +4 type: a task work space
put16(0x0D10, 4)               # +16 four 2 KB pages - the arena, 8192 bytes
put16(0x0D12, 0)               # +18 pages mapped starts at zero; it is NOT the size
put24(0x0D18, 100)             # +24 its task work area disk address

# --- map parameter lists ----------------------------------------------------
# A: SA21-9436 3-125's own worked example, byte for byte.
put(0x0D40, 0x51, 0x48, 0x01, 0x08, 0x00, 0x81, 0x00, 0x00)

# B: action 9, map the caller's own program at region page 6, 4096 bytes,
#    answering through the list's own +8..10 field.
put(0x0D48, 0x31, 0x9B, 0x08)
put16(0x0D4B, 0x1000)
put(0x0D4D, 0x00)
put16(0x0D4E, 0x0000)
put24(0x0D50, 0x801000)        # source: translated 0x1000, the module's page 2

# C: action 5, two entries, the second following on.
put(0x0D58, 0x18, 0x5B, 0x08)  # page 3, action 5, USING the list's +8..10
put16(0x0D5B, 0x1000)          # 4096 bytes
put(0x0D5D, 0x00)
put16(0x0D5E, 0x0000)
put24(0x0D60, 0x804000)        # source: page 8, which is RB2's module
put(0x0D63, 0x29, 0x5B, 0x08)  # page 5, last entry
put16(0x0D66, 0x1000)
put(0x0D68, 0x00)
put16(0x0D69, 0x0000)
put24(0x0D6B, 0x808000)        # source: page 16, RB2's own map table entry

# --- the IPL task's request block ------------------------------------------
put16(0x0E00, EYE_RB)
put(0x0E02, 12)                # +2 length: 192 bytes, room for a map table
put24(0x0E03, 0x0C80)          # +3 the caller's request block, for action 5
put(0x0E28, 0)                 # +40 no map table entries yet
put24(0x0E29, 0x0C00)          # +41 this block runs the program block at 0C00

# --- the program -----------------------------------------------------------
# One supervisor call per address, so the script can step to each in turn.
put(0x1000, 0xF4, 0x00, 0x2F)              # 2F with the manual's list
put(0x1003, 0xF4, 0x00, 0x2F)              # 2F action 9
put(0x1006, 0xF4, 0x00, 0x2F)              # 2F action 5
put(0x1009, 0xF4, 0x00, 0x2C)              # 2C assign
put(0x100C, 0xF4, 0x00, 0x2C)              # 2C assign again
put(0x100F, 0xF4, 0x00, 0x2D)              # 2D free the first
put(0x1012, 0xF4, 0x00, 0x2C)              # 2C assign again - the hole comes back
put(0x1015, 0xF4, 0x00, 0x51, 0x00, 0x00, 0x02)   # 51 get, XR1 sector, 2 sectors
put(0x101C, 0xF4, 0x00, 0x51, 0x02, 0x00, 0x02)   # 51 get, RELATIVE - refused
put(0x1023, 0xF4, 0x00, 0x51, 0x08, 0x00, 0x01)   # 51 get, indirect XR1
put(0x102A, 0xF4, 0x00, 0x51, 0x04, 0x00, 0x01)   # 51 get from JCBWSWA - no JCB
put(0x1031, 0xF4, 0x00, 0x51, 0x01, 0x00, 0x01)   # 51 PUT - the volume is read-only

# The indirect disk address SVC 51 reads through XR1: three bytes ENDING at
# the address in XR1, so the field is 0x0DF0..0x0DF2 and XR1 points at 0x0DF2.
put24(0x0DF0, 27)

open(sys.argv[1], 'wb').write(img)
open(sys.argv[2], 'wb').write(code)
