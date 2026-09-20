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
match.  That internal condition is not the guest-visible MIC.  The locally
extracted `#CATP` transient checks completion byte `45`, compares IOB bytes
`+1d..+1e` with its constant `60 74`, and then requires byte `+1f` to be `34`.
The native command-16 implementation therefore returns the verified
three-byte `60 74 34` status tail for dataset-not-found.  This also corrects
the earlier claim that the tape MIC was simply a halfword beginning at
`+1e`: that is the SLIC-facing field, while the guest consumes the overlapping
three-byte status representation.

The shipped `TAPEINIT` procedure supplies that initialization workflow.  Its
standard-label form prompts for `TC`, label type `SL`, volume and owner IDs,
expiration checking/clearing, optional erase, and final rewind/unload.  The
observed execution begins `01/00`, `02/00`, then `12/00` with an 80-byte guest
buffer.  The local `NuTapeIo` command-12 arm requires that exact length, writes
the supplied VOL1 record, writes two filemarks, and rewinds.  Command `12` now
implements that verified sequence; the unobserved command-11 variant sharing
the SLIC arm remains refused.

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
- the System/36-specific UHL1/UHL2 fields and `LIBRFILE` data stream (the
  standard VOL1/HDR1/HDR2/EOF1/EOF2 envelope is now established);
- whether a block larger than the supplied buffer is rejected, partially
  returned, or continued through another chained IOB;
- the full `GENERATE` and `RPGP` parameter/result contract on this volume.

No implementation milestone may turn those unknowns into constants by
inference.  Synthetic tape tests remain useful for backend failure
localization, but they are not evidence for SSP IOB semantics.
