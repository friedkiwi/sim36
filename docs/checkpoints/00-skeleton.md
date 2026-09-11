# Checkpoint 0 — repository skeleton

Tag: `cp-0`

## What was delivered

- CMake project (`CMakeLists.txt`, `CMakePresets.json`) with C++17, no
  extensions, warnings as errors on both compilers, static CRT on MSVC,
  Windows 7 target definitions.
- `vcpkg.json` in manifest mode pinned to baseline
  `52d80838fb40c755b1615fbc9c7b994a33742a22`, with the six approved ports.
  `minizip-ng` is taken with `default-features: false` and only its `zlib`
  feature, because its default feature set pulls in OpenSSL (Apache-2.0),
  which is outside the licence allow-list.  zlib itself is therefore in the
  closure as a transitive dependency (zlib licence) and is linked directly
  for the gzip snapshot container; no other dependency was added.
- `cmake/ThirdPartyNotices.cmake` generates `THIRD_PARTY_NOTICES` at build
  time from the vcpkg `copyright` files of the installed closure.  The
  generated file lists cxxopts, doctest, fmt, minizip-ng, nlohmann-json,
  replxx and zlib.
- `.github/workflows/ci.yml`: g++ 10 on ubuntu-22.04, MSVC v142 on
  windows-2022 (the toolset is added through the Visual Studio installer
  because the hosted image ships only v143), vcpkg binary caching through
  the GitHub Actions cache, `dumpbin /dependents` check against an
  operating-system-only allow-list, and packaged artefacts for both.
- MIT `LICENSE`, `README.md`.
- `sim36` binary: `-c`, `-s`, `-t`, `-h` options; the `sim36> ` prompt through
  replxx on a terminal and through plain line input when stdin is redirected
  (so scripted transcripts are byte-stable); the full command registry table
  with the reference's grouped help text; `help`, `quit`/`exit`, `do <file>`
  with per-file relative path resolution and recursion detection; the
  reference's error texts for unknown commands and for commands that need a
  constructed machine.
- doctest target `sim36_tests` with lexer tests; `ctest` runs it plus
  `sim36 -s test/empty.sim`.

## Gate

### g++ 10 (local, `build/gcc10`)

```
$ CXX=g++-10 CC=gcc-10 cmake --preset linux -B build/gcc10
-- The CXX compiler identification is GNU 10.5.0
-- Configuring done (150.9s)
$ cmake --build build/gcc10 -j16 2>&1 | grep -E "warning|error|Linking"
[7/11] Linking CXX static library libsim36core.a
[9/11] Linking CXX executable sim36_tests
[11/11] Linking CXX executable sim36
$ (cd build/gcc10 && ctest)
100% tests passed, 0 tests failed out of 2
$ ./build/gcc10/sim36 -s test/empty.sim >/dev/null; echo "empty exit=$?"
empty exit=0
```

Also built warning-free with g++ 13.3 (`build/linux-make`).

### MSVC v142 static

Not run locally: no Windows host is available in this environment.  The CI
job `windows-msvc142-static` performs the build, `ctest`, the empty command
file run and the `dumpbin /dependents` check.  Its first result is recorded
in the next checkpoint report.

## Divergences from the reference

| Behaviour | Reference | SIM/36 | Cause |
|---|---|---|---|
| Prompt and echo prefix | `sim> ` | `sim36> ` | Required by the project brief. `tools/diffrun.sh` (milestone 1) normalises it. |
| Banner | `s36refemu - System/36 reference emulator` | `SIM/36 - System/36 emulator` | Name change. Interactive only; never in `-s` transcripts. |
| Default startup file | `etc/s36refemu.sim`, else `etc/s36refemu.conf`; missing file is an error | `etc/sim36.sim` when it exists, otherwise built-in defaults | A fresh clone must start without any file. An explicit `-c` of a missing file is still `not found: <path>`, exit 2. |
| Legacy INI startup files (`[section]` first line) | accepted | not yet ported | Deferred to milestone 1 with the rest of the Configuration layer. |
| Unknown option | `unknown option --x` then usage, exit 2 | cxxopts' message (`Option 'x' does not exist`) then usage, exit 2 | Option parser is a dependency. |

## Reference bugs suspected and left in place

None observed at this milestone.

## Open items carried forward

- Milestone 1: Storage layer, Configuration layer, `show config`,
  `save config`, `reset`, `tools/diffrun.sh`, legacy INI reader decision.
- Confirm the MSVC CI job is green and record its `dumpbin` output.
