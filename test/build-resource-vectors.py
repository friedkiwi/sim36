#!/usr/bin/env python3
"""Guest state for test/resource-svcs.sh - SVC 20 and SVC 21.

Nothing here is an allocation queue element: the AQEs are the things under test
and the machine has to build them itself.  What this writes is only what a
caller has to supply - a resource queue header, a job control block, and two
extra tasks with somewhere to resume.

  0B00  job control block, 214 bytes.  Only +211..213 is used, and it starts
        empty, because SVC 21 is what fills it in.
  0C01  resource queue header, three bytes named by their last - so XR2 = 0C03,
        which is SA21-9436 3-108's own worked example address.
  0C40  task block "B", ID 0002, priority 40, request block 0CA0 / IAR 1100
  0D00  task block "C", ID 0003, priority 20, request block 0D60 / IAR 1200,
        with XR1 = 0F00 so its task post names the IPL task
  1000  program A, run by the IPL task (task block 0F00, request block 0E00)
  1100  program B
  1200  program C

docs/s36/svc-resource-allocation.md
"""
import sys

BASE, END = 0x0B00, 0x0E00
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
TB_ID, TB_STATE, TB_STAT2, TB_PRIO, TB_FLOOR, TB_RB = 2, 4, 5, 7, 20, 65
RB_PIAR, RB_PSR, RB_IAR, RB_XR1HI, RB_XR1LO = 13, 23, 24, 9, 26


def task(at, ident, priority, rb):
    put16(at, EYE_TB)
    put16(at + TB_ID, ident)
    put(at + TB_PRIO, priority)
    put(at + TB_FLOOR, priority)
    put(at + TB_RB, (rb >> 16) & 0xFF, (rb >> 8) & 0xFF, rb & 0xFF)


def request(at, iar, xr1=0):
    put16(at, EYE_RB)
    put(at + 2, 4)
    put(at + RB_PIAR, 0)
    put(at + RB_PSR, 0x01)
    put16(at + RB_IAR, iar)
    put(at + RB_XR1HI, (xr1 >> 16) & 0xFF)
    put16(at + RB_XR1LO, xr1 & 0xFFFF)


task(0x0C40, 0x0002, 0x40, 0x0CA0)
request(0x0CA0, 0x1100)

task(0x0D00, 0x0003, 0x20, 0x0D60)
request(0x0D60, 0x1200, 0x0F00)

# --- program A, the IPL task -------------------------------------------------
# The monitor-driven half: one supervisor call per `set iar` / `step 1`.
put(0x1000, 0xF4, 0x00, 0x21, 0x83)   # enqueue, level 3, no wait
put(0x1004, 0xF4, 0x00, 0x21, 0x83)   # ...again: one AQE, not two (3-106)
put(0x1008, 0xF4, 0x00, 0x21, 0x80)   # ...and again at level 0: same AQE, new level
put(0x100C, 0xF4, 0x00, 0x21, 0x03)   # dequeue - inline 1 bit 0 off
put(0x1010, 0xF4, 0x00, 0x21, 0x03)   # dequeue with nothing queued -> nonequal
put(0x1014, 0xF4, 0x00, 0x21, 0x84)   # inline 1 bit 5 (0x04) on a plain header: nursenqc2, bit stripped
put(0x1018, 0xF4, 0x20, 0x21, 0x83)   # Q bit 2 - critical system resource, refused
put(0x101C, 0xF4, 0x10, 0x21, 0xB3)   # enqueue by JCB (XR1), nested, level 3
put(0x1020, 0xF4, 0x10, 0x21, 0xB0)   # nest a level 0 enqueue over it
put(0x1024, 0xF4, 0x00, 0x20)         # SVC 20 - rebuild the job's active levels
put(0x1027, 0xF4, 0x00, 0x20)         # ...against a JCB that holds nothing
put(0x102A, 0xF4, 0x00, 0x11)         # SVC 11 - nupterm drains task-owned AQEs
put(0x1030, 0xF4, 0x00, 0x21, 0x84)   # inline 1 bit 5 with a DA eyecatcher at XR2: host device arm, refused

# --- program A's second half, the round trip ---------------------------------
put(0x1040, 0xF4, 0x00, 0x24, 0x40)   # 24  put B on the ready list
put(0x1044, 0xF4, 0x00, 0x24, 0x20)   # 24  put C on the ready list
put(0x1048, 0xF4, 0x01, 0x21, 0x83)   # 21  enqueue level 3 with wait -> A owns it
put(0x104C, 0xF4, 0x00, 0x1E, 0x08)   # 1E  wait -> the processor goes to B
put(0x1050, 0xF4, 0x00, 0x21, 0x03)   # 21  dequeue -> B can share, and is POSTED
put(0x1054, 0xF4, 0x00, 0x1E, 0x08)   # 1E  wait -> back to B

# --- program B ---------------------------------------------------------------
put(0x1100, 0xF4, 0x01, 0x21, 0x83)   # 21  the same resource, exclusive, with wait
put(0x1104, 0xF4, 0x00, 0x21, 0x03)   # 21  dequeue, once it has been granted
put(0x1108, 0xF4, 0x00, 0x1E, 0x08)   # 1E  wait

# --- program C ---------------------------------------------------------------
put(0x1200, 0xF4, 0x00, 0x1D, 0x08)   # 1D  post A, so A is ready when B waits
put(0x1204, 0xF4, 0x00, 0x1E, 0x08)   # 1E  wait - and now nothing is ready

open(sys.argv[1], 'wb').write(img)
open(sys.argv[2], 'wb').write(code)
