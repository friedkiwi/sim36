# Folder tapes and `tape-folder.py`

SIM/36 represents a tape as a directory so its filemarks, block boundaries,
and labels remain inspectable.  The emulator and the standard-library-only
[`tools/tape-folder.py`](../../tools/tape-folder.py) utility share format
identifier `s36-folder-tape`, version 1.

## Physical model

A folder contains `manifest.json` and one blob for each tape file:

```text
example-tape/
  manifest.json
  0001.dat
  0002.dat
```

A *tape file* is the run of blocks between two physical tape marks.  Every
entry in the manifest ends with one implicit tape mark.  Therefore an entry
with zero blocks represents a tape mark immediately after the preceding mark,
and two such entries represent consecutive marks.  A standard-labeled dataset
normally occupies three tape files: header labels, data, and trailer labels.

Each blob is the exact concatenation of that tape file's blocks.  A uniform
file declares `blockLength` and `blockCount`; a variable file declares the
ordered `blockLengths` array and `blockCount`.  There are no host framing bytes
in a blob.  The manifest's `kind`, `recordFormat`, and `recordLength` fields are
descriptive.  The bytes and block-length declarations are authoritative.

Decoded `labels` are a checked convenience mirror.  Standard label records are
80-byte EBCDIC CP037 blocks.  Verification decodes their actual bytes and
requires the JSON mirror to match.  The tool authors only the verified VOL1
layout (identifier at 0, six-byte volume ID at 4, access byte at 10, and
14-byte owner at 37).  It deliberately does not offer HDR/EOF authoring while
the guest-specific fields needed by SSP remain under discovery.

## Common operations

Create and inspect a blank labeled volume:

```sh
tools/tape-folder.py init /tmp/tape --volume-id TEST01 --owner SIM36
tools/tape-folder.py verify /tmp/tape
tools/tape-folder.py list /tmp/tape
tools/tape-folder.py labels /tmp/tape
```

The inspection commands are read-only: they do not rewrite the manifest,
blobs, or modification times.  `verify` checks the format/version, exact
sequence order, portable relative blob paths, missing and duplicate blobs,
bounded positive block lengths, blob byte counts, VOL1/volume agreement, and
decoded label groups.  Orphan `.dat` files produce a warning; use
`--reject-orphans` to make them an error.

Unpack, edit, and repack:

```sh
tools/tape-folder.py unpack /tmp/tape /tmp/tape-work
# Edit tape-work/tape.json and files/0001/block-000001.bin.
# Update that block's length and SHA-256 in tape.json after changing bytes.
tools/tape-folder.py pack /tmp/tape-work /tmp/repacked
tools/tape-folder.py verify /tmp/repacked
```

The editable `s36-tape-work` representation stores every block in its own
file.  The order of `files` is tape-file order and the order of `blocks` is
block order.  A no-op unpack/pack preserves the logical sequence of blocks and
marks exactly and produces canonical, deterministic blob names and JSON.

Extract one tape file, or every file, without discarding boundaries:

```sh
tools/tape-folder.py extract /tmp/tape /tmp/extract --file 2
tools/tape-folder.py extract /tmp/tape /tmp/extract-all --all
```

Each extracted file contains `block-NNNNNN.bin` files and a convenience
`joined.bin`; `extract.json` records lengths and checksums so callers need not
infer boundaries from the joined view.

Append fixed-length host text records to an unpacked workspace:

```sh
tools/tape-folder.py import-text /tmp/tape-work source.txt \
  --encoding utf-8 --record-length 96 --records-per-block 10
```

Line endings are removed, each line is converted strictly to EBCDIC CP037 and
padded with EBCDIC spaces, and records are grouped without changing their
fixed-column content.  An unrepresentable character or overlong record is an
error; the tool never truncates.  The workspace replacement is atomic at the
directory level.

Commands refuse to replace an existing output directory unless `--force` is
given.  With `--force`, the new directory is fully built and verified beside
the destination, the old destination is renamed aside, and only then is the
new directory renamed into place.  A failed swap restores the old directory.

## Tests and limitations

Run the media-independent suite with:

```sh
python3 -m unittest -v tests/test_tape_folder.py
ctest --test-dir build/linux -R tape_folder_tool --output-on-failure
```

The folder format can faithfully represent arbitrary block and filemark
streams, but it does not itself define SSP `LIBRFILE` contents.  HDR/EOF label
authoring, SSP completion/MIC mapping, and guest tape commands remain governed
by the evidence boundary in
[`startrek-tape-discovery.md`](startrek-tape-discovery.md).
