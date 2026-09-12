# Installing SSP 5.1 from the bitsavers media

This walks through bringing up IBM System Support Program (SSP) Release 5.1
under SIM/36, starting from a freshly compiled `sim36` and a blank hard-disk
image, using the publicly archived diskette set from bitsavers.

> **Status (read this first).** SIM/36 converts the bitsavers diskettes,
> creates the disk image, IPLs phase 1 (`#IPLBOOT` / `MSPID`) from the
> diskette, and reaches SSP's own **SSP GENERATION AND RELOAD** program.
> The generation program then stops on a fresh, uninitialised disk with
> `SYS-3908 System error--an invalid SSP was found`, because nothing has
> written the disk's system area / unit definition table yet and the
> generator refuses to relocate an empty one. This behaviour is **identical
> on the reference emulator** (byte for byte), so it is a genuine missing
> step, not a SIM/36 regression, and blank-disk initialisation and UDT
> synthesis are open items. Until they land, the ready-to-run SSP 5.1 volume
> in `images/volumes/ssp51.img` is the working path (§6); everything up to
> the generation step (§1–§4) works today and is what this document
> verifies.

## The media

The exact image set is on bitsavers, under
`http://bitsavers.org/bits/IBM/System_36/` (the `bitsavers.org` host returns
`403` to a plain `curl`; the `http://bitsavers.trailing-edge.com/bits/IBM/System_36/`
mirror serves the same files):

| what | file | link |
|---|---|---|
| SSP 5.1 base + microcode + products, 5 1/4-inch (5363/5364) | `5363/S36-5.25.zip` | <http://bitsavers.org/bits/IBM/System_36/5363/S36-5.25.zip> |
| the same release, split into three archives with per-volume names | `5_inch/IBM_System_36_5.1_Software1.zip`, `…Software2.zip`, `…Last_PTFs_DK3900_DK3925.zip` | <http://bitsavers.org/bits/IBM/System_36/5_inch/> |

Either archive carries the same base SSP: inside `S36-5.25.zip` the eleven
volumes `SSP-5.1/SSP51-01.IMD` … `SSP51-11.IMD`, plus the control-storage
microcode `MCODE11.IMD` / `MCODE12.IMD`. Volume 1 is the bootable one — its
`VOL1` names owner `5727SS65190V01` and it carries the `#IPLBOOT` data set
(extent `01001..03108`) followed by `SSPMRI`.

A local copy of this media already lives, uncommitted, under
`images/distribution/` (see `images/README.md`); the steps below use the
bitsavers archive directly so the document stands on its own.

The diskettes are ImageDisk (`.IMD`) captures. SSP 5.1 for the 5363/5364 is
5 1/4-inch media: cylinder 0 is `26 × 256`-byte sectors (the label track) and
the data area is `8 × 1024`-byte sectors per track.

## Prerequisites

- A built `sim36` (see `README.md`): `cmake --preset linux && cmake --build --preset linux`.
- Python 3 (for the diskette converter, `tools/imd2flat.py`).
- `unzip`, and the bitsavers archive above.

The commands below assume the repository root as the working directory and
`build/linux/sim36` as the binary; adjust the path for your preset.

## 1. Unpack the media

```sh
mkdir -p work/ssp51
unzip -o -d work/ssp51 S36-5.25.zip
ls work/ssp51/S36-5.25/SSP-5.1        # SSP51-01.IMD … SSP51-11.IMD
```

## 2. Convert the diskettes to flat images

SIM/36's diskette drive attaches a flat, byte-exact sector image; the
`.IMD` container has to be flattened first. `tools/imd2flat.py` does that,
following the physical (cylinder, head, sector-id) order the drive expects:

```sh
python3 tools/imd2flat.py -o work/ssp51/flat \
    work/ssp51/S36-5.25/SSP-5.1/SSP51-*.IMD \
    work/ssp51/S36-5.25/MCODE11.IMD \
    work/ssp51/S36-5.25/MCODE12.IMD
```

Each base volume flattens to `1258496` bytes and each microcode volume to
`1180672`. One sector of `SSP51-11.IMD` is unreadable in the bitsavers
capture; add `--allow-bad` to zero-fill it (that volume holds the office /
communications products, not the base SSP, so it does not affect the
install). The converter refuses an unreadable sector by default rather than
hand you invented bytes.

## 3. Create the blank hard-disk image

The Advanced/36 volume geometry is `819200` sectors of `256` bytes = 200 MB:

```sh
mkdir -p work
truncate -s 209715200 work/ssp51-new.img
```

You can now confirm a converted volume is a valid IPL diskette by mounting
it against that disk (the drive needs a constructed machine, and `ipl pause`
constructs one without running the guest):

```sh
build/linux/sim36 -s /dev/stdin <<'EOF'
attach disk0 work/ssp51-new.img rw
attach diskette0 work/ssp51/flat/SSP51-01.img ro
ipl pause
diskette
quit
EOF
```

The `diskette` line reports `volume PPMRI  owner 5727SS65190V01`, the
`5.25-inch` geometry and the `8 × 1024 B` data area — the drive probed the
label track and accepted the image.

## 4. IPL phase 1 from the diskette

Boot with the diskette as the load source. The one poke below presets the
system-entry customize byte at guest `0x0850` to `8D`: SSP 5.1's `MSPID`
requires `8B`/`8D` there, and SIM/36 does not yet synthesise the unit
definition table that a real machine derives it from, so it is supplied by
hand (an emulator decision, flagged as such).

```sh
build/linux/sim36 -s /dev/stdin <<'EOF'
set machine load-source diskette
set machine ipl-type attend
attach disk0 work/ssp51-new.img rw
attach diskette0 work/ssp51/flat/SSP51-01.img ro
ipl pause
poke 0850 8D
start
wait idle 120
console
quit
EOF
```

`console` shows the guest reaching its own installer:

```
SSP GENERATION AND RELOAD - MESSAGES
Relocating system area
SYS-3908 System error--an invalid SSP was found
...
Press Enter to continue processing.
```

Phase 1 (`#IPLBOOT` / `MSPID`) loaded and ran from the diskette, the label
track was read by CHRNX and the `#IPLBOOT` data set by sequential-sector
addressing, and control reached SSP's generation program — which is as far
as the generation gets today (see the status note at the top). It has not
copied any library to the disk (`show cpu` after this run reports the guest
parked in its idle wait; the disk image is unchanged), so it does not yet
produce a bootable volume.

## 5. What is missing to finish the generation

The generation aborts at `SYS-3908` because relocating the system area on an
all-zero disk finds no valid SSP and no unit definition table. Completing it
needs two things SIM/36 (and the reference) do not do yet:

- **Blank-disk initialisation** — writing an initial system area, VTOC and
  UDT skeleton the generator can relocate into. Nothing in the emulator
  writes these today.
- **UDT synthesis from configuration** — so the customize byte at `0x0850`
  is produced by the machine instead of poked, and `SYS-3922`/`SYS-3908`
  clear on their own.

Once past those, the documented shape of the full procedure (from the
research notes and SC21-9051-4 *Updating to a New Release*, Chapter 2 Step
10) is: sign on at the generation panel, then feed the remaining volumes as
it prompts — `SSP51-01`/`-02` (`SSPMRI`), the `HELPMRI` volumes, the
`SSPBASE` volumes, and finally the microcode diskette (`MCODE12`) — ending
with `SSP generation complete, MSIPL from disk required` and an IPL from the
disk. The volumes are read as data by ordinary MSP code; there is no second
"microcode" processor to run.

## 6. Booting a generated SSP 5.1 volume

A generated SSP 5.1 volume is provided at `images/volumes/ssp51.img` (see
`images/README.md`), together with its startup file `images/ssp51.sim`. It
IPLs to the SSP 5.1 sign-on:

```sh
build/linux/sim36 -c images/ssp51.sim
# then, at the prompt:
start
wait idle 120
console
```

`console` shows the `IPL SIGN ON` panel and `COPYRIGHT 1985 IBM
Corporation`. This volume is the product of the generation above performed
once; it is the way to run SSP 5.1 under SIM/36 until the blank-disk
generation completes from scratch.

## Summary

| step | state |
|---|---|
| convert `.IMD` diskettes to flat images (`tools/imd2flat.py`) | works |
| create the 200 MB blank disk image | works |
| IPL phase 1 from the diskette, reach the SSP generation program | works |
| complete the generation onto a blank disk | blocked at `SYS-3908` (open: blank-disk init, UDT synthesis); parity with the reference |
| boot a generated SSP 5.1 volume (`images/volumes/ssp51.img`) | works |
