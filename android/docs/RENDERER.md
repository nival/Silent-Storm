# The renderer: what a GLES backend has to provide

This is the contract for reimplementing the five Direct3D 9 files behind the
engine's `NGfx` interface (`Gfx.cpp`, `GfxBuffers.cpp`, `GfxRender.cpp`,
`GfxEffects.cpp`, and the `GfxShaders.cpp`/`GfxShadersDescr.h` shader tables).
Everything else in `Main` — 264 files — compiles for arm64 already and talks
only to what is listed here. Line references are into `android/gen/Main/`.

## Shape of the interface

`Gfx.h`, `GfxRender.h`, `GfxBuffers.h`, `GfxEffects.h`, `GfxUtils.h`,
`GPixelFormat.h`. Include fan-out across the engine: `GfxBuffers.h` 23 files,
`Gfx.h` 22, `GfxRender.h` 15, `GfxEffects.h` 10, `GfxUtils.h` 10,
`GfxShaders.h` 5. `Render.h` and `GGeometryUtil.cpp` are *not* D3D (a CPU
rasteriser and a vertex-cache optimiser) and need nothing.

### Device lifecycle (`Gfx.h`)

`Init3D(HWND)`, `Done3D()`, `Is3DActive()` (device-loss check → recreate),
`SetMode(SVideoMode, SRenderTargetsInfo)`, `GetModesList`, `GetScreenRect`,
`Flip()`, `MakeScreenShot`, `CheckBackBufferSize`, `CheckDeviceCaps`, `SetGamma`.
The engine keeps a scene open the whole time: `BeginScene` at init, `EndScene` /
`Present` / `BeginScene` inside `Flip` (`Gfx.cpp:533`).

### `CRenderContext` (`GfxRender.h:100-178`) — the object everything draws with

A value-type state block; one is "current" and `Use()` applies the diff lazily.
Render targets: `SetScreenRT`, `SetTextureRT(tex, mip)`, `SetCubeTextureRT(cube,
face, mip)`, `SetVirtualRT` + `SetRegister(0..4)`, `ClearBuffers/Target/ZBuffer`.
State: `SetTransform(SFBTransform)` (a forward+backward 4×4), `SetAlphaCombine`,
`SetStencil`, `SetDepth`, `SetCulling`, `SetColorWrite`, `SetAlphaRef`.
Shaders: `SetPixelShader(SPShader)`, `SetVertexShader(SVShader)`,
`SetVSConst/SetPSConst(reg, CVec4*/CVec3/CVec4)`, `SetTexture(stage, tex,
bPointFilter)`, `SetTexture(stage, cube)`, `SetEffect(SEff*)`.
Draw: `DrawPrimitive(CGeometry*, CTriList*/STriangleList)`, `AddPrimitive(...)`
(batched), `Flush()`, `DrawLineStrip`.

Enum → GL translations are all in `GfxRender.cpp` `Apply<>` specialisations:
blend :294-343, stencil :345-478, depth :480-521, cull :523-538, colour-write
:540-549. Reproduce those tables exactly.

### Buffers and textures (`GfxBuffers.h`)

`CreateBuffer(formatID, size, STATIC|DYNAMIC)` → `ILinearBuffer` (Lock/Unlock);
`CBufferLock<T>` is the template the engine uses over it. `MakeTexture(x, y,
mips, pixelID, REGULAR|TARGET|TEXTURE_2D|TRANSPARENT_TEXTURE, WRAP|CLAMP)`,
`MakeCubeTexture(size, mips, pixelID, usage)`, `CTextureLock<TPixel>` (Lock a
mip, `operator[](y)` → row). `GetRegisterTexture(n)` reads a register back as
a texture (29 call sites).

**Atlas semantics — do not skip.** `TEXTURE_2D` / `TRANSPARENT_TEXTURE` do not
create a texture: they sub-allocate a rectangle out of a 1024×1024 A8R8G8B8
atlas (`textureCache`, 1 mip; `transparentCache`, 4 mips; `GfxBuffers.cpp:1321`).
`CTexture::GetXSize()` is the sub-rect, `GetTextureContainer()` returns the
physical atlas plus the rect, and `C2DQuadsRenderer` flushes when the container
changes. All UI/text goes through this.

Vertex buffers: one per (format, usage), sub-allocated by an LRU cache
(`CLinearBuffer`, `GfxBuffers.cpp:73-237`), `NOOVERWRITE` locks with a `DISCARD`
on the first lock of each frame. All geometry lives in one large VB and indices
are rebased by adding `nVBStart` — a hand-written base-vertex emulation
(`ReallyFastShiftingTransfer`, `GfxBuffers.cpp:903-954`, MMX — port like the
others). GLES 3.2 has `glDrawElementsBaseVertex`; on 3.0 rebase on the CPU or
use the 16-bit path (`GfxBuffers.cpp:1120-1128`).

### Vertex formats — three, `GfxRender.cpp:126-134`

* `SGeomVecFull` (32 B, the shader path, all 3D): `pos` f32×3 @0, `normal`
  D3DCOLOR (`z,y,x,w` u8) @12, `tex` SHORT2 @16, `texLM` SHORT2 @20, `texU`
  D3DCOLOR @24, `texV` D3DCOLOR @28. Normals are expanded in every VS by
  `mad r, v1, c3.xxx, c3.yyy` with `c3 = (2·255/254, −256/254)`. `tex` is scaled
  by `c6.x = 1/2048`, `texLM` by `c6.y = 1/65536`. In the 2D path `texU.dw` is a
  packed colour and `tex` is scaled ×8 (`GfxUtils.cpp:132`).
* `SGeomVecT1C1` (24 B): pos, D3DCOLOR colour, f32×2 tex — TnL/2D fallback.
* `SGeomVecNT1` (32 B): pos, f32×3 normal, f32×2 tex — TnL only.

Indices: `S3DTriangle` = 3×WORD. Primitives: TRIANGLELIST, LINELIST, LINESTRIP
only.

## Shaders (`GfxShaders.cpp`, generated, 369 KB)

77 vertex shaders (`vs.1.1`) and 78 pixel shaders (`ps.1.1`, each also as a
`ps.1.4` blob) as compiled D3D bytecode. **Every blob embeds a `DBUG` chunk with
the original assembly source**, so the full text of every shader is recoverable
from the file — write a small extractor first.

Every VS starts with the same six `dcl_` lines and `m4x4 oPos, v0, c10`.
Constant map preset in `InitEffects` (`GfxRender.cpp:1318-1328`): `c0`=0,
`c1`=1, `c2`=(0.5,1,2,4), `c3` normal expansion, `c4`=(0.25,1/16,6,4096),
`c5`=(18,0.5,0,0), `c6` UV scales, `c7` register-map scale, `c9` camera
position, `c10..c13` projection, `c14..c33` per effect (light dir/pos, colours,
shadow matrices, cached-lighting sky basis).

The 14 `SEff*` structs in `GfxEffects.h` bind specific VS/PS pairs and
constants — that table is the list of material paths the game actually uses.
`SEffConstLight` (9 callers) and `SEffTexture` matter most; `SEffColoredTexture`
is dead.

ps1.x features with no direct GLSL equivalent that must be hand-ported:
`texreg2ar` (dependent read, 6 shaders), `texm3x2pad/tex` (`psTexturedFog`),
`texm3x3pad`×2 + `texm3x3vspec` (`psBumpedMirror`), `cnd` (4 shaders), and the
`_x2/_x4/_bx2/_sat` modifiers everywhere — those are the engine's range
compression; a port that drops them loses all brightness.

Fixed-function is only the TnL fallback (`IsTnLDevice()`): `SetMaterial`
(once), one directional `SetLight`, `SetTransform(PROJECTION)`, and the
`SetTextureStageState` tables in the shader descriptors. Report `HL_RADEON2` and
never take that path. Alpha test is baked per pixel shader (`pStateRS`, 78
entries) plus `SetAlphaRef` — in GLES it becomes a `discard` in the fragment
shader.

## Render targets

The core of the lighting architecture is a bank of up to 5 screen-sized
"registers" (`N_MAX_REGISTERS`, `GfxRender.cpp:29`; count chosen in
`GInit.cpp:73-86`), sharing one depth/stencil surface. `SetVirtualRT` +
`SetRegister(n)` render into one; `GetRegisterTexture(n)` samples it. Register
0/1 hold light accumulation and shadow, 2/3 the cached-lighting temp/target.

Off-screen: `MakeTexture(..., TARGET, CLAMP)` sub-allocates square A8R8G8B8
targets from an LRU pool at pre-registered resolutions; shared square depth
buffers per size (`sharedZBuffers`, `GfxRender.cpp:1105`). `SetRT`
(`GfxRender.cpp:590`) unbinds all textures before switching target because
targets are also sampled.

Consumers: shadow maps (`GShadowMap.cpp:525`, depth encoded as **RGB colour**,
compared in the pixel shader — no hardware depth textures anywhere), cached
lighting via per-object cube maps rendered face by face (`GLightmapCalc.cpp:459`,
16..256², ~100 of them), UI-to-texture (`2DScene.cpp:111`), post-processing
composites (`GRenderExecute.cpp:498-884`, `psAlienEffect` refraction). Cube face
order in `EFace` is **+X, +Y, +Z, −X, −Y, −Z** — not GL's.

Stencil is required (`D24S8`): light/shadow masking with bits `0x80/0x7f/0x40`
(`GRenderExecute.cpp:133`), glow (`GSceneInternal.cpp:1596`).

## Formats and odds and ends

Texture formats: A8R8G8B8, R5G6B5, A1R5G5B5, A4R4G4B4, DXT1–5 — all nine live
in shipped `.mmp` assets. On GLES: `GL_EXT_texture_compression_s3tc` where
present (most Adreno/Mali), else `platform/dxt_decode.cpp`. A8R8G8B8 is BGRA
byte order. Fog is entirely shader/texture based (no `D3DRS_FOG*` anywhere).
Screenshots read the front buffer (`Gfx.cpp:488`) → `glReadPixels`. One
`D3DQUERYTYPE_VCACHE` query, stub to 10. Gamma ramp → a final pass, or drop.
Sampler defaults `GfxRender.cpp:1191`: trilinear, optional 2× anisotropic;
registers are always sampled with point filtering.

Keep a redundant-state cache like `renderStates`/`samplerStates`
(`GfxRender.cpp:52`) or the port will be `glEnable`-bound.

## Suggested order

1. Extract the shader sources from the `DBUG` chunks; check them in as text.
2. `Gfx.cpp` on EGL (the boot console already owns a context) with `Flip`,
   `GetScreenRect`, `MakeScreenShot`, `Is3DActive` on context loss.
3. `GfxBuffers.cpp`: buffers, `MakeTexture` in all four usages including the
   atlas, cube maps, render-target pool, base-vertex handling.
4. `GfxRender.cpp`: `CRenderContext` state application from the `Apply<>`
   tables, registers, and a GLSL program cache keyed by (VS, PS) pair — the 14
   effects define the pairs that occur.
5. Port shaders effect by effect, starting with `SEffConstLight`/`SEffTexture`
   (enough for the main menu and a static scene), then lighting/shadows.
