#!/usr/bin/env python3
"""Guest state for test/program-block-svcs.sh - SVC 31, 32, 2E and 2F action 4.

Same shape and the same constraint as build-storage-vectors.py: two images,
0x0C00..0x0EFF and the program at 0x1000, because guest 0x0F00 in between is the
IPL task block and overwriting it detaches the request block from the task.

  0C00  program block "PB"   the caller's own module, load page 2, 2 pages
  0D00  storage block "SB"   a task work space, type hex 81, load page 0
  0D40  map parameter list   SA21-9436 3-125's own worked example, byte for byte
  0E00  request block "RB"   the IPL task's
  1000  the program

docs/s36/svc-program-block-lifecycle.md
"""
import sys

BASE, END = 0x0C00, 0x0F00
CODE, CODE_END = 0x1000, 0x1040
img = bytearray(END - BASE)
code = bytearray(CODE_END - CODE)


def put(addr, *bytes_):
    buf, base = (code, CODE) if addr >= CODE else (img, BASE)
    for i, b in enumerate(bytes_):
        buf[addr - base + i] = b


def put16(addr, v): put(addr, (v >> 8) & 0xFF, v & 0xFF)
def put24(addr, v): put(addr, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


EYE_PB, EYE_RB, EYE_SB = 0xD7C2, 0xD9C2, 0xE2C2

# --- the caller's own program block -----------------------------------------
put16(0x0C00, EYE_PB)
put(0x0C0C, 2)                 # +12 load page: the module is at 0x1000
put16(0x0C12, 2)               # +18 pages
put16(0x0C14, 2)               # +20 pages ready
put(0x0C3F, 0)                 # +63 request block units floor -> map table at rb+64

# --- a task work space, for SVC 2F action 4 ---------------------------------
# Type hex 81 is SA21-9436 3-125's own "task work space type", and 0x81 >= 128 is
# what puts it on the OWNING TASK's chain at tb+43 rather than on queue header 42
# (nucwsbsq c18bc154).  Load page 0, so the displacement action 4 computes is
# zero and the manual's answer is reproduced exactly.
put16(0x0D00, EYE_SB)
put(0x0D04, 0x81)              # +4 type
put(0x0D0C, 0)                 # +12 load page
put16(0x0D10, 4)               # +16 four 2 KB pages
put16(0x0D12, 4)               # +18 pages mapped

# --- SA21-9436 3-125's map parameter list, unchanged ------------------------
# "Assuming XR1 contains hex 800000 and location hex 802080 contains hex
#  5148010800810000 then the SVC maps the first page (virtual page 0) of the task
#  work space type (hex 81) to logical address hex 5000.  At the end of the SVC,
#  XR1 contains hex 805000."
put(0x0D40, 0x51, 0x48, 0x01, 0x08, 0x00, 0x81, 0x00, 0x00)

# --- the IPL task's request block -------------------------------------------
put16(0x0E00, EYE_RB)
put(0x0E02, 12)                # +2 length: 192 bytes, room for a map table
put(0x0E28, 0)                 # +40 no map table entries yet
put24(0x0E29, 0x0C00)          # +41 this block runs the program block at 0C00

# --- the program ------------------------------------------------------------
put(0x1000, 0xF4, 0x00, 0x31)                     # ATASK
put(0x1003, 0xF4, 0x10, 0x31)                     # ATASK, Q bit 3: maximum swap area
put(0x1006, 0xF4, 0x00, 0x32)                     # DTASK
put(0x1009, 0xF4, 0x00, 0x2E)                     # Time of Day
put(0x100C, 0xF4, 0x00, 0x0E, 0x00, 0x0B, 0x10)   # queue LIFO on the head in XR2,
                                                  #   chain field's last byte = 11
put(0x1013, 0xF4, 0x00, 0x2F)                     # MAP, the manual's list

open(sys.argv[1], 'wb').write(img)
open(sys.argv[2], 'wb').write(code)
