# STARTREK tape acceptance: discovery record

This is the evidence record for the tape-based STARTREK acceptance test.  It
separates behaviour observed in SSP from behaviour that is still unknown.  In
particular, an opcode is not implemented merely because its likely meaning is
suggested by its position in a trace.

## Inputs and safety boundary

The public input is `jgeorge44/FUNLIB` commit
`1d0d2ea221cdb7b22d611d8cef474ebd518fb70f`.  It is fetched into temporary
storage; neither its unlicensed source nor media made from it may be committed
or uploaded.  The files used by the native System/36 path are:

| File | Lines | SHA-256 | Guest member |
| --- | ---: | --- | --- |
| `STREK.RPG36` | 2329 | `5b0da3dcd43d667c2b840494ff08dcd336fa07458c796e5067779992f4187aa6` | `STREK`, S, 96 |
| `STREKFM.DSPF36` | 451 | `6f976e4cef7879c75aba2c385e9a08ee7369910f8a065f00e01c45c0b2f14a4f` | `STREKFM`, S, 80 |
| `STREK.OCL36` | 3 | `5e0f8a6213a45bd5c5679897eaf102ea0d07b1e75f561c8bca08ec5739ecdd80` | `STREK`, P, 120 |

The pinned files use LF line endings.  Their maximum physical line lengths are
96, 80, and 23 bytes respectively.  `TREKLOAD.S36PROC` confirms the three
member types and record lengths with its `COPY ... FROM-READER` statements,
but it is a reader/self-unpacking procedure, not a tape format.

All guest observations below used `images/volumes/as36.img` attached with
`overlay`.  No source volume was opened read-write.  Tape folders were unique
temporary directories.  The repository and CI continue to obtain the private
volume only through `SIM36_VOLUME` and `AS36_MEDIA_PAT`.

## Native SSP workflow observed on the test volume

The unattended SSP 7.5 image was driven through a real TN5250 station, signed
on as its test user, and navigated through these shipped menus:

```text
MAIN 3  -> USEDEV 3  -> TAPE
TAPE 4  -> TAPELIBR
```

`TAPELIBR` names the supported library operations:

| Option | Procedure | Purpose |
| ---: | --- | --- |
| 1 | `BLDLIBR` | create a library and copy members from tape |
| 2 | `FROMLIBR` | copy library members to a tape file |
| 3 | `TOLIBR` | copy members from tape to an existing library |
| 4 | `SAVELIBR` | save a complete library to tape |
| 5 | `RESTLIBR` | restore a complete library from tape |
| 6 | `JOBSTR` | copy a job stream from tape to a library |

`FROMLIBR` page 1 prompts for member or `ALL`, member type
(`SOURCE`, `LOAD`, `PROC`, `SUBR`, or `LIBRARY`), tape file name, and location
(`TC`, `T1`, or `T2`; it also lists non-tape locations).  Page 2 prompts for
retention/`ADD`, volume ID, source library, automatic drive advance, final
position (`REWIND`, `LEAVE`, or `UNLOAD`), optional record length 40--120, and
`SVATTR`.  `BLDLIBR` prompts for the new library geometry and optional input
tape file.  `TOLIBR` is the corresponding existing-library restore path.

The compile path observed is:

```text
MAIN 5 -> PROGRAM 4 -> COMPILE 8 -> GENERATE
MAIN 5 -> PROGRAM 4 -> COMPILE 1 -> RPGP
```

`GENERATE` is the shipped generator for menus, display formats, and message
members.  `RPGP` is the shipped RPG II compiler procedure.  The installed
`STREK` procedure then performs `LOAD STREK` and `RUN`.

For a write-path oracle, the probe did not inject a host member.  It used SSP
`BLDLIBR` to create `TRKTEST`, SSP `SEU` to create a two-statement 120-byte
procedure member `TPPROC`, and selected SEU Cmd7, end option 1, before invoking:

```text
FROMLIBR TPPROC,PROC,DISCFILE,TC,1,DISC01,TRKTEST,,,REWIND
```

This is reproducible proof that the failing request belongs to the real guest
export path.

## Native request sequence and the corrected SLIC boundary

With disk/tape tracing enabled, that `FROMLIBR` request loads the SSP tape
transients and issues:

```text
SVC 46 iob=037D58 eye=C9E3 cmd=01 mod=00 len=880 buffer=803A00 Q=01
```

The IOB address varies with allocation, but the command, modifier, Q byte,
length, and translated-buffer form are stable in repeated overlay sessions.
A disposable diagnostic mapping, removed after each trace, exposed the next
native requests without treating them as implemented:

```text
01/00 length 880   initialise/rewind path
02/03 length 880   establishes the loaded tape session without moving media
13/03 length 880   rewinds and returns the 80-byte VOL1 record
16/03 length 480   find/open using a buffer beginning with DISCFILE and owner SIM36
```

Returning an invented result for `16` either takes the existing-file path or
invokes SSP's error/storage-dump path; it does not establish the write
operation.  Those early diagnostic mappings are therefore not present in the
implementation.

The local V4R4 SLIC corpus supplies the missing upper layer.  In
`NuTapeIo::entry` (`ffffffffc23e20c0`) the command is read from IOB `+0x0A`,
one is subtracted, and an unsigned comparison against **48 decimal** accepts
commands `0x01..0x31`.  The former `0x40` ceiling in sim36 was a decimal/hex
transcription error and has been corrected to `0x31`.  The decoded jump table
at `ffffffffc30761f0` proves these relevant dispatches:

| command | SLIC target | established role |
|---:|---|---|
| `13` | `entry+0x14DC` | requires length 880 and performs volume/label setup |
| `16`, `20` | `NuTapeIo::tapFind` | locate/find a named tape file |
| `17` | `entry+0x0C18` | data-read family |
| `18` | `entry+0x0FB4` | data-write family |
| `21` | `entry+0x0AE4` | data-write family |
| `22` | `entry+0x032C` | data-read family |

Other named routines in the same locally decoded layer include `tapLocate`,
`tapRdLbls`, `tapWrtLbls`, `tapWriteEov`, and `tapEofHan`.  This corrects the
earlier conclusion drawn from `NuTapeLogicalOp` alone: positioning and label
handling are not absent from the A/36 implementation; they live in
`NuTapeIo`, above that lower data mover.

IBM's *System/36 Program Service Information*, LY21-0590-4, sections 2-116 to
2-118, documents the record-mode tape data-management modules `#TAFND`,
`#TAGET`, `#TAPUT`, `#TAADD`, `#TARBK`, and `#TAWBK`.  Section 2-212 documents
that tape save/restore builds chained tape IOBs, appends UHL1/UHL2 to HDR1/HDR2,
and uses different paths for reel/6157 and quarter-inch cartridge devices.
The *System/36 Concepts and Programmer's Guide*, SC21-9019-5, identifies
`FROMLIBR` output as a `LIBRFILE` and states that the 6157 uses IBM standard
labels.  These sources establish the layer and dataset family, but the pages
available so far do not assign semantics to IOB command `0x01`; the decoded
SLIC dispatcher and its `tapeRemoved`/`readyTape` calls now establish that
activation path independently.

`NuTapeIo::tapFind` at `ffffffffc23e4d60` establishes command `16` in more
detail.  For the observed modifier `03` it requires a 480-byte (`0x1e0`) work
area, copies the first 17 bytes as the requested dataset identifier, rewinds,
reads 80-byte label records, and compares that key with the 17-byte field at
`HDR1+4`.  A match invokes the label reader for the remaining header records
and consumes the closing tape mark, leaving the head at block zero of the
dataset's data tape file.  Its `tapLbls2` continuation writes the accumulated
label byte count to IOB `+0x12` and moves the contiguous label group beginning
with the matched HDR1 into the guest work area.  A synthetic labeled tape
driven through the real SVC 46 path now proves that the next guest read returns
the chosen data block.

The same decoded SLIC end-condition arm uses internal condition `0x001b` and
completion low nibble `5` when the search reaches physical end without a
match.  That internal condition is not itself the guest-visible MIC.  Local
SLIC routine `NuTapeMicSrcGenS` indexes its condition table (entry `0x001b` is
`0x6236`) and generates two halfwords at IOB `+0x1c` and `+0x1e`: `7462` and
`1b36`.  Returning that complete status was checked with the real `#CATP`
transient: `FROMLIBR` accepts dataset-not-found and immediately issues command
`14/03` with its 320-byte label area.  Returning the internal condition alone,
or only a three-byte signature, instead leads to an SSP error or storage dump.

The local SLIC jump table maps command 14 to the `NuTapeIo::entry` arm at
`c23e2ad0`.  That arm calls `tapLblCnt` and then `tapWrLbls1`; the latter writes
the individual 80-byte labels and the closing filemark.  For the observed
320-byte request these are EBCDIC `HDR1`, `HDR2`, `UHL1`, and `UHL2`, leaving
the tape at the start of the data file.  The implementation accepts only this
verified `14/03/320` standard-label form and refuses other layouts.

The same jump table maps command 19 to `c23e2784`.  Real FROMLIBR exports
issue `19/00` immediately after command 21 writes the final data block: the
small procedure probe retained a 512-byte length, the STARTREK source export
retained its 256-byte short-block length, and the complete-library export
retained 768 bytes.  The decoded arm does not read the IOB length.  It writes
the data-closing filemark, derives and emits the
saved label group's `EOF1`, `EOF2`, `UTL1`, and `UTL2` records through
`tapWrtLbls`, and writes the label-file closing mark.  The supported command-19
form is deliberately limited to that observed active-dataset sequence.

FROMLIBR next issues command `1B/00` with the retained work-area length: 512
bytes in the small procedure probe, 256 bytes in the STARTREK source export,
and 768 bytes in the complete-library export.  Its decoded arm likewise does
not read the IOB length.
The jump table maps it to `c23e3cb4`; the decoded arm's first media operation
is the tape-proxy tape-mark method, followed by driver/session finalization.
It therefore adds the second consecutive terminal mark after command 19's
trailer-label closing mark.  Only this observed active-dataset finalization
form is supported.

Finally, the observed export reactivates the tape and issues command `27/00`
with no data buffer, retaining the preceding 512-, 256-, or 768-byte work-area
length in the observed exports.  Its jump-table arm is `c23e3964`; that arm
does not read the IOB length, and the
accepted path calls tape-proxy vtable slot `0x198`, identified from the local
proxy layout as unload.  This is the native flush boundary: the folder backend
is persisted and the cartridge becomes not-ready without discarding the
mounted medium object.
The restore workflow issues the same command with modifier 00 and its
4096-byte input work area; the unload arm transfers neither buffer, and both
observed lengths are accepted.

The restore side reads `LIBRFILE` data with command 22.  Its SLIC arm at
`c23e23ec` stores the transferred byte count at IOB `+0x12`: the requested
length for a full block, or requested length minus the driver residual for a
short block.  `$MAINT` uses that count when consuming the input buffer, so the
emulator now returns the actual block length there.  Command 17 uses a
different arm and no unverified ancillary-field behavior is assigned to it.
When command 22 encounters the data-closing mark, internal status `1c`
branches through `tapRdLbls` to `tapEofHan`: it consumes EOF1, EOF2, UTL1,
UTL2 and their closing mark, compares them with the header group retained by
`tapFind`, clears the returned byte count, and posts completion nibble 2.
The emulator implements only that verified four-label standard-label form;
malformed or mismatched trailers are refused.

An overlay-backed acceptance replay against the guest-created `DISCFILE`
volume completed with:

    BLDLIBR TRKRS9,100,,,DISCFILE,TC,,,,REWIND

The trace showed command 22 transfers of 4096, 4096, and 512 bytes, followed
by completion `42` after the four matching trailer labels and their mark.  A
subsequent command 27/00 with length 4096 unloaded the tape, and BLDLIBR
returned to the System/36 main menu without a processor check.  This proves
the native labeled restore workflow; the disposable disk overlay was removed
with the emulator process.

The FROMLIBR payload is a `$MAINT` card stream.  Each physical record is 256
bytes: a 120-byte CP037 card image followed by 136 CP037 `S` bytes.  Sixteen
records form each 4096-byte tape block, with a short final block allowed.  The
first exported record is a `// COPY LIBRARY-<type>,NAME-<member>` descriptor;
the native export does not carry the self-unpacker's `FROM-READER` or
`TO-FUNLIB` transport clauses.  The standard header group is byte-for-byte
HDR1, HDR2, `UHL1LIBRFILER&` plus 66 `S` bytes, and UHL2 plus 76 `S` bytes;
the trailer changes those identifiers to EOF1, EOF2, UTL1, and UTL2.

`tools/build-startrek-tape.py` validates the pinned checkout and checksums,
then maps the three upstream members to COPY descriptors retaining their
verified `RECL-120`, `RECL-096`, and `RECL-080` values.  The remaining card
sequence must match pinned `TREKLOAD.S36PROC` after stripping only its
reader/FUNLIB endpoints.  A deterministic build contains 2,788 records,
713,728 payload bytes, and 175 data blocks.  Its payload SHA-256 is
`614da14ec03d906074654ed8d5cc0ef6c2fd8dba52478049f77d5a392b866d7a`; its
canonical media-tree SHA-256 is
`d745f025ed29894392cbfe01af2b6ea3cae317f5e5a8853591f577e5c2c5a87f`.
An overlay-backed `BLDLIBR TRKSTB,5000,,,DISCFILE,TC,,,,REWIND` replay read all
175 blocks, consumed the trailer labels, unloaded, and returned to MAIN
without a processor check.

The shipped `TAPEINIT` procedure supplies that initialization workflow.  Its
standard-label form prompts for `TC`, label type `SL`, volume and owner IDs,
expiration checking/clearing, optional erase, and final rewind/unload.  The
observed execution begins `01/00`, `02/00`, then `12/00` with an 80-byte guest
buffer.  The local `NuTapeIo` command-12 arm requires that exact length, writes
the supplied VOL1 record, writes two filemarks, and rewinds.  Command `12` now
implements that verified sequence; the unobserved command-11 variant sharing
the SLIC arm remains refused.

The first native RPGC replay also exposed a non-tape prerequisite in the
fixed-disk IOS.  Its `#MGRE` message path issued SVC 40 with command `00` and
an otherwise empty IOB.  This is not an inferred alias for a disk operation:
local V4R4 `NuDiskIo::executeInternal` at `c1864a4c..c1865478` initializes its
result to completion `40`; only the A1, A2, and A3 transfer arms replace that
result, so command `00` reaches the common return unchanged without a
transfer.  The emulator now accepts only that observed default form in
addition to the established A0/A4 no-transfer commands.  With it, RPGC
advances from `#MGRE+0687` to the guest's own `#CLSG` error disposition rather
than stopping at the device boundary.

The longer replay crossed a second, independently evidenced boundary.  DDDM's
resident fast-transfer slot 63 allocated request blocks above 1 MB; a flow
trace showed its final indirect load using `PXR2:XR2 = 10:D90A`, while sim36
incorrectly fetched from `00:D90A` and entered data at `9003`.  SA21-9436
1-20 and 1-28 state that PACT provides real addressing up to 7302 KB and that
an untranslated PACT value is concatenated with the 16-bit register.  Thus
bits `0x10`, `0x20`, and `0x40` are real-address prefix bits, not flags; only
IBM bit 0 (`0x80`) selects ATR translation.  Resolving all seven prefix bits
makes `10:D90A` name the live request area and carries RPGC past the false SVC
D3.  The same trace also established that a direct resident transfer takes a
program-block reference which `nupexit` releases; the emulator now balances
that reference instead of wrapping PB+27 during repeated DDDM calls.

This organization is independently consistent with the local OS/400 V4R4
SAVSYS analysis in the adjacent `syspass_research` tree: standard label
records are 80-byte EBCDIC CP037, `HDR1` carries the 17-byte dataset ID at
offset 4, and a dataset is represented as header-label file, data file, and
trailer-label file separated by tape marks.  Those local decoded labels are
evidence for the shared standard-label envelope only; they do not establish
the System/36 `LIBRFILE` payload or the additional UHL records documented for
SSP library save/restore.

## Operations required, and what is not yet known

The upper dispatcher has an important split: commands below `0x10` bypass
the table. Command `01` enters activation/readiness handling; command `02`
goes directly to `c23e33fc`, accepts modifiers 0, 1, and 3, and the observed
`02/03` path performs session/readiness handling without a data-mover call.
For commands `0x10` and above the table is indexed by `command - 0x10`.
Consequently command `13` (not command 02) is arm `c23e359c`: it requires an
880-byte work area, invokes rewind at driver-vtable offset `0x190`, and calls
the label reader. The implementation now reproduces the observed `01/00`,
`02/03`, and `13/03` FROMLIBR prefix and returns the verified 80-byte VOL1.

The complete acceptance path requires the following backend primitives; these
already exist independently of the guest IOB mapping: load/unload, rewind,
block read/write, filemark write, forward/backward record spacing,
forward/backward file spacing, and position reporting.  The guest mapping must
also distinguish BOT, tape mark, EOD, not-ready, write-protect, short block,
long block, and malformed requests, including the verified completion, MIC,
and count/residual fields for each case.

The following remain deliberately unsupported until established by the local
decoded SSP/SLIC paths or a reproducible guest trace:

- unobserved modifier variants and complete ancillary output fields for
  commands `01`, `02`, `13`, and `16`;
- the remaining command-to-rewind/spacing/filemark mappings in the
  `NuTapeIo` jump-table arms;
- exact completion bytes and MICs for tape mark, EOD, BOT, write protection,
  and short/long blocks;
- whether a block larger than the supplied buffer is rejected, partially
  returned, or continued through another chained IOB;
- the full `GENERATE` and `RPGP` parameter/result contract on this volume.

No implementation milestone may turn those unknowns into constants by
inference.  Synthetic tape tests remain useful for backend failure
localization, but they are not evidence for SSP IOB semantics.
