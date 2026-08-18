# Porting Silent Storm to Android

This is the working map for the port: what is done, what is left, in what order,
and the traps found along the way. It is written for someone picking the work up,
not as a summary of what happened.

## Where things stand

```
        compat layer  ████████████████████  the Win32 surface the ported modules use
        Misc          ████████████████████  ported, running on device
        FileIO        ████████████████████  ported, reading real .res packages
        Script        ████████████████████  Lua 4.0 running the game's own .l sources
        MiscDll       ████████████████░░░░  builds; console vars untested
        DBFormat      ████████████████░░░░  builds and runs; blocked on the *data* (see below)
        Image         ████████████████████  ported; DXT1/3/5 software decoder in platform/
        Main          ░░░░░░░░░░░░░░░░░░░░  154k lines: renderer, scene, AI, UI, game
        Input         ░░░░░░░░░░░░░░░░░░░░  DirectInput -> touch; replace, do not wrap
        FModSound     ░░░░░░░░░░░░░░░░░░░░  FMOD 3 -> Oboe/OpenSL; replace
```

## The order the remaining work should happen in

### 1. `wstring` — done

The engine serialises `std::wstring` straight into `game.db` and asset files,
and it is UTF-16 because `wchar_t` is 2 bytes on Win32. Android's is 4. The
staged sources now use `char16_t`/`std::u16string` (a mechanical pass in
`prepare_sources.py`), `WCHAR` is `char16_t` in the compat `windows.h`, and
`compat/src/wide_char.cpp` provides the char16_t CRT forms plus real
windows-1251/1252 conversion tables for `MultiByteToWideChar` and friends.

### 1b. `game.db` — the data is newer than the source

`DBFormat` and the `ADOFake` database stub compile, link and run: all 130 record
classes register, `NDatabase::Serialize` parses `game.db` and creates the table
registry. But **no `game.db` in this repository matches this source snapshot**.

* This source (January 2003) stores the database as `hash_map<int, CDBTableBase>`
  by value, each table's records going through the record classes' `operator&`.
* Every shipped `game.db` (`Data/`, `Complete/`, `Versions/Current/`) stores each
  table as a heap object of a class registered as `0xA1843130` — a class this
  source does not have — with a uniform body of chunks 2..8 that looks like a
  column-oriented layout common to all tables. `Complete/game.db` additionally
  carries a leading chunk-4 version tag.

The chunk serialiser reports the situation exactly (`a5_serializer_unknown_types`),
and the harness shows it as a warning rather than a failure. Two ways forward:

1. **Find a matching database.** The `Data/*.mdb` Access files are the authoring
   source; `Tools/` contains the importer that produced `game.db` from them
   (`DataImport.exe`). If a copy of the January 2003 build's `game.db` — or the
   ability to run the importer of that era — turns up, the port loads it as-is.
2. **Reverse the retail table format.** Chunks 2..8 per table across 56 tables
   is a bounded job, but it is a job of *recovering a schema*, and its correctness
   would rest on inference rather than on the source. Not attempted here.

Either way, this is a data-versioning problem, not a porting one: the engine
code that reads the format it was written for is running on device.

### 2. `Image` — done

BMP/TGA/PNG/MMP loading builds and runs; libpng 1.0.9 is built from the tree
against the NDK's zlib with the x86 assembler back ends off. `ImagePack.cpp` (the
DXT *encoder* on a proprietary `s3tc.h`) is a tools-side dependency of TexConv
and is not part of the runtime. `platform/dxt_decode.cpp` decodes DXT1/3/5 in
software for GPUs without `GL_EXT_texture_compression_s3tc` and for the harness,
which checks decoded mean colour against the header's `dwAverageColor`.

### 3. The renderer — `Main`'s `Gfx*` files

This is the large piece, but it is better contained than the line count suggests.
Direct3D types appear in only a handful of files (`Gfx.cpp`, `GfxInternal.h`,
`GfxBuffers.cpp`, `GfxShaders.cpp`, `GGeometryUtil.cpp`); the rest of the engine
talks to the `Gfx.h`/`GScene.h` abstraction above them. The shape of the work:

* implement the `Gfx.h` interface on GLES 3.0 instead of D3D9
* vertex/index buffers map onto GLES buffer objects fairly directly
  (`GfxBuffers.cpp` is already an allocator over device buffers)
* the shader layer (`GfxShaders.cpp`, `GfxShadersDescr.h`) is the hard part: it
  compiles D3D vertex/pixel shader assembly. Expect to hand-write GLSL for the
  handful of material paths the game actually uses rather than translating
* fixed-function state (`GRenderModes.h`) becomes explicit GL state
* five `__asm` blocks remain in `Main` (`Bound.h`, `SWTexture.cpp`,
  `2DSceneSW.cpp`, `GSceneParticles.h`) — the software-renderer ones can go away
  entirely, the bounding-volume one needs a scalar rewrite

The EGL context the boot console already creates (`platform/android_main.cpp`) is
where that backend should render.

### 4. Input, audio, video

* **Input**: `Input/Input.cpp` is DirectInput and should be deleted, not ported.
  `Input/Bind.cpp` — the action-binding layer above it — is portable and worth
  keeping; feed it Android touch/key events. A turn-based tactical game maps
  reasonably onto touch, but the port will need a camera-control scheme of its own.
* **Audio**: `FModSound` wraps FMOD 3.x, whose licence is not included. Replace
  the wrapper with Oboe or OpenSL ES behind the same `FMSound.h` interface.
* **Video**: Bink is licensed and absent. Cutscenes should be skipped or the
  container replaced; nothing else depends on it.

### 5. The game layer

Everything else in `Main` — scene graph, animation, pathfinding, AI, UI, RPG and
turn logic — is platform-independent C++ and should mostly compile once the
compat layer covers it. Budget the effort for the renderer, not for this.

## Traps found so far

Things that cost time here, so they do not cost it again:

**32-bit pointer assumptions.** The stream layer stores a logical EOF as a
pointer past the end of its buffer and adjusts it with unsigned arithmetic that
relied on 32-bit wraparound. It crashes instantly on arm64 rather than
misbehaving subtly — but the same pattern may exist elsewhere in code that has
not been exercised yet. Grep for pointer arithmetic mixing `unsigned int` offsets.

**`Float2Int` rounds.** 238 call sites. The x87 original uses the current FPU
rounding mode (nearest-even); `(int)` truncates. Use `lrintf`.

**`hash_map` iteration order.** The port maps STLport's `hash_map` onto
`std::unordered_map`. Reading is unaffected, but anything that *writes* a
container to disk will produce a different entry order than the 2003 tools did.
That is fine for `.res` packages (they carry a lookup table) — check before
relying on it for save games.

**Object references in the chunk serialiser are 32-bit save-time addresses.**
`CStructureSaver` writes 4 bytes of an object's *pointer* as its reference ID and
maps them back on load. On 64-bit that dropped half the address on write and
left half a `void*` unwritten on read, so every reference resolved to nothing.
The port keys references as `uint32` throughout and, on write, numbers stored
objects densely instead of using addresses. On-disk format is unchanged.

**MSVC 7 leniencies that recur across the tree** — all handled by mechanical
passes in `prepare_sources.py`, so they will not need attention again in `Main`:
forward-declared enums (`enum E;` → `enum E : int;` plus the definition),
`typename` on dependent iterator types, `if ( CDynamicCast<T> p( x ) )`
declarations, `typeid` on incomplete types in the class factory, and the
`CPtr<T> == T*` overload ambiguity.

**`lua_dobuffer` does not run the chunk.** It parses and *starts* it on a Lua
thread; `lua_executeThreads()` is commented out in `ldo.cpp` because the engine
pumps threads from its frame loop. Call `Script::ExecuteThreads()` or nothing
happens and no error is reported.

**The shipped `.res` files are one revision newer than this source snapshot.**
Signature `0x96948A22`, not the `0x95938921` in `FilesPackage.cpp`. The chunk
format itself is unchanged. `Complete/regs.res` is not a package at all despite
the extension.

**Directories pushed with `adb push` are owned by `shell` with mode 0770** and
the app cannot enter them. `scripts/push_data.sh` chmods the tree afterwards.

## Things deliberately not done

* **`MemoryMngr`** replaces global `operator new`/`delete` and walks the PE
  import table for symbol names. Drop it; keep `DumbPow2Alloc` only if profiling
  says the system allocator is a problem.
* **`ADOImport`** is COM/ADO against SQL Server, used at content-build time. The
  shipping game already links `ADOFake` instead, so the runtime never needs it.
* **`MapEdit`** (362 files) is MFC. It is a desktop tool, not part of the game.
* **The `.def` files.** Each module was a DLL exporting mangled C++ symbols. On
  Android everything links into one `.so`; `externA5` becomes plain `extern`.

## Testing

`platform/boot_harness.cpp` drives the ported subsystems against real game data
and is the port's regression test. It runs in two places from the same source:

* on device, rendered by the GLES console and logged to `adb logcat -s SilentStorm`
* on the host, headless: `build/host/silentstorm_hosttest <game-data-dir>`,
  exit code 0 when everything passes

Add a check there whenever a subsystem starts working. The host target builds in
seconds and is debuggable with lldb, which is how the 64-bit stream bug above was
found — do not debug engine logic on a device if the host can reproduce it.
