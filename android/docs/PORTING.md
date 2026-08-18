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
        Image         ████████░░░░░░░░░░░░  staged, needs a DXT encoder replacement
        DBFormat      ████░░░░░░░░░░░░░░░░  staged, blocked on wchar_t (see below)
        Main          ░░░░░░░░░░░░░░░░░░░░  154k lines: renderer, scene, AI, UI, game
        Input         ░░░░░░░░░░░░░░░░░░░░  DirectInput -> touch; replace, do not wrap
        FModSound     ░░░░░░░░░░░░░░░░░░░░  FMOD 3 -> Oboe/OpenSL; replace
```

## The order the remaining work should happen in

### 1. `wstring` — do this before `DBFormat` (blocking)

The engine serialises `std::wstring` directly into `game.db` and into asset
files (`DBFormat/DataText.h`, `DBFormat/DataFormat.h`, `MiscDll/Commands.h`).
On Windows `wchar_t` is 2 bytes, so those files contain UTF-16. On Android
`wchar_t` is 4 bytes, so the same code reads and writes UTF-32 and every string
in the database comes out as garbage.

`-fshort-wchar` is not a fix: libc++ and bionic are compiled with 4-byte
`wchar_t`, and `std::wstring`'s `char_traits` calls into `wmemcpy`/`wmemcmp`.

The fix is to map the engine's wide string onto `std::u16string`:

* `wstring` → `std::u16string`, `WCHAR`/`wchar_t` → `char16_t` in engine code
* `Misc/StrProc.cpp`'s ten or so wide-character helpers (`WideCharToMultiByte`,
  `MultiByteToWideChar`, `vswprintf`, `wcscat`, `_itow`) need `char16_t` versions
* the narrow↔wide conversion has to keep assuming CP1251, which is what
  `Misc/StrProc.cpp:15` (`nCodePage = CP_ACP`) means on a Russian Windows build

Once that lands, `DBFormat` should build and `game.db` should load, which is the
next real milestone: the port would have the whole object database in memory.

### 2. `Image`

Loading is portable (BMP/TGA/PNG/MMP). The one external dependency is
`ImagePack.cpp`'s `<s3tc.h>`, a proprietary DXT *encoder*. The shipped assets are
already DXT-compressed inside MMP containers, so the runtime only needs to
*decode* — the encoder is a tools-side dependency and can be stubbed out.

libpng is vendored in the tree with both assembly backends; build it with
`PNG_USE_PNGVCRD` and `PNG_USE_PNGGCCRD` off.

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
