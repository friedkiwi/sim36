# System/36 BASIC extended-control-storage assist

This document records the contract recovered for Advanced/36 XFER `02,00`.
It separates facts demonstrated by V4R4 SLIC and the supplied panic image from
interpretations that still need a trace or a second implementation.  The
emulator implementation is original C++; IBM code and images are research
inputs only and are not runtime dependencies.

## Evidence and reproduction

The primary image was
`sim36-panic-20260918-003017-5263eb3f.zip`, SHA-256
`c330899a6e7661d7e78439de0d401645a24a12eb9b143b0983a0e3427a98ff07`.
The relevant V4R4 routines are `NuBasic::nubl1` at `c1844040`, its named
arithmetic helpers through `c184a130`, `NuEmul::nuecs` at `c18e3230`, and the
three nubl1 dispatch tables rooted at TOC object `c32fb040`.  Commands used to
repeat the static work include:

```text
python3 /home/friedkiwi/src/syspass_research/tools/slicfn.py nubl1 --release v4r4 --tsv
python3 /home/friedkiwi/src/syspass_research/tools/slicfn.py processAddSubt --release v4r4 --tsv
python3 /home/friedkiwi/src/syspass_research/tools/slicfn.py processMultiply --release v4r4 --tsv
python3 /home/friedkiwi/src/syspass_research/tools/slicfn.py processDivide --release v4r4 --tsv
```

Repeat the local full-session acceptance (the IBM volume is intentionally not
a distributable default-test dependency) with:

```sh
env SIM36="$PWD/build/sim36" \
  SIM36_VOLUME="$PWD/images/volumes/as36.img" S36_PORT_BASE=24420 \
  S36_BASIC_COMMAND=1 \
  S36_BASIC_STATEMENT_FILE="$PWD/test/basic-acceptance-lines.txt" \
  S36_BASIC_EXPECT_FILE="$PWD/test/basic-acceptance-expect.txt" \
  python3 test/ipl-main-session.py --basic-research
```

The System/34 and System/32 Scientific Microinstructions Functions Reference,
SA21-9275-0, documents the ancestor three-byte instruction set and register
model.  System/36 SA21-9436-5 and SC21-7908-3 document the MSP/XFER and guest
architecture.  The System/36 publication catalogue confirms the BASIC manuals
Programming with BASIC (SC21-9003), BASIC Summary (SC21-9012), and BASIC
Messages (SC21-7943).  Those three volumes were not present in the local
research collection; syntax claims must therefore also be checked against an
available copy or the installed product help before being called proven.

Confidence labels below mean **proven** (direct load/store/control-flow evidence
and, where possible, live bytes), **strong inference** (one interpretation fits
all observed code), and **unknown** (do not build a silent behavior on it).

## XFER contract and architectural state

**Proven.** XFER leaves the MSP through its normal saved-state path.  `nuecs`
selects NuFortran for Q=`01`, NuBasic for Q=`02`, and `nuerr2(61)` otherwise;
NuBasic is valid for R=`00`.  The call is synchronous and is not a scheduler
event.

The current request block and task block are supplied by the control-storage
dispatcher.  The BASIC control block address is the saved XR1, including its
saved prefix byte.  NuBasic snapshots and later restores only:

| Request-block field | Register |
|---:|---|
| `+23` | PSR |
| `+24` | IAR |
| `+30` | ARR |
| `+9`, `+26` | XR1 prefix and low half |
| `+11`, `+28` | XR2 prefix and low half |
| `+34` | WR5 |

WR4, WR6, and WR7 are not part of NuBasic's private save.  On every examined
normal, continuation, and error exit `nuebrest` also applies `TB+4 &= 0xF5`.
`Completed` consequently means “the requested guest continuation is fully
prepared,” not necessarily “one BASIC statement has finished.”

## BASIC control block and persistent machine state

Offsets are decimal unless prefixed with `0x`.  Values are the supplied panic.

| Offset | Value | Contract | Confidence |
|---:|---:|---|---|
| `0` | `00` | mode flags; selects 5- versus 9-byte numeric values | proven |
| `17` | `5C00` | stream boundary/base; exact boundary rule | strong inference |
| `19` | `5C00` | current absolute 16-bit internal IP | proven |
| `23` | `5400` | evaluation arena base | strong inference |
| `25` | `5400` | upward-growing evaluation stack pointer | proven |
| `27` | `5ABF` | evaluation stack upper bound | proven |
| `29` | `5AC0` | secondary/control arena boundary | strong inference |
| `31` | `5AC0` | mutable secondary/control pointer | proven |
| `33` | `5BFF` | secondary/control arena upper bound | proven |
| `35` | `0296` | first class-2 guest exit vector | proven |
| `35+2*n` | varies | 16 halfword vectors used by `2n` operations | proven |
| `67` | varies | unresolved lazy-transfer continuation | proven |
| `71` | varies | evaluation-stack range continuation | proven |
| `73` | varies | control-stack underflow continuation | proven |
| `75` | varies | control-stack overflow continuation | proven |
| `81` | varies | empty/zero control-return continuation | proven |
| `85` | varies | FOR-loop gate continuation | proven |
| `87` | varies | numeric overflow continuation | proven |
| `89` | varies | numeric underflow continuation | proven |
| `91` | varies | divide-by-zero continuation | proven |
| `93` | varies | string-range continuation | proven |
| `97` | varies | class-0/1 guest continuation | proven |

`blputprm` commits exactly IP to `+19`, evaluation SP to `+25`, and the
secondary pointer to `+31`.  Persistent stacks live in guest storage; the
assist must not retain host pointers between calls.  In the panic, translated
XR1 `80:3B00` resolves to the control block and logical `5C00` resolves to the
stream.  Address arithmetic is 16-bit in a translated task region and follows
normal page translation/wrapping.

Numeric stack entries occupy 5 bytes in short mode and 9 in long mode.  The
secondary/control stack uses eight-byte upward-growing records on the paths
examined.  One proven record has marker `FF`, current IP, an operand-derived
halfword, and the current evaluation SP; the source-level purpose of every
record variant remains unknown.

Evaluation strings have the proven expanded layout
`[length][characters:length][length]` and occupy `length+2` bytes.  A persistent
string descriptor is `[capacity][length][characters:length]`; class D names the
capacity byte and presents the following length byte to the common operation
table.  A compact stack lvalue is `[capacity][BE16 destination]`.  Strings
remain guest-owned; no SLIC allocator or retained host pointer is involved.
Maximum length is 255.  Comparisons use the byte at CB `+115` to pad the shorter
operand before unsigned bytewise comparison.

## Instruction decoding

**Proven.** Fetch one byte at the absolute logical IP, increment IP by one,
split the byte into high and low nibbles, use the high nibble to prepare an
operand/value, then use the low nibble as an operation.  It is not a flat
256-opcode set and not, in general, the old three-byte scientific encoding.

| High nibble | Operand/control class |
|---:|---|
| `0`, `1` | direct control/parameter forms; six bytes on the observed continue path |
| `2` | guest exit/vector operation |
| `3` | control-frame and branch table |
| `4`, `6` | invalid |
| `5` | six inline operations (`0..5`), including current-reference push |
| `7` | Boolean stack operations (`4` AND, `5` OR, `6` NOT/XOR 1) |
| `8` | pop one numeric width and use its stack address as operand |
| `9` | BE16 absolute operand follows; total length 3 |
| `A`, `B` | one- and two-dimensional numeric arrays |
| `C` | pop an expanded string and use its base as the effective operand |
| `D` | BE16 persistent-string descriptor; effective address is descriptor+1 |
| `E`, `F` | one- and two-dimensional string arrays |

The numeric low-nibble table is:

| Low | Operation | Stack/state effect |
|---:|---|---|
| `0` | push reference | push BE16 effective address |
| `1` | load | push 5/9 bytes from effective address |
| `2` | assignment via stacked reference | pop destination reference, copy value |
| `3` | store | copy top value to effective address, pop value |
| `4` | add | binary numeric reduction |
| `5` | subtract | binary numeric reduction |
| `6` | multiply | binary numeric reduction |
| `7` | divide | binary numeric reduction |
| `8` | less than | push Boolean byte |
| `9` | less than or equal | push Boolean byte |
| `A` | greater than | push Boolean byte |
| `B` | greater than or equal | push Boolean byte |
| `C` | not equal | push Boolean byte |
| `D` | equal | push Boolean byte |
| `E` | negate | push a sign-toggled copy; source is unchanged |
| `F` | signed-integer conversion | push BE16, rounded half away from zero |

For class `3`, `3C`/`3E` load an inline target into saved guest IAR and exit,
`3D` restores guest IAR from the entry ARR and loads XR2 from the inline word,
and `3F` is an unconditional internal branch to the inline BE16 target.
`30` points at a mutable lazy-transfer record.  Its low-nibble key is looked up
in the chain rooted at CB `+17`; a match is cached as
`80,target-high,target-low` and transfers directly to `target`.  A miss leaves
IP at the record and exits through the guest continuation at CB `+67`.
`31` pushes an eight-byte record: marker `FF`, the current IP, one preserved
byte, the word at IP+4, and the evaluation SP.  It advances the control pointer
by eight and IP by six before a computed transfer.  `35` pops one Boolean and
chooses the first or second four-byte transfer record.  `36` compares a FOR
variable with its descriptor limit, updates the descriptor active bit, and
selects one of two four-byte transfer records without displacement.  `37`
tests an indirect FOR gate; a clear gate repeats through the guest continuation
at CB `+85`, while a set gate takes its internal transfer.  `38` pops one
control record and either performs its displacement-
enabled return transfer or uses the underflow/empty continuations.  An
unresolved return descriptor exits through CB `+69`, rather than the
no-displacement resolver's CB `+67`.  `3A` and `3B` are invalid.

The common string low table is:

| Low | Operation | Stack/state effect |
|---:|---|---|
| `0` | lvalue | push `[capacity,BE16 effective]` |
| `1` | expand/load | push `[length,characters,length]` |
| `2` | assign direct source | pop compact lvalue and copy/truncate-or-error |
| `3` | assign expanded source | pop expanded value and copy/truncate-or-error |
| `4` | substring/slice | consumes two rounded numeric arguments; edge rules pending |
| `5` | invalid | error 61 |
| `6` | concatenate | append to expanded left value; maximum 255 |
| `7` | length | push a 5/9-byte guest numeric value |
| `8..D` | comparisons | same predicate order as numeric; push Boolean |

For assignment and concatenation, CB byte `+4 == 1` permits truncation;
otherwise excess length exits through CB `+93`.

Class-5 operations are specialized numeric stack forms.  `52` truncates the
top 5/9-byte value toward an integer in place: it retains the sign/exponent and
whole base-100 digits, zeroes fractional digits, and leaves SP unchanged.

Array descriptors contain BE16 base at `+0`, first-dimension byte extent (and
second-dimension stride) at `+2`, and a nonzero second-dimension count at `+4`.
String arrays additionally contain element capacity at `+8`.  Task byte 0 bit
`80` selects lower bound zero; otherwise the lower bound is one.  Indices use
the exact `9F` rounding conversion.  With numeric width `W`, lower bound `LB`,
and string record size `capacity+1`, the effective-address formulas are:

```text
A: base + W*i
B: base + W*i + (j-LB)*stride
E: base + (capacity+1)*i
F: base + (capacity+1)*i + (j-LB)*stride
```

The first byte displacement must be strictly below `+2`; `j-LB` must be
strictly below `+4`; each multiplication must fit 16 bits.  Final additions
wrap naturally.  Range errors use the task vector at `+0x53`; a zero `+4`
uses the distinct undefined-array vector at task `+0x5f`.

The panic begins:

```text
5C00  00 00 10 00 5C 20
5C06  31 C0 5C ...
5C61  0F FF FF 32 00 00
5C67  91 3A DA
5C6A  96 5C E4
```

This validates a six-byte class-0 form, stateful class-31 control transfer, and
the three-byte `91` load and `96` multiply forms.  A decoder that blindly treats
every instruction as either one or three bytes desynchronizes immediately.

## Decimal numeric records

**Proven except for the explicitly labelled value formula.** A numeric record
is 5 or 9 bytes.  Byte zero contains sign in `0x80` and a seven-bit base-100
exponent.  The remaining 4 or 8 bytes are unsigned base-100 digits `0..99`,
most significant first.  Zero is all zero and a normalized nonzero value has a
nonzero first digit.

The formula strongly inferred from multiply/divide exponent handling is:

```text
(-1)^sign * (d1/100 + d2/100^2 + ...) * 100^(exponent - 64)
```

Live constants corroborate the formula: `41 01 00 00 00` is 1,
`40 32 00 00 00` is 0.5, and `42 01 00 00 00` is 100.

Arithmetic rounds to 6 significant decimal digits in short mode and 14 in long
mode, even though the records have 8/16 digit capacity.  If the leading
base-100 digit is at least 10, the discarded whole base-100 digit rounds upward
at 50.  If it is below 10, the discarded decimal digit rounds upward at 5.
Ties therefore round away from zero, not to even.  Carry propagates in base 100
and may increment the exponent.  Add/subtract align base-100 exponents,
multiply is schoolbook base-100, and divide is long base-100.  Exact zero is
canonicalized positive.

`9F` is the SLIC `blstrnd` conversion: characteristics below `40` give zero;
`40..42` use the first three base-100 digits and round half away from zero; a
larger characteristic returns signed sentinel `+/-0x4000`.  Array modes apply
their own bounds after conversion.

Underflow substitution writes an all-zero record.  Overflow and divide-by-zero substitution is
`7F 63 63 63 63` (plus four more `63` bytes in long mode).  Runtime-control
bytes at CB `+1`, `+2`, and `+3` select substitution and/or the overflow,
underflow, and divide-by-zero continuations.  Mode 1 substitutes and resumes;
mode 2 takes the continuation without substitution; other modes substitute and
then take the continuation.  These are recoverable guest paths, not fatal
`GuestError`s.

## Interactive acceptance

The local SSP volume was exercised through the unmodified BASIC menus and
compiler.  These commands enter internal bytecode through XFER `02,00`; the
emulator never sees or parses BASIC source.  The following all returned to an
invited, unlocked row-23 input field:

```text
PRINT 1          -> 1
PRINT 1+2        -> 3
PRINT RND(1)     -> .397863   (representative run)

10 PRINT "HELLO FROM BASIC"
20 A=1+2
30 IF A=3 THEN 50
40 PRINT "BRANCH FAILED"
50 PRINT A
60 END
RUN              -> HELLO FROM BASIC, then 3, then BAS-5033 at line 60

10 FOR I=1 TO 3
20 PRINT I
30 NEXT I
40 END
RUN              -> 1, 2, 3, then BAS-5033 at line 40
```

The stored program demonstrates line entry, string output, arithmetic,
conditional branching, FOR/NEXT looping with the syntax accepted by the
installed System/36 product, clean program termination, and return to the prompt.
The checked-in fixture is `test/basic-acceptance-lines.txt`; one expectation
line per statement is supplied separately so no BASIC-legal delimiter is used.
The loop is in `test/basic-loop-acceptance-lines.txt`.

The `RND` regression fixture `test/basic-rnd-loop-lines.txt` performs 500
iterations of the reported conditional-print loop and then terminates.  Run it
against the local SSP volume with:

```sh
env SIM36="$PWD/build/sim36" \
  SIM36_VOLUME="$PWD/images/volumes/as36.img" S36_PORT_BASE=24430 \
  S36_BASIC_COMMAND=1 \
  S36_BASIC_STATEMENT_FILE="$PWD/test/basic-rnd-loop-lines.txt" \
  S36_BASIC_STATEMENT_WAIT=30 \
  python3 test/ipl-main-session.py --basic-research
```

This produces both `/` and `\\`, ends with the expected `BAS-5033` at line
40, and returns to the invited prompt.  The original unbounded program exposed
an erroneous decode of `52`: popping a two-byte address leaked two stack bytes
per `RND` evaluation until SSP reported `BAS-5035`.  V4R4 code at
`c1844e2c..c1844eac` proves that `52` instead truncates the top numeric value
in place without changing SP.  The same live stream proves that SSP emits
numeric opcode `98` for source `<`; this corrected the relational table and
restored the mixed branch output seen on native SLIC.

The unbounded form is also the workstation Attention regression.  Attention
is an out-of-band RFC 1205 `NoOperation` with flag `40`, not an AID record.
It is kept out of the field-input park and delivered through the native
`TU+8E=F2`/internal-condition path, which SSP classifies as request `8202` and
routes to `#CPT2`.  This command verifies that a running BASIC job opens the
SSP `INQUIRY OPTIONS` panel:

System Request is the other ordinary operator key on this out-of-band path:
RFC flag `04` maps to controller function `F0` and SSP request `8000`/`#CPT3`.
The RFC `Test Request` and `Help in error state` bits are diagnostic/error
controls with distinct guest routes; they are not aliases for Attention.
Enter, command/PF keys, Clear, Help, Roll and Record Backspace remain normal
5250 AIDs.  Reset is local terminal state and sends no guest key.  In the
stock `tn5250` client Attention is Ctrl-A (or Escape then uppercase A), and
System Request is Ctrl-C (or Escape then uppercase S).

```sh
env SIM36="$PWD/build/sim36" \
  SIM36_VOLUME="$PWD/images/volumes/as36.img" S36_PORT_BASE=24440 \
  S36_BASIC_COMMAND=1 S36_BASIC_ATTENTION=1 \
  S36_BASIC_STATEMENT_FILE="$PWD/test/basic-attention-lines.txt" \
  python3 test/ipl-main-session.py --basic-research
```

A representative trace excerpt is:

```text
asstb BASIC entry XFER 02,00 ... IP=5D16 SP=5400 control=5AC0 width=5
asstb BASIC op IP=5D16 opcode=91 class=9 op=1 SP=5400 ...
asstb BASIC op done IP=5D16->5D19 opcode=91 SP=5400->5405 ...
asstb BASIC op IP=5D19 opcode=94 class=9 op=4 SP=5405 ...
asstb BASIC arithmetic op=4 condition=0
```

## Errors and remaining gaps

In SLIC, an invalid entry state or opcode commits the three mutable CB fields,
restores the saved architecture, and invokes `nuerr2(61)`, leading to abnormal
task termination.  The C++ boundary preserves this distinctly as
`GuestError(61)` with a diagnostic; it does not silently resume the MSP.
Storage translation failures are emulator failures and must not partially
update guest ranges.  Arithmetic underflow/overflow and string range
conditions normally synthesize a result and/or leave through a guest vector.

Still unknown, and therefore not candidates for silent approximation:

- every edge adjustment made by string low `4` (substring/slice);
- the source-level names for internal control operations;
- the universal meaning, if any, of control marker `FF`;
- instruction-level V5R1 comparison (the names are present locally, but its
  containing binary segment is not).

Unsupported states must report a diagnostic and must not return `Completed`.
