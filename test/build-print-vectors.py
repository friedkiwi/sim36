#!/usr/bin/env python3
"""Guest state for test/print-buffer.sh - the SVC 26 and SVC 36 exercise.

The addresses are SA21-9436 3-114's own: "The IOB address is hex 004540 and the
print buffer is at hex 006000 ... Program A moves the data to be printed to
address hex 006006".  The field list is the manual's too - flag byte, buffer
address, length, control byte, skip before and space after, forms length,
printer unit block address, current line - and the SLIC routines place each of
them (docs/s36/svc-prepare-print-buffer.md).

  4540  the print IOB, output SPOOLED
  4580  a second print IOB, output DIRECT to a printer, so the unit block is
        updated
  4600  the printer unit block
  4680  a third print IOB, ideographic (2-byte) data, output SPOOLED
  4700  its printer unit block
  6000  the first print buffer, data at +6
  6100  the second print buffer, data at +6
  6200  the third (ideographic) print buffer, data at +6
  1000  the program
"""
import sys

BLOCKS, BLOCKS_END = 0x4540, 0x4740
BUFS, BUFS_END = 0x6000, 0x6300
CODE, CODE_END = 0x1000, 0x1010

blocks = bytearray(BLOCKS_END - BLOCKS)
bufs = bytearray(BUFS_END - BUFS)
code = bytearray(CODE_END - CODE)


def buf(addr):
    if addr >= BUFS:
        return bufs, BUFS
    if addr >= BLOCKS:
        return blocks, BLOCKS
    return code, CODE


def put(addr, *bs):
    b, base = buf(addr)
    for i, v in enumerate(bs):
        b[addr - base + i] = v


def put16(addr, v): put(addr, (v >> 8) & 0xFF, v & 0xFF)
def put24(addr, v): put(addr, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


# The data: two characters, five blanks - "more than three contiguous blanks" -
# a character, one byte below hex 40, and a last character.  Ten bytes, and each
# clause of 3-113's scan paragraph gets exercised once.
DATA = [0xC1, 0xC2, 0x40, 0x40, 0x40, 0x40, 0x40, 0xC3, 0x05, 0xC4]

FORMS_LENGTH = 66
START_LINE = 10


# The ideographic case: an SO (hex 0E) enters 2-byte mode, two ward/point pairs
# follow, and an SI (hex 0F) leaves it.  Every data byte is at or above hex 40 so
# none is compressed or turned into hex FF - the point is that the pair bytes,
# and the two shift codes, are copied through verbatim.  Six bytes.
DATA_IDEO = [0x0E, 0x42, 0x43, 0x44, 0x45, 0x0F]


def iob(at, buffer, spooled, pub, data=DATA):
    put(at + 7, 0x02 if spooled else 0x00)   # $IOBPFLO, bit 02 = spooled
    put24(at + 13, buffer)                   # $IOBPDAT, and it must be real
    put16(at + 16, len(data))                # $IOBPLNG
    put24(at + 21, pub)                      # $IOBPPUB
    put(at + 24, FORMS_LENGTH)               # $IOBPFML
    put(at + 25, START_LINE)                 # $IOBPCLN
    put(at + 26, 0x40)                       # $IOBPCTL, bit 40 = print operation
    put(at + 27, 1)                          # skip before printing, to line 1
    put(at + 28, 0)                          # space before printing
    put(at + 29, 0)                          # skip after printing
    put(at + 30, 1)                          # $IOBPSPA, space after printing
    put(at + 31, 0xEE)                       # $IOBP#FF, overwritten by the call
    put(at + 32, 0xEE)                       # $IOBP#AF, overwritten by the call
    for i, b in enumerate(data):
        put(buffer + 6 + i, b)


iob(0x4540, 0x6000, spooled=True, pub=0x4600)
iob(0x4580, 0x6100, spooled=False, pub=0x4600)
iob(0x4680, 0x6200, spooled=True, pub=0x4700, data=DATA_IDEO)

# The printer unit block, with values the call must replace.
put(0x4600 + 69, 0xEE)
put(0x4600 + 70, 0xEE)

# SVC 26 and SVC 36 take no inline parameters, so each is three bytes.
put(0x1000, 0xF4, 0x00, 0x26)      # spooled
put(0x1003, 0xF4, 0x00, 0x26)      # direct to a printer
put(0x1006, 0xF4, 0x20, 0x36)      # SMFC, Q bit 2 on: receive error
put(0x1009, 0xF4, 0x00, 0x36)      # SMFC, Q bit 2 off: transmit error
put(0x100C, 0xF4, 0x00, 0x26)      # ideographic, spooled

open(sys.argv[1], 'wb').write(blocks)
open(sys.argv[2], 'wb').write(bufs)
open(sys.argv[3], 'wb').write(code)
