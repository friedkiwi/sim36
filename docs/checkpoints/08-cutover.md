# Checkpoint 8 — cutover review, documentation, release

Tag: `cp-8`; release: `v0.1.0`.

## The cutover review

**Self-contained.**  A search of the tree for the reference's name, the
research repository and its paths finds:

- three trace strings that name the reference emulator in passing (a
  storage transient's "builds no job control block" note, the work station
  controller's "not configured in etc/...conf" record note, and the loader's
  refreshable-program note).  Trace strings are carried verbatim, as decided
  at checkpoint 4, because the transcript differential asserts them; they
  are the only remaining mentions in `src/`;
- nothing in `test/`, `tools/`, `cmake/` or the build files.  The
  operator-facing product strings (the panic dump file name and its README
  line, the snapshot's "not a ... checkpoint" refusal, the Python drivers'
  docstrings) now say SIM/36.

The comments and method names describe behaviour; SA21-9436 is cited by
page where the reference cites it (128 citations in `src/`).

**No licensed material.**  `git ls-files` holds only text; a search for the
SSP copyright banner, the program number and the "licensed materials" line
finds nothing.  The gates read `SIM36_VOLUME` at run time and skip with 77
without it; the checkpoint reports quote counts and trace text only, never
a screen.  `var/` (where the default startup file expects a volume) is
ignored by git.

**Licence hygiene.**  `vcpkg.json` names replxx (BSD-3), minizip-ng with
zlib (zlib), nlohmann-json, fmt, cxxopts and doctest (MIT).
`THIRD_PARTY_NOTICES` is generated at build time from the vcpkg copyright
files and installed next to the binary.

**Toolchain.**  g++ 10 and g++ 13 build warning-free under `-Wall -Wextra
-Wpedantic -Werror`; CI builds with g++ 10 on Ubuntu 22.04 and MSVC v142
static on Windows 2022 (`/permissive- /Zc:__cplusplus /utf-8 /W4 /WX`) and
checks the Windows binary's DLL imports against the operating-system set.

## Documentation

- `README.md`: build, run, media, testing, the milestone table.
- `RUNNING.md`: the operator guide (volume, startup files, the appliance
  form, the live monitor, connecting 5250 displays and printers, the
  multiplexer, scripted sessions, snapshots and panic dumps, tracing).
- `etc/sim36.sim` and `etc/sim36-appliance.sim`: the default machine and
  its unattended form, installed with the binary.
- `docs/checkpoints/`: this series.

## Release

- `project(sim36 VERSION 0.1.0)`; `vcpkg.json` version 0.1.0.
- CI packages `sim36-linux-x64.tar.gz` and `sim36-windows-x64.zip` (the
  binary, `etc/`, `LICENSE`, `README.md`, `RUNNING.md`,
  `THIRD_PARTY_NOTICES`) on every push, and a `v*` tag publishes them as a
  GitHub release.

## Gate

The complete `ctest` run with the volume and the reference:

```
46 tests, all passing (each skips with 77 without SIM36_VOLUME)
[doctest] assertions: 1321 | 1321 passed | 0 failed |
```

## Not carried from the reference, by decision

- The `s36xref` Python tooling and the `s36xref-indexed` suite: a
  cross-reference generator over the research documentation, not part of
  the emulator.
- The reference's research probes under `test/` (some two hundred
  `tn5250-*.sim`, `hri-*`, `tfrm36-*`, `mstwa-*`, `cptc-*`, `nupterm-*`
  command files and their monitor-probe pairs): working notes against the
  private volume, most of which predate later changes of the reference and
  fail against it.  The gates that carry evidence were brought across.  The
  experiment commands those probes exercised are still in the monitor as
  in the reference; the experiments that were off by default there
  (`signonexp`, the interactive-session and cnfws experiments) answer
  `not carried by SIM/36; nothing changed`, as recorded at checkpoints 5
  and 6.
- The reference's `.conf` startup format is accepted (the printer suite uses
  it); the shipped startup files use the command language.

## Open items

- The three verbatim trace strings above name the reference; a later
  release may reword them once the transcript differential is retired as a
  gate.
- Station multiplexing of printers is not implemented (nor in the
  reference).
- The SSP 5.1 generated-volume startup file of the reference is not carried:
  it presets a customize byte the emulator does not yet derive.
