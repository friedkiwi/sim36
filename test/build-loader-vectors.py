#!/usr/bin/env python3
"""Guest state for test/relocating-loader.sh - the SVC 52 exercise.

Three images, because guest 0x0E00 is the IPL task's request block and 0x0F00 is
its task block, and overwriting either detaches the task:

  0C00  relocating loader parameter lists, 17 bytes each
  1000  the program - two SVC 51 puts that stage a module and its relocation
        directory on the volume, then six SVC 52 calls
  3800  the module image and the relocation directory, staged in guest storage
        so that SVC 51 can put them on disk

The FIRST parameter list is IBM's own, transcribed byte for byte from
SA21-9436 3-146:

    010342000F00200000200000000F002000

together with the two statements the manual makes about it - "the sequential
sector address of the module is 010342" and "relocation of the subroutine is not
necessary since the load address and link address of the subroutine are
identical".  Those are the only assertions in this suite that owe nothing to
this project's own decode.

docs/s36/svc-relocating-loader.md
"""
import sys

BASE, END = 0x0C00, 0x0D00
CODE, CODE_END = 0x1000, 0x1040
STAGE, STAGE_END = 0x3800, 0x3A00

img = bytearray(END - BASE)
code = bytearray(CODE_END - CODE)
stage = bytearray(STAGE_END - STAGE)


def buf(addr):
    if addr >= STAGE:
        return stage, STAGE
    if addr >= CODE:
        return code, CODE
    return img, BASE


def put(addr, *bs):
    b, base = buf(addr)
    for i, v in enumerate(bs):
        b[addr - base + i] = v


def put16(addr, v): put(addr, (v >> 8) & 0xFF, v & 0xFF)
def put24(addr, v): put(addr, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def parmlist(at, disk, sectors, link, start, rld_offset, load):
    put24(at + 0, disk)          # +0..2   disk address
    put16(at + 3, sectors)       # +3..4   length in sectors
    put24(at + 5, link)          # +5..7   link-edit address
    put24(at + 8, start)         # +8..10  start control address
    put(at + 11, rld_offset)     # +11     relocation directory byte offset
    put16(at + 12, 0)            # +12..13 not decoded
    put24(at + 14, load)         # +14..16 load address


# Where the staged module and its relocation directory go on the volume.  Both
# are 1-based sector addresses, high enough to be clear of everything the IPL
# reads or writes.
MODULE_SECTOR = 600001
RLD_SECTOR = MODULE_SECTOR + 1

# --- the parameter lists ----------------------------------------------------

# 0C00: SA21-9436 3-146, byte for byte.
for i, b in enumerate(bytes.fromhex('010342000F00200000200000000F002000')):
    put(0x0C00 + i, b)

# 0C20: load to address, link == load, so no relocation.
parmlist(0x0C20, MODULE_SECTOR, 1, 0x3000, 0x3040, 0, 0x3000)

# 0C40: fetch to address, load 0x2000 above the link address, so the relocation
# directory runs and the start control address moves with the module.
parmlist(0x0C40, MODULE_SECTOR, 1, 0x1000, 0x1040, 0, 0x3000)

# 0C60: type 11, a memory resident overlay - refused.
parmlist(0x0C60, 0, 1, 0x1000, 0x1040, 0, 0x3000)

# 0C80: system fetch to address.  Same geometry as 0C40, but the task block's
# relocation factor and loader disk address are updated from it.
parmlist(0x0C80, MODULE_SECTOR, 1, 0x1000, 0x1040, 0, 0x3000)

# 0CA0: plain fetch.  No load address, so the address is the link address plus
# whatever relocation factor the system request above left in the task block.
parmlist(0x0CA0, MODULE_SECTOR, 1, 0x1000, 0x1040, 0, 0)

# --- the module and its relocation directory --------------------------------
#
# The module opens the way every System/36 load member does, and carries two
# two-byte address fields for the relocation directory to find: 0x1000 at
# offset 2..3 and 0x1234 at offset 10..11.
put(0x3800, 0xC2, 0x10)
put16(0x3802, 0x1000)
put(0x3804, 0xD4, 0xD6, 0xC4, 0xE4, 0xD3)      # "MODUL", EBCDIC
put(0x3809, 0x00)
put16(0x380A, 0x1234)
for i in range(0x0C, 0x100):
    put(0x3800 + i, 0x00)

# The directory: advance 3 and relocate (the halfword ENDING at offset 3),
# advance 8 and relocate (the halfword ending at offset 11), then stop.
put(0x3900, 0x03, 0x08, 0xFF)

# --- the program ------------------------------------------------------------
# SVC 51 is six bytes - type, key, sector count - and SVC 52 is four.
put(0x1000, 0xF4, 0x00, 0x51, 0x01, 0x00, 0x01)   # put the module
put(0x1006, 0xF4, 0x00, 0x51, 0x01, 0x00, 0x01)   # put the relocation directory
put(0x100C, 0xF4, 0x00, 0x52, 0x02)               # 3-146's own example
put(0x1010, 0xF4, 0x00, 0x52, 0x02)               # load to address, no relocation
put(0x1014, 0xF4, 0x00, 0x52, 0x06)               # fetch to address, relocated
put(0x1018, 0xF4, 0x00, 0x52, 0x11)               # memory resident overlay
put(0x101C, 0xF4, 0x00, 0x52, 0x0E)               # system fetch to address
put(0x1020, 0xF4, 0x00, 0x52, 0x04)               # fetch, using the factor

open(sys.argv[1], 'wb').write(img)
open(sys.argv[2], 'wb').write(code)
open(sys.argv[3], 'wb').write(stage)

# --- what the volume itself says --------------------------------------------
# The 3-146 example reads a real sector of the reference volume, so the expected
# bytes come from the image rather than from anything this emulator wrote.
with open(sys.argv[4], 'rb') as vol:
    vol.seek((0x010342 - 1) * 256)
    sector = vol.read(16)
with open(sys.argv[5], 'w') as out:
    out.write('IBM_SECTOR="%s"\n' % ' '.join('%02x' % b for b in sector))
    out.write('MODULE_SECTOR=%d\n' % MODULE_SECTOR)
    out.write('RLD_SECTOR=%d\n' % RLD_SECTOR)
