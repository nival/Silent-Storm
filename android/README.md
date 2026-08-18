# Silent Storm — Android port

An Android build of the Silent Storm engine, made from the 2003 source release in
this repository (`Soft/Andy/Jan03/a5dll`). The original targets Win32, DirectX 8/9,
MSVC .NET 2003 and STLport; this directory builds the same code with the Android
NDK (clang/libc++) for `arm64-v8a`, `armeabi-v7a` and `x86_64`.

**Status: the whole engine builds, links and runs on Android; the game reaches
its first screen.** File I/O, the chunk serialiser, the `.res` package reader,
the object model, the Lua 4 VM, texture loading and the retail `game.db`
importer are verified against real game data on device. `Main` — the renderer,
scene, AI, UI and game logic, 154k lines — compiles for arm64 in all 269 of its
files; the Direct3D 9 backend runs on a D3D9-on-GLES 3.0 implementation
(`compat/d3d9gles/`, all 155 engine shaders translated). On a Galaxy Z Fold7 the
game loop runs at ~120 fps into a 1024×768 virtual back buffer; what it draws is
still being brought up (the intermission screen's clear colour is there, the UI
text is not yet). See [docs/PORTING.md](docs/PORTING.md) for the state of each
piece and what comes next.

The app currently boots, mounts your game data, exercises those subsystems and
shows the result on screen through GLES 2.0:

```
== FileIO: game data packages
[ok]   Globals.res: 5 entries (0.1 ms)
         entry -1: read 20 bytes, first byte 0x01
[ok]   Chapters.res: 3 entries (0.0 ms)
...
== Script: the game's own Lua sources
[ok]   Constants.l: 1399 bytes executed (0.1 ms)
         POSE_RUN = 3, as defined in the file
[ok]   20 passed, 0 failed
```

Verified on a Galaxy Z Fold7 (SM-F966B, Android 16, Adreno 830), on the Android
emulator, and headlessly on macOS.

## Quick start

```bash
cd android
./scripts/build_apk.sh          # stage sources, build all ABIs, package an APK
./scripts/push_data.sh          # copy a subset of Complete/ to the device
./scripts/run.sh                # install, launch, print the engine's report
```

Requirements: Android SDK with build-tools and an NDK (r27 and r29 are both
tested), CMake 3.22+, Ninja, Python 3, a JDK for `apksigner`. No Gradle needed —
though `android/` is also a valid Gradle project if you prefer Android Studio.

To run the engine checks on your development machine instead of a device:

```bash
cmake -S . -B build/host -G Ninja && cmake --build build/host
./build/host/silentstorm_hosttest ../Complete
```

See [docs/RUNNING.md](docs/RUNNING.md) for where the game data has to live.

## How the port is organised

```
android/
  compat/        Win32 -> POSIX/Android compatibility layer
    include/       windows.h, hash_map, crtdbg.h ... the vocabulary the engine expects
    include-host/  headers only the host build needs
    src/           implementations (files, threads, time, paths, CRT gaps)
  tools/
    prepare_sources.py   stages the engine sources into gen/, applying documented rewrites
    preview_font.py      renders the boot console font to a PNG for checking
  gen/           generated: the staged engine modules (not checked in)
  platform/      Android entry point, GLES boot console, data mounting, boot harness
  app/           AndroidManifest.xml (a NativeActivity; there is no Java code)
  scripts/       build.sh, build_apk.sh, push_data.sh, run.sh
  docs/          PORTING.md, RUNNING.md, WIN32_SURFACE.md
```

**The original sources are never modified.** `tools/prepare_sources.py` copies the
modules it builds into `gen/` and applies a set of named, individually documented
rewrites on the way through — include paths, x86 assembly, MSVC-only C++, and a
handful of genuine 64-bit bugs. Run it with `--report` to see every change it
makes:

```bash
python3 tools/prepare_sources.py --report
```

That list *is* the port's diff against 2003. Everything else is new code in
`compat/` and `platform/`.

## What is ported

| Module | What it is | State |
|---|---|---|
| `Misc` | refcounting, math, strings, timing, RNG | ported, verified |
| `FileIO` | streams, chunk serialiser, `.res` packages | ported, verified |
| `Script` | Lua 4.0 + the engine's C++ wrapper | ported, verified |
| `MiscDll` | console variables/commands, log streams | ported (builds) |
| `DBFormat` | the `game.db` schema (130 record classes) | ported; `platform/db_retail.cpp` imports the shipped (retail-format) `game.db`: 130/130 tables, 222k records |
| `Image` + libpng | BMP/TGA/PNG and MMP/DXT textures | ported, verified (real textures decode to their stored average colour) |
| `Main` | renderer, scene, AI, UI, game logic (154k lines) | 269/269 files build and link; runs on device over `compat/d3d9gles` (D3D9 on GLES 3.0, 155 shaders) — see RENDERER.md |
| `Input` | DirectInput | `platform/input_android.cpp` implements `Input.h` from Android keys and touch |
| `FModSound` | FMOD 3 wrapper | `NFMSound` implemented as a silent null back end |

## Notable things the port had to fix

Three bugs in the original code only appear on a 64-bit target, and all three
crash immediately rather than degrade:

* `CBufferedStream::LoadBufferForced` shifted two pointers by `nBufferStart - nPos`
  computed in **unsigned int**. On Win32 the arithmetic wrapped around and gave
  the intended negative offset; on arm64 it moves the pointer 4 GB away. This is
  what made every `.res` package segfault.
* `CBufferedStream::SetNewBufferSize` truncated a pointer difference to `int`.
* `CRandomGenerator::FillRandRsl` seeded ISAAC by walking `C:\` for a random file
  and looping until it found one — an infinite loop anywhere else.

* `CStructureSaver` used 4 bytes of an object's *address* as its on-disk
  reference ID. On 64-bit that truncates on write and half-fills a pointer on
  read; the port keys references as `uint32` and numbers objects densely on
  write. Format unchanged.

And one that is not a bug but a trap: `Float2Int` was x87 `fld`/`fistp`, which
**rounds**. A naive `(int)` replacement truncates and silently shifts geometry;
the port uses `lrintf`.

## Licence

The engine sources are covered by the repository's [LICENSE.md](../LICENSE.md) —
non-commercial use only. The porting layer in `compat/`, `platform/`, `tools/`
and `scripts/` is part of this repository and carries the same terms.
