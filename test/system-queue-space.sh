#!/bin/sh
# The system queue space - SVC 06 Assign and SVC 07 Free Assigned Areas, and the
# buddy allocator underneath them.
#
# Every address asserted here is a PREDICTION, not a transcript: it follows from
# NuEmulatorHeap's own rules and from nothing else. The pool is seeded exactly as
# the constructor seeds it (c1873874: enqueueHeap(base + 112, size - 112)), which
# decomposes into one free element per power of two; the size classes and the
# 256-byte rounding are initHeapInfo's (c1873e50); allocation takes the head of
# the lowest class that fits (c18739a0); and free coalesces with its buddy at
# `offset XOR size` (c1873c64). Work the arithmetic and the addresses below are
# forced.
#
# docs/s36/system-queue-space.md
set -e
cd "$(dirname "$0")/.."
. test/gate-common.sh

python3 test/build-sqs-vectors.py "$TMP/program.bin"

cat > "$TMP/run.sim" <<EOF
do $PWD/test/advanced36-nolisten.sim
attach disk0 $SIM36_VOLUME ro
ipl pause
loadfile $TMP/program.bin 1000
trace csp
set pxr1 00
# --- 1. the 16-byte class, which the seed leaves at the very top of the pool
set xr1 0010
set iar 1000
step 1
show cpu
# --- 2. a second 16: class 0 is empty, so class 3's 128 is split -----------
set xr1 0010
set iar 1000
step 1
show cpu
# --- 3. give the first one back: it merges all the way back to 128 ---------
set xr1 FFF0
set wr6 0010
set iar 1003
step 1
# --- 4. ...which a 128-byte assign now finds, and could not before ---------
set pxr1 00
set xr1 0080
set iar 1000
step 1
show cpu
# --- 5. free it and ask for 100: the class is 128, so the same area comes back
set xr1 FF80
set wr6 0080
set iar 1003
step 1
set xr1 0064
set iar 1000
step 1
show cpu
# --- 6. 600 bytes: class 1024, but carved as 768 - initHeapInfo's 256 rule -
set xr1 FF80
set wr6 0064
set iar 1003
step 1
set xr1 0258
set iar 1000
step 1
show cpu
# --- 7. above the largest class there is no answer at all ------------------
set xr1 9C40
set iar 1000
step 1
show cpu
# --- 8. nufree's own checks: not on a 16-byte boundary ---------------------
set xr1 FF78
set wr6 0010
set iar 1003
step 1
# --- 9. ...and below 8192 -------------------------------------------------
set xr1 1000
set wr6 0010
set iar 1003
step 1
# --- 10. a double free is error 514, not a corrupted chain ----------------
set xr1 FF70
set wr6 0010
set iar 1003
step 1
set xr1 FF70
set wr6 0010
set iar 1003
step 1
# --- 11. NuEmulatorHeap's allocation-failure override extends, then retries -
# Consume the initial class-32768 element. A second request of the same class
# cannot be answered from the fragmented initial segment, so c1873930 calls
# extendHeap (c1874260), which commits the next 64 KB and retries searchHeap.
set pxr1 00
set xr1 8000
set iar 1000
step 1
set pxr1 00
set xr1 8000
set iar 1000
step 1
sqsstate
# The snapshot round trip of the extended heap (native state outside MSP
# memory) is exercised by test/system-queue-space-snapshot.sh once snapshots
# exist (milestone 7).
# The Advanced/36's native ceiling is 0x006F0000.  Force four more misses so
# this crosses the emulator's former 0x040000 module-arena boundary; resident
# module backing now lives above the native queue-space ceiling instead.
set pxr1 00
set xr1 8000
set iar 1000
step 1
set pxr1 00
set xr1 8000
set iar 1000
step 1
set pxr1 00
set xr1 8000
set iar 1000
step 1
set pxr1 00
set xr1 8000
set iar 1000
step 1
sqsstate
quit
EOF

out=$("$SIM36" -c "$TMP/run.sim" 2>&1)
[ -n "$VERBOSE" ] && echo "$out"


# The seed. 57344 - 112 = 57232 bytes decompose into one element per set bit,
# laid out descending from the base, so the 16-byte element is the last 16 bytes
# of the pool and the 128-byte element the 128 below it.
# The capacity is the point of this check, not how much happens to be in use:
# the control processor now assigns the system console unit block at IPL, so the
# pool is not empty by the time anything else asks. Pinning "16 of" made an
# unrelated allocation break this suite.
check "06  the pool is 57232 assignable bytes     " 'of 57232 used, peak'
check "06  the 16-byte class is the top of the pool" 'assign 16 -> 16 bytes at guest FFF0 from class 16'
check "06  ...and XR1 carries it back             " 'XR1 FFF0'
check "06  the next 16 splits the 128 element     " 'assign 16 -> 16 bytes at guest FF70 from class 128'

# FFF0 is pool offset 57328. 57328^16 = 57312, free; then 57312^32 = 57280,
# free; then 57280^64 = 57216, free - the three pieces the previous split left.
# 57216^128 = 57088 is inside the 256-byte element, not an element, so it stops.
check "07  a free coalesces with its buddy chain  " 'free 16 -> 16 bytes at guest FFF0'
check "06  and 128 bytes now exist at FF80        " 'assign 128 -> 128 bytes at guest FF80 from class 128'

# initHeapInfo: 100 rounds to the 128 class, and 128 < 100 + 256, so 128 bytes
# are carved - the same area, byte for byte.
check "06  100 bytes is a 128-byte class          " 'assign 100 -> 128 bytes at guest FF80 from class 128'
check "07  plain free rounds only to 16 bytes     " 'SVC 07: free 100 -> 112 bytes at 00FF80'

# 600's class is 1024, and 1024 >= 600 + 256, so the carve is (600+255)&~255.
check "06  600 bytes is carved as 768 (c1873ec8)  " 'assign 600 -> 768 bytes at guest F870 from class 1024'

# 40000 > 32768, so searchHeap starts past its own limit of 11 and never looks.
check "06  40000 bytes is above the largest class " 'sqs: the system queue space cannot assign 40000 byte(s): that is above the 32768-byte largest size class'
check "06  ...and the architected answer is zero  " 'SVC 06: assign 40000 bytes -> XR1 = 000000'

# NuEmul::nufree c18e2f98, c18e2fa8.
check "07  an unaligned free is nuerabt 52        " 'free of FF78 is not on a 16-byte boundary'
check "07  a free below 8192 is nuerabt 52        " 'free of 1000 is below 8192 or on a 64 KB boundary'

# NuHeap::deallocateHeap c188529c: the block still carries FQ and isFreeElement
# finds it on a list.
check "07  a double free is NuHeap::error 514     " 'FF70 is already free - NuHeap::error 514'

# NuEmulatorHeap::allocateHeapFailed c1873930 invokes its vtable +0x58 override.
# extendHeap clears [old-limit, old-limit+64K), reserves its first 16 bytes,
# enqueues 0xFFF0 bytes at old-limit+16 and advances this+0x50.
check "06  allocation miss invokes native extension" 'NuEmulatorHeap::extendHeap committed 010000..01FFFF'
check "06  extension reserves its first 16 bytes  " 'assign 32768 -> 32768 bytes at guest 10010 from class 32768'
check "    extended heap ownership remains valid  " 'system queue 2000..20000: 66576/122752 used'
check "    extended heap audit remains clean       " 'invariants OK'
check "    heap crosses former 040000 ceiling     " 'NuEmulatorHeap::extendHeap committed 040000..04FFFF'
check "    native-range growth remains valid      " 'system queue 2000..60000:'

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
