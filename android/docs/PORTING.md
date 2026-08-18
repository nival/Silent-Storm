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
        DBFormat      ████████████████████  ported; loads the retail game.db (130/130 tables, 222k records)
        Image         ████████████████████  ported; DXT1/3/5 software decoder in platform/
        Main          ████████████████████  269/269 files compile and link; the game loop runs on device
        d3d9gles      ██████████████████░░  D3D9 device on GLES 3.0; 155 shaders translated; first frames drawn
        Input         ████████████░░░░░░░░  NInput on Android keys/touch; camera scheme still to design
        FModSound     ████████░░░░░░░░░░░░  NFMSound implemented as a silent null back end
```

**On the Samsung Z Fold7 (2026-08-18):** the app boots, runs the boot harness
(38 checks pass), loads `Complete/game.db` in ~0.5 s through the retail-format
importer, initialises the renderer through the D3D9-on-GLES shim, and enters the
game's main loop: `CInterMissionInterface` steps and presents at ~120 fps into a
1024×768 virtual back buffer letterboxed on the 2184×1968 panel. What is on
screen is the intermission's grey clear colour; the two open items at the head
of the list below are getting the text/UI to draw and getting past the
intermission into the main menu.

## The order the remaining work should happen in

### 1. `wstring` — done

The engine serialises `std::wstring` straight into `game.db` and asset files,
and it is UTF-16 because `wchar_t` is 2 bytes on Win32. Android's is 4. The
staged sources now use `char16_t`/`std::u16string` (a mechanical pass in
`prepare_sources.py`), `WCHAR` is `char16_t` in the compat `windows.h`, and
`compat/src/wide_char.cpp` provides the char16_t CRT forms plus real
windows-1251/1252 conversion tables for `MultiByteToWideChar` and friends.

### 1b. `game.db` — done, by importing the retail format

No `game.db` in this repository is in the layout this source's `ADOFake` stub
expects (typed records serialised through their own `operator&`). Every shipped
file (`Data/`, `Complete/`, `Versions/Current/*/`) stores each table as an object
of a class registered as `0xA1843130` — a **generic column store**, i.e. a dump of
the ADO table the content team edited:

```
chunk 2   per record: vector<int>       int and bool columns, in column order
chunk 3   per record: vector<float>     float columns
chunk 4   per record: vector<wstring>   string columns (UTF-16)
chunk 5   column descriptors { 2: name (windows-1251), 3: type 0 int/1 bool/2 float/3 string }
chunk 6/7/8   names of the int / float / string columns, in value order
```

The table ids in the file's top-level `hash_map<int, CObj<CObjectBase>>` are the
same ids `DBFormat` registers (`45` = Strings, `0xE0000001` = RPGWeaponTypes…),
which is what makes the import possible: the shipping engine evidently ran the
ADO-style import at load time against that dump, and so does the port.
`platform/db_retail.cpp` (built instead of `ADOFake/BasicDBfake.cpp`) is the
`ADOImport/BasicDB.cpp` driver — PreCreate every table's records by their `ID`
column, then call each record's own `Import()` with the table cursor on its
row — with the chunk format as the "connection". The record classes' `Import()`
methods, which is where the schema lives, run unchanged.

Result on `Complete/game.db` (34 MB): 130/130 tables matched, 222,619 records,
14 columns the source asks for that the file no longer has (`RPGWeapons.
AmmoTypeID`, `MaleCustomHead1..6`… — the fields stay default), 25 tables in the
file this source has no class for (later features: medals, chests, hair/glasses
customisation). `Data/game.db` (3 MB) is a development cut of the same format
with too little data to be consistent (one weapon type) — use `Complete/`'s.
`A5_DB_DUMP=1` in the environment logs every table's columns.

Two details that matter: column names in the file are windows-1251 bytes and the
staged sources are UTF-8, and one source column name really does contain a
Cyrillic letter (`DamageMоd`); the loader converts. Relation tables
(`RPGPers2Scripts`) are not in the file, so `ImportRelation` returns empty lists.

### 2. `Image` — done

BMP/TGA/PNG/MMP loading builds and runs; libpng 1.0.9 is built from the tree
against the NDK's zlib with the x86 assembler back ends off. `ImagePack.cpp` (the
DXT *encoder* on a proprietary `s3tc.h`) is a tools-side dependency of TexConv
and is not part of the runtime. `platform/dxt_decode.cpp` decodes DXT1/3/5 in
software for GPUs without `GL_EXT_texture_compression_s3tc` and for the harness,
which checks decoded mean colour against the header's `dwAverageColor`.

### 3. `Main` — done: 269/269 files build, link and run

Getting there was almost entirely a matter of MSVC 7 leniencies handled once, as
mechanical passes or rules in `prepare_sources.py` — see "Traps" below for the
list. Every remaining piece of x86 inline assembly is gone: the MMX skinning in
`GCombiner.cpp` (verified against float within 0.01), the particle colour
modulate (bit-exact against an emulation of the instruction sequence), the
bilinear resample and the 2D blend in the software paths, the MMX AABB
accumulator, and the MMX `ReallyFastShiftingTransfer` in the vertex-buffer path.

The Direct3D 9 backend (`Gfx.cpp`, `GfxBuffers.cpp`, `GfxRender.cpp`,
`GfxEffects.cpp`, `GfxShaders.cpp` — ~6,500 lines) is **kept as written** and
runs on a Direct3D 9 implementation over GLES 3.0:
`compat/d3d9gles/d3d9.h` + `d3d9gles.cpp` (~2,000 lines). The full contract that
implementation honours — the ~40 device methods, the three vertex formats, the
constant-register map, the render-target model, the coordinate-system
differences — is [RENDERER.md](RENDERER.md). In short:

* framebuffer memory is always in D3D layout (row 0 at the top); every draw goes
  into an FBO with a y-flip in the vertex shader (`posFixup`), which is why the
  cull mode is inverted and `Present` is a flipped `glBlitFramebuffer` into the
  EGL surface, letterboxed to the requested mode (1024×768 by default)
* the 155 D3D shader-assembly programs the engine embeds (`GfxShadersDescr.h`)
  are recovered by `tools/extract_shaders.py` and translated to GLSL ES 3.00 by
  `tools/d3dasm2glsl.py`; the runtime finds a program by the FNV-1a hash of the
  assembly text (`shaders/glsl_table.cpp`) and links per (vs, ps, cube-sampler
  mask). All 155 compile and draw on the Adreno 830 (`platform/d3d_selftest.cpp`)
* textures keep a CPU shadow so `LockRect` works; DXT goes to the GPU when
  `GL_EXT_texture_compression_s3tc` is there and through `platform/dxt_decode.cpp`
  otherwise; vertex/index buffers keep a shadow with dirty ranges (`MarkDirty`,
  hinted from the engine's own lock calls by a staging rule) so the 32-bit
  index streaming path costs one upload per lock

`platform/game_entry.cpp` is `Game/Main.cpp`'s WinMain in three calls
(`a5_game_init` / `a5_game_step` / `a5_game_shutdown`), driven by
`android_main.cpp` once the boot harness passes and data is mounted.

### 4. Input, audio, video

* **Input**: `Input/Input.h` is the seam — a clean, DirectInput-free interface
  (`InitInput`, `PumpMessages`, `GetMessage(SMessage*)`, `GetControlID`).
  `platform/input_android.cpp` implements it: a control table with the original
  control names mapped to Android key codes, mouse buttons and wheel; touches
  become an absolute pointer position (`a5_set_pointer_position`) that
  `Cursor.cpp` reads in preference to integrated deltas (staging rule).
  `Bind.cpp` (action mapping) is portable and sits on top. A turn-based tactical
  game maps reasonably onto touch, but the port still needs a camera-control
  scheme of its own (pinch/drag → the camera binds).
* **Audio**: `FModSound/FMsound.h` is staged and `platform/audio_null.cpp`
  implements the whole `NFMSound` interface silently — every call succeeds and
  hands back a live handle, so the game runs without sound. The real back end
  (Oboe, or OpenSL ES) replaces that file behind the same functions.
* **Video**: Bink is licensed and absent. Cutscenes should be skipped or the
  container replaced; nothing else depends on it.
* **LifeStudio:HEAD** (facial animation for dialogue heads, proprietary) is
  stubbed under `compat/include/thirdparty-stubs/`: heads render in their neutral
  pose. Reviving it means licensing the SDK or writing a macro-muscle deformer.

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

**RTTI: MSVC lets you `dynamic_cast` from the wrong subobject; Itanium does
not.** The engine casts opaque pointers to `(CObjectBase*)` — forward-declared
`NGfx::CTexture*`, `void*` command contexts — and then `dynamic_cast`s. That is a
reinterpret_cast; MSVC's RTTI still finds the complete object from whatever vptr
is at that address, libc++abi looks for a `CObjectBase` subobject there, finds
none (it is a *virtual* base, at the end of the object) and returns null.
Symptom: every `CDynamicCast<I2DBuffer>( pTexture )` null, first texture load
crashes. `compat/src/rtti_compat.cpp` (`a5_cast_opaque<T>()`) does what MSVC did
— vptr → complete object and its `type_info` → walk the ABI's base-class
descriptors to the destination — and `CDynamicCast` and the `pContext` sites go
through it (staging rules). Any *new* `(CObjectBase*)something` cast is suspect.

**`LONG` is 32 bits.** The engine casts `CTRect<int>*` to `RECT*` and
`CTPoint<int>*` to `POINT*` when calling D3D (`GfxBuffers.cpp`'s texture
locker). With `typedef long LONG` on LP64 those structs double in size and the
lock rectangle reads garbage — a write 2^50 bytes past the texture. The compat
`windows.h` now types `LONG` as `int`, as Win32 does.

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
* **`ADOImport`** is COM/ADO against SQL Server, used at content-build time. Its
  *driver* logic is what `platform/db_retail.cpp` re-creates over the retail
  `game.db`; the COM part is never built.
* **`MapEdit`** (362 files) is MFC. It is a desktop tool, not part of the game.
* **The `.def` files.** Each module was a DLL exporting mangled C++ symbols. On
  Android everything links into one `.so`; `externA5` becomes plain `extern`.

## Testing

`platform/boot_harness.cpp` drives the ported subsystems against real game data
and is the port's regression test. It runs in two places from the same source:

* on device, rendered by the GLES console and logged to `adb logcat -s SilentStorm`
* on the host, headless: `build/host/silentstorm_hosttest <game-data-dir>`,
  exit code 0 when everything passes

Add a check there whenever a subsystem starts working. Past the harness, the
game itself runs on device only (it needs the GLES device): `scripts/build.sh
arm64-v8a && scripts/build_apk.sh arm64-v8a && scripts/run.sh`, then
`adb logcat -s SilentStorm` — the loop logs `game: N steps, M presents,
interface depth D` every five seconds, and the D3D shim warns on anything it
refuses. Note `build_apk.sh` only builds ABIs whose library is *missing*; rebuild
the library explicitly after source changes. The host target builds in
seconds and is debuggable with lldb, which is how the 64-bit stream bug above was
found — do not debug engine logic on a device if the host can reproduce it.
