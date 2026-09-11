#!/usr/bin/env python3
"""Guest state for test/task-creation.sh - NuEmul::nuptask through SVC 10 Q bit 2.

Three images, and the split is the same one every suite here uses: guest 0x0F00
is the IPL task block, so the control blocks stop at 0x0EFF.

  0C00  program block "PB"  the CALLER's own module, load page 2 -> guest 1000
  0C40  program block "PB"  the TARGET module, load page 4 -> guest 2000, and
                            already RESIDENT: its +48..50 is sector 5 and it is
                            chained on that sector's hash bucket, so nup1000
                            finds it without reading the disk
  0D15  the hash bucket for sector 5: 0xD00 + 4*(5 & 7) + 1, three bytes
  0D40  a five-byte transfer control table entry naming sector 5
  0E00  request block "RB"  the IPL task's
  1000  the caller's program
  2000  the target module - one instruction, so that a task really entering it
        is visible as an instruction address and not only as a trace line

docs/s36/svc-task-creation.md, docs/s36/svc-transfer-control.md
"""
import sys

BASE, END = 0x0C00, 0x0F00
CODE, CODE_END = 0x1000, 0x1040
MODULE, MODULE_END = 0x2000, 0x2010

img = bytearray(END - BASE)
code = bytearray(CODE_END - CODE)
module = bytearray(MODULE_END - MODULE)


def put(addr, *bytes_):
    if addr >= MODULE:
        buf, base = module, MODULE
    elif addr >= CODE:
        buf, base = code, CODE
    else:
        buf, base = img, BASE
    for i, b in enumerate(bytes_):
        buf[addr - base + i] = b


def put16(addr, v): put(addr, (v >> 8) & 0xFF, v & 0xFF)
def put24(addr, v): put(addr, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


EYE_PB, EYE_RB = 0xD7C2, 0xD9C2

# --- the caller's own program block, exactly build-program-block-vectors.py's --
put16(0x0C00, EYE_PB)
put(0x0C04, 1)                 # +4  type 1, a program block
put(0x0C0C, 2)                 # +12 load page: the caller's module is at 0x1000
put16(0x0C12, 2)               # +18 pages
put16(0x0C14, 2)               # +20 pages ready
put(0x0C3F, 0)                 # +63 request block units floor

# --- the target's program block ----------------------------------------------
# nuprblen: units = pb[63] + 4 + (pb[57] & 0x0F), plus (callerRb[40] + 2) / 2
# because pb[56] & 0xA0 is not 0xA0.  With pb[63] = 1 and rb[40] = 0 that is
# 1 + 4 + 0 + 1 = SIX units, and nuptask adds ten more for the task block.
put16(0x0C40, EYE_PB)
put(0x0C44, 1)                 # +4  type 1
put(0x0C4C, 4)                 # +12 load page 4 -> the module is at guest 0x2000
put16(0x0C52, 1)               # +18 one page
put16(0x0C54, 1)               # +20 ... and it is ready, so nup1000 transfers
put24(0x0C70, 5)               # +48 the disk sector, which is the hash key
put(0x0C74, 0x00)              # +52 attribute: not privileged, not resident
put(0x0C78, 0x00)              # +56 mode: the module runs REAL
put(0x0C79, 0x00)              # +57 flags: bits 0x30 clear, so nuptask claims it
put(0x0C7F, 1)                 # +63 request block units floor

# --- the hash bucket, nucwpbsq's 0xD03 + 4 * (sector & 7) --------------------
# FindProgramBlock reads the three bytes ending there, so they start at 0xD01+4n.
put24(0x0D00 + 4 * (5 & 7) + 1, 0x0C40)

# --- SA21-9436 3-75's five-byte transfer control table entry ------------------
put24(0x0D40, 5)               # bytes 0-2 the program disk address, 1-based
put(0x0D43, 8)                 # byte 3    length in sectors
put(0x0D44, 0x00)              # byte 4    attributes

# --- the IPL task's request block --------------------------------------------
put16(0x0E00, EYE_RB)
put(0x0E02, 12)                # +2  length in 16-byte units
put(0x0E28, 0)                 # +40 no map table entries
put24(0x0E29, 0x0C00)          # +41 this block runs the program block at 0C00

# --- the caller's program ----------------------------------------------------
# SVC 10 is six bytes: F4 Q 10 then the entry's address and the entry point.
put(0x1000, 0xF4, 0x20, 0x10, 0x0D, 0x40, 0x00)   # async
put(0x1006, 0xF4, 0x21, 0x10, 0x0D, 0x40, 0x00)   # async + return control to caller
put(0x100C, 0xF4, 0x00, 0x1D, 0x40)               # post the task in XR1 with 40
put(0x1010, 0xF4, 0x00, 0x31)                     # ATASK, region 255 -> 2042 sectors
put(0x1013, 0xF4, 0x01, 0x31)                     # ATASK with Q bit 7: WAIT for space

# --- the target module -------------------------------------------------------
# Its header's +10..11 is zero, which is what makes nup2000 skip the entry point
# table and enter at the load address (c18a57dc).  The instruction is Sense Data
# Switches, which does nothing and cannot fail.
put(0x2000, 0xF4, 0x00, 0x09)

open(sys.argv[1], 'wb').write(img)
open(sys.argv[2], 'wb').write(code)
open(sys.argv[3], 'wb').write(module)
