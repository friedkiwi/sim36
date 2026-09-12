# images/ — local volumes and distribution media

Everything in this directory except this file and the `*.sim` startup
files is ignored by git (`images/*` in `.gitignore`).  The media is
licensed material and is never committed; this folder is the local
testing and development store.

## Volumes (`images/volumes/`)

| file | what | startup file |
|---|---|---|
| `as36.img` | Advanced/36 volume, SSP 7.5, 200 MB (819200 sectors) | `images/as36.sim` — unattended IPL, multiplexer on :2300 |
| `ssp51.img` | SSP 5.1 (5727-SS6) volume generated from the 5 1/4-inch set below, 200 MB | `images/ssp51.sim` — attended IPL, paused after a customize-byte poke, multiplexer on :2323 |
| `ssp51-generated-2026-09-10.img.gz` | the same volume, compressed, as generated | `gunzip -c ... > ssp51.img` |

```sh
sim36 -c images/as36.sim                 # then: ipl
sim36 -c images/ssp51.sim                # then: start
SIM36_VOLUME=$PWD/images/volumes/as36.img ctest --preset linux
```

Both startup files attach the volume as an `overlay`, so a run never
modifies the file.  For a read-write run copy the image first and attach
the copy `rw`.

## Distribution media (`images/distribution/`)

ImageDisk (`.IMD`) captures of the original diskettes, with photographs
of the labels where they were taken, grouped by machine and release.

| directory | contents |
|---|---|
| `ssp-5.1-5.25inch/` | 5727-SS6 SSP 5.1 for the 5363/5364 on 5 1/4-inch media: system support (11), base communications (2), utilities (2), RPG II, BASIC, Query/36 (3), PC Support (4), VASP, the DK3900 (11) and DK3922 (5) PTF sets; `README.md` is the per-volume manifest (VOL1, owner id, data sets and extents), `comments.txt` the ImageDisk comment lines |
| `ssp-5.1-5.25inch-zips/` | the same release as originally archived (two software zips and the last PTF set) |
| `ssp-releases-8inch/` | SSP releases 1 to 5 on 8-inch media (`S36_R01.zip` .. `S36_R05.zip` with their catalogues, `S36_PTF.zip`), the release 3 feature diskettes (RPG II, COBOL, utilities, 3270 emulation, communications, microcode DSKT24), the `DSKT` set 1012..1017 and the 5799-BNW system support |
| `5360-stage3/` | 5360 stage 3: SSP release 5 feature diskettes (`ssp_r5/`) and diagnostics (`diag/`) |
| `5363/` | 5363 5 1/4-inch software archive |
| `5364/` | 5364 SSP release 5 base communications |
| `ce-diagnostics/` | CE diagnostic and microcode diskettes for release 5.1 |
| `mapics/` | MAPICS II source and binaries (1991) |
| `firmware/` | control panel firmware dumps (5360 stage 3, 5362) |
| `hand-labeled/` | a hand-labelled games diskette |

The `.IMD` containers carry the physical geometry (cylinder 0 head 0 is
26 x 256-byte sectors with the label; the data area is 8 x 1024).  The
emulator's `attach diskette0` takes a flat image; converting an ImageDisk
capture to a flat image is a separate step (not part of SIM/36).
