#!/usr/bin/env python3
"""Guest state for test/task-dispatcher.sh - three extra tasks and their programs.

Nothing here is invented state: every field written is one the dispatcher chain
reads, and each is named with the routine that reads it.

  0C00  task block  "B", ID 0002, priority 40 - the task A hands the processor to
  0C60  request block for B, instruction address 1100 and XR1 = 0F00
  0CC0  task block  "C", ID 0003, priority 80 - a quick-lock waiter
  0D20  request block for C, instruction address 1200
  0D80  task block  "D", ID 0004, priority 30 - the same qlock, lower priority
  0DE0  request block for D
  1000  program A, which runs as the IPL task (task block 0F00, request block 0E00)
  1100  program B
  1200  program C

Guest 0x0E00 is the IPL task's request block and 0x0F00 its task block, which is
why the blocks stop at 0x0DFF.  docs/s36/task-dispatcher.md
"""
import sys

BASE, END = 0x0C00, 0x0E00
CODE, CODE_END = 0x1000, 0x1300
img = bytearray(END - BASE)
code = bytearray(CODE_END - CODE)


def put(addr, *bytes_):
    buf, base = (code, CODE) if addr >= CODE else (img, BASE)
    for i, b in enumerate(bytes_):
        buf[addr - base + i] = b


def put16(addr, v):
    put(addr, (v >> 8) & 0xFF, v & 0xFF)


EYE_TB, EYE_RB = 0xE3C2, 0xD9C2

# Task block offsets, decimal, from docs/s36/guest-structures.md.
TB_ID, TB_STATE, TB_STAT2, TB_DEFER, TB_PRIO = 2, 4, 5, 6, 7
TB_WMASK, TB_QLOCK, TB_FLOOR, TB_RB = 8, 14, 20, 65
RB_PIAR, RB_PSR, RB_IAR, RB_XR1HI, RB_XR1LO = 13, 23, 24, 9, 26


def task(at, ident, priority, rb, state=0, stat2=0, wmask=0, qlock=0):
    put16(at, EYE_TB)                 # nupotb refuses a block without it
    put16(at + TB_ID, ident)          # nuidfind walks queue 39 comparing this
    put(at + TB_STATE, state)         # 0x80 waiting - nuwartn sets, nupotb clears
    put(at + TB_STAT2, stat2)         # TB_STAT2, the wait conditions
    put(at + TB_PRIO, priority)       # the sort key both task queues order on
    put(at + TB_FLOOR, priority)      # the floor nuprqvlw will not go below
    put16(at + TB_WMASK, wmask)       # TB_WMASK, what SVC 01 and SVC 30 clear
    put16(at + TB_QLOCK, qlock)       # nuqlock's search argument, tb+14..15
    put(at + TB_RB, (rb >> 16) & 0xFF, (rb >> 8) & 0xFF, rb & 0xFF)


def request(at, iar, xr1=0):
    put16(at, EYE_RB)
    put(at + 2, 4)                    # +2 length in 16-byte units
    put(at + RB_PIAR, 0)              # untranslated
    put(at + RB_PSR, 0x01)            # Equal, the post-reset PSR
    put16(at + RB_IAR, iar)           # THE instruction address the task resumes at
    put(at + RB_XR1HI, (xr1 >> 16) & 0xFF)
    put16(at + RB_XR1LO, xr1 & 0xFFFF)


# B is ready and runnable; its saved XR1 already names the IPL task block, so the
# post it issues proves the register switch as well as the dispatch.
task(0x0C00, 0x0002, 0x40, 0x0C60)
request(0x0C60, 0x1100, 0x0F00)

# C and D are both parked in a general wait (TB_STAT2 = 0x20, tb+4 = 0x80) on the
# quick lock failure condition - TB_WMASK inline parameter 2 bit 0x02, SA21-9436
# 3-69 - and both name the same qlock. C outranks D.
task(0x0CC0, 0x0003, 0x80, 0x0D20, state=0x80, stat2=0x20, wmask=0x0002, qlock=0x1234)
request(0x0D20, 0x1200)

task(0x0D80, 0x0004, 0x30, 0x0DE0, state=0x80, stat2=0x20, wmask=0x0002, qlock=0x1234)
request(0x0DE0, 0x1300)

# --- program A, the IPL task -------------------------------------------------
put(0x1000, 0xF4, 0x00, 0x24, 0x40)         # 24  queue B on 39 AND 40 (it is ready)
put(0x1004, 0xF4, 0x00, 0x1E, 0x02)         # 1E  wait on 02 -> the processor goes to B
put(0x1008, 0xF4, 0x00, 0x24, 0x80)         # 24  queue C on 39 only (it is waiting)
put(0x100C, 0xF4, 0x00, 0x24, 0x30)         # 24  queue D on 39 only
put(0x1010, 0xF4, 0x00, 0x30)               # 30  QLOCK, WR6 = 1234
put(0x1013, 0xF4, 0x00, 0x25)               # 25  ready check on D - not an event wait
put(0x1016, 0xF4, 0x00, 0x17, 0x02)         # 17  async wait D  - nuwasusp refuses it
put(0x101A, 0xF4, 0x00, 0x17, 0x02)         # 17  async wait B  - accepted
put(0x101E, 0xF4, 0x00, 0x00, 0x00, 0x80)   # 00  general wait, SA21-9436 3-69's own
put(0x1023, 0xF4, 0x00, 0x09)               #     example value: printer allocate

# --- program B ---------------------------------------------------------------
put(0x1100, 0xF4, 0x00, 0x1D, 0x02)         # 1D  post the task named by XR1 (0F00)
put(0x1104, 0xF4, 0x00, 0x09)

# --- program C ---------------------------------------------------------------
put(0x1200, 0xF4, 0x00, 0x1E, 0x08)         # 1E  wait - and now nothing is ready

open(sys.argv[1], 'wb').write(img)
open(sys.argv[2], 'wb').write(code)
