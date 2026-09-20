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

## First tape request and current boundary

With disk/tape tracing enabled, that `FROMLIBR` request loads the SSP tape
transients and issues:

```text
SVC 46 iob=037D58 eye=C9E3 cmd=01 mod=00 len=880 buffer=803A00 Q=01
```

The IOB address varies with allocation, but the command, modifier, Q byte,
length, and translated-buffer form are stable in repeated overlay sessions.
Current sim36 refuses command `0x01`; SSP posts a processor check before any
tape read or write.  Thus the existing synthetic tests for commands `0x17`,
`0x18`, `0x21`, and `0x22` do not establish the command used to open/create a
real labeled SSP tape file.

IBM's *System/36 Program Service Information*, LY21-0590-4, sections 2-116 to
2-118, documents the record-mode tape data-management modules `#TAFND`,
`#TAGET`, `#TAPUT`, `#TAADD`, `#TARBK`, and `#TAWBK`.  Section 2-212 documents
that tape save/restore builds chained tape IOBs, appends UHL1/UHL2 to HDR1/HDR2,
and uses different paths for reel/6157 and quarter-inch cartridge devices.
The *System/36 Concepts and Programmer's Guide*, SC21-9019-5, identifies
`FROMLIBR` output as a `LIBRFILE` and states that the 6157 uses IBM standard
labels.  These sources establish the layer and dataset family, but the pages
available so far do not assign semantics to IOB command `0x01`.

## Operations required, and what is not yet known

The complete acceptance path requires the following backend primitives; these
already exist independently of the guest IOB mapping: load/unload, rewind,
block read/write, filemark write, forward/backward record spacing,
forward/backward file spacing, and position reporting.  The guest mapping must
also distinguish BOT, tape mark, EOD, not-ready, write-protect, short block,
long block, and malformed requests, including the verified completion, MIC,
and count/residual fields for each case.

The following remain deliberately unsupported until established by a decoded
SSP path, the reference emulator, IBM's LY21-0592 *System Data Areas*, or a
reproducible differential trace:

- command `0x01` semantics and all of its IOB fields;
- the guest commands that cause rewind, spacing, filemark, and position
  operations;
- exact completion bytes and MICs for tape mark, EOD, BOT, write protection,
  and short/long blocks;
- exact HDR1/HDR2/UHL1/UHL2/EOF1/EOF2 contents and the `LIBRFILE` data stream;
- whether a block larger than the supplied buffer is rejected, partially
  returned, or continued through another chained IOB;
- the full `GENERATE` and `RPGP` parameter/result contract on this volume.

No implementation milestone may turn those unknowns into constants by
inference.  Synthetic tape tests remain useful for backend failure
localization, but they are not evidence for SSP IOB semantics.

