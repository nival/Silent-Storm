/*
 *  d3d9.h -- a small Direct3D 9 implemented on OpenGL ES 3.0, for the engine.
 *
 *  Why this exists.  The engine's renderer (Main/Gfx.cpp, GfxBuffers.cpp,
 *  GfxRender.cpp -- about 3,800 lines) is mostly API-neutral machinery Nival
 *  wrote and tuned: LRU-cached vertex buffers, a texture atlas for the UI, a
 *  render-target pool, index batching, redundant-state filtering.  Only the
 *  leaves touch Direct3D, through ~40 IDirect3DDevice9 methods and a handful of
 *  resource objects.  Reimplementing that slice of D3D9 over GLES keeps all of
 *  the engine's renderer as it was, which is worth far more than a rewrite: its
 *  behaviour is the one the game was built and tested against.
 *
 *  This is *not* a general D3D9 implementation.  It covers exactly what those
 *  three files call (see docs/RENDERER.md for the enumeration) and asserts on
 *  anything else.  The objects are plain C++ classes with reference counting,
 *  not COM; the engine only ever calls methods through com_ptr<>.
 *
 *  Conventions that matter (details in d3d9gles.cpp):
 *   - Framebuffer memory is always in D3D layout (row 0 = top).  Every draw
 *     goes to an FBO with the clip-space y flipped, so render targets the
 *     engine samples back have the layout its projective UVs expect; the
 *     final Present blits the virtual back buffer to the window.
 *   - The back buffer is a virtual FBO of the size the engine asks for
 *     (SetMode / D3DPRESENT_PARAMETERS), presented scaled to the window.
 *   - Textures keep a CPU shadow so LockRect works like D3D's managed pool.
 *   - Shaders: CreateVertexShader/CreatePixelShader receive the engine's D3D
 *     bytecode, find the assembly text in its DBUG chunk, and look the GLSL up
 *     in the generated table (shaders/glsl_table.cpp) by hash.
 */
#ifndef A5_D3D9GLES_D3D9_H
#define A5_D3D9GLES_D3D9_H

#include "windows.h"
#include <stdint.h>

/* ----- HRESULT ------------------------------------------------------------ */
#define D3D_OK                     ((HRESULT)0)
#define D3DERR_DEVICELOST          ((HRESULT)0x88760868L)
#define D3DERR_DEVICENOTRESET      ((HRESULT)0x88760869L)
#define D3DERR_OUTOFVIDEOMEMORY    ((HRESULT)0x8876017CL)
#define D3DERR_INVALIDCALL         ((HRESULT)0x8876086CL)
#define D3DERR_NOTAVAILABLE        ((HRESULT)0x8876086AL)
#define D3D_SDK_VERSION            32
#define D3DADAPTER_DEFAULT         0

/* ----- Enumerations ------------------------------------------------------- */
typedef enum _D3DFORMAT {
    D3DFMT_UNKNOWN       = 0,
    D3DFMT_R8G8B8        = 20,
    D3DFMT_A8R8G8B8      = 21,
    D3DFMT_X8R8G8B8      = 22,
    D3DFMT_R5G6B5        = 23,
    D3DFMT_X1R5G5B5      = 24,
    D3DFMT_A1R5G5B5      = 25,
    D3DFMT_A4R4G4B4      = 26,
    D3DFMT_R3G3B2        = 27,
    D3DFMT_A8            = 28,
    D3DFMT_A8R3G3B2      = 29,
    D3DFMT_X4R4G4B4      = 30,
    D3DFMT_A8P8          = 40,
    D3DFMT_P8            = 41,
    D3DFMT_L8            = 50,
    D3DFMT_A8L8          = 51,
    D3DFMT_A4L4          = 52,
    D3DFMT_V8U8          = 60,
    D3DFMT_L6V5U5        = 61,
    D3DFMT_X8L8V8U8      = 62,
    D3DFMT_D16_LOCKABLE  = 70,
    D3DFMT_D32           = 71,
    D3DFMT_D15S1         = 73,
    D3DFMT_D24S8         = 75,
    D3DFMT_D24X8         = 77,
    D3DFMT_D24X4S4       = 79,
    D3DFMT_D16           = 80,
    D3DFMT_INDEX16       = 101,
    D3DFMT_INDEX32       = 102,
    D3DFMT_DXT1          = 0x31545844,   /* 'DXT1' */
    D3DFMT_DXT2          = 0x32545844,
    D3DFMT_DXT3          = 0x33545844,
    D3DFMT_DXT4          = 0x34545844,
    D3DFMT_DXT5          = 0x35545844,
    D3DFMT_FORCE_DWORD   = 0x7fffffff
} D3DFORMAT;
/* The engine spells one non-existent format in a comment; harmless to define. */
#define D3DFMT_UNKNOWN_D24S8 D3DFMT_UNKNOWN

typedef enum _D3DPOOL {
    D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2, D3DPOOL_SCRATCH = 3
} D3DPOOL;

typedef enum _D3DDEVTYPE { D3DDEVTYPE_HAL = 1, D3DDEVTYPE_REF = 2 } D3DDEVTYPE;
typedef D3DDEVTYPE _D3DDEVTYPE;

typedef enum _D3DRESOURCETYPE {
    D3DRTYPE_SURFACE = 1, D3DRTYPE_VOLUME = 2, D3DRTYPE_TEXTURE = 3, D3DRTYPE_VOLUMETEXTURE = 4,
    D3DRTYPE_CUBETEXTURE = 5, D3DRTYPE_VERTEXBUFFER = 6, D3DRTYPE_INDEXBUFFER = 7
} D3DRESOURCETYPE;

typedef enum _D3DMULTISAMPLE_TYPE { D3DMULTISAMPLE_NONE = 0 } D3DMULTISAMPLE_TYPE;
typedef enum _D3DSWAPEFFECT { D3DSWAPEFFECT_DISCARD = 1 } D3DSWAPEFFECT;

typedef enum _D3DPRIMITIVETYPE {
    D3DPT_POINTLIST = 1, D3DPT_LINELIST = 2, D3DPT_LINESTRIP = 3,
    D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5, D3DPT_TRIANGLEFAN = 6
} D3DPRIMITIVETYPE;

typedef enum _D3DCUBEMAP_FACES {
    D3DCUBEMAP_FACE_POSITIVE_X = 0, D3DCUBEMAP_FACE_NEGATIVE_X = 1,
    D3DCUBEMAP_FACE_POSITIVE_Y = 2, D3DCUBEMAP_FACE_NEGATIVE_Y = 3,
    D3DCUBEMAP_FACE_POSITIVE_Z = 4, D3DCUBEMAP_FACE_NEGATIVE_Z = 5
} D3DCUBEMAP_FACES;

typedef enum _D3DRENDERSTATETYPE {
    D3DRS_ZENABLE = 7, D3DRS_FILLMODE = 8, D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15,
    D3DRS_SRCBLEND = 19, D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23,
    D3DRS_ALPHAREF = 24, D3DRS_ALPHAFUNC = 25, D3DRS_DITHERENABLE = 26, D3DRS_ALPHABLENDENABLE = 27,
    D3DRS_FOGENABLE = 28, D3DRS_STENCILENABLE = 52, D3DRS_STENCILFAIL = 53, D3DRS_STENCILZFAIL = 54,
    D3DRS_STENCILPASS = 55, D3DRS_STENCILFUNC = 56, D3DRS_STENCILREF = 57, D3DRS_STENCILMASK = 58,
    D3DRS_STENCILWRITEMASK = 59, D3DRS_TEXTUREFACTOR = 60, D3DRS_LIGHTING = 137,
    D3DRS_COLORWRITEENABLE = 168,
    D3DRS_FORCE_DWORD = 0x7fffffff
} D3DRENDERSTATETYPE;

typedef enum _D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3, D3DTSS_ALPHAOP = 4,
    D3DTSS_ALPHAARG1 = 5, D3DTSS_ALPHAARG2 = 6, D3DTSS_TEXCOORDINDEX = 11,
    D3DTSS_TEXTURETRANSFORMFLAGS = 24, D3DTSS_COLORARG0 = 26, D3DTSS_ALPHAARG0 = 27,
    D3DTSS_RESULTARG = 28,
    D3DTSS_FORCE_DWORD = 0x7fffffff
} D3DTEXTURESTAGESTATETYPE;

typedef enum _D3DSAMPLERSTATETYPE {
    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_ADDRESSW = 3, D3DSAMP_BORDERCOLOR = 4,
    D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7, D3DSAMP_MIPMAPLODBIAS = 8,
    D3DSAMP_MAXMIPLEVEL = 9, D3DSAMP_MAXANISOTROPY = 10,
    D3DSAMP_FORCE_DWORD = 0x7fffffff
} D3DSAMPLERSTATETYPE;

typedef enum _D3DTEXTUREOP {
    D3DTOP_DISABLE = 1, D3DTOP_SELECTARG1 = 2, D3DTOP_SELECTARG2 = 3, D3DTOP_MODULATE = 4,
    D3DTOP_MODULATE2X = 5, D3DTOP_MODULATE4X = 6, D3DTOP_ADD = 7, D3DTOP_ADDSIGNED = 8,
    D3DTOP_ADDSIGNED2X = 9, D3DTOP_SUBTRACT = 10, D3DTOP_ADDSMOOTH = 11,
    D3DTOP_BLENDDIFFUSEALPHA = 12, D3DTOP_BLENDTEXTUREALPHA = 13, D3DTOP_BLENDFACTORALPHA = 14,
    D3DTOP_BLENDTEXTUREALPHAPM = 15, D3DTOP_BLENDCURRENTALPHA = 16, D3DTOP_PREMODULATE = 17,
    D3DTOP_MODULATEALPHA_ADDCOLOR = 18, D3DTOP_MODULATECOLOR_ADDALPHA = 19,
    D3DTOP_MODULATEINVALPHA_ADDCOLOR = 20, D3DTOP_MODULATEINVCOLOR_ADDALPHA = 21,
    D3DTOP_BUMPENVMAP = 22, D3DTOP_BUMPENVMAPLUMINANCE = 23, D3DTOP_DOTPRODUCT3 = 24,
    D3DTOP_MULTIPLYADD = 25, D3DTOP_LERP = 26,
    D3DTOP_FORCE_DWORD = 0x7fffffff
} D3DTEXTUREOP;

#define D3DTA_SELECTMASK   0x0000000f
#define D3DTA_DIFFUSE      0x00000000
#define D3DTA_CURRENT      0x00000001
#define D3DTA_TEXTURE      0x00000002
#define D3DTA_TFACTOR      0x00000003
#define D3DTA_SPECULAR     0x00000004
#define D3DTA_TEMP         0x00000005
#define D3DTA_COMPLEMENT   0x00000010
#define D3DTA_ALPHAREPLICATE 0x00000020

typedef enum _D3DTEXTURETRANSFORMFLAGS {
    D3DTTFF_DISABLE = 0, D3DTTFF_COUNT1 = 1, D3DTTFF_COUNT2 = 2, D3DTTFF_COUNT3 = 3,
    D3DTTFF_COUNT4 = 4, D3DTTFF_PROJECTED = 256, D3DTTFF_FORCE_DWORD = 0x7fffffff
} D3DTEXTURETRANSFORMFLAGS;

typedef enum _D3DTEXTUREFILTERTYPE {
    D3DTEXF_NONE = 0, D3DTEXF_POINT = 1, D3DTEXF_LINEAR = 2, D3DTEXF_ANISOTROPIC = 3
} D3DTEXTUREFILTERTYPE;

typedef enum _D3DTEXTUREADDRESS {
    D3DTADDRESS_WRAP = 1, D3DTADDRESS_MIRROR = 2, D3DTADDRESS_CLAMP = 3, D3DTADDRESS_BORDER = 4
} D3DTEXTUREADDRESS;

typedef enum _D3DCMPFUNC {
    D3DCMP_NEVER = 1, D3DCMP_LESS = 2, D3DCMP_EQUAL = 3, D3DCMP_LESSEQUAL = 4,
    D3DCMP_GREATER = 5, D3DCMP_NOTEQUAL = 6, D3DCMP_GREATEREQUAL = 7, D3DCMP_ALWAYS = 8
} D3DCMPFUNC;

typedef enum _D3DBLEND {
    D3DBLEND_ZERO = 1, D3DBLEND_ONE = 2, D3DBLEND_SRCCOLOR = 3, D3DBLEND_INVSRCCOLOR = 4,
    D3DBLEND_SRCALPHA = 5, D3DBLEND_INVSRCALPHA = 6, D3DBLEND_DESTALPHA = 7,
    D3DBLEND_INVDESTALPHA = 8, D3DBLEND_DESTCOLOR = 9, D3DBLEND_INVDESTCOLOR = 10,
    D3DBLEND_SRCALPHASAT = 11
} D3DBLEND;

typedef enum _D3DCULL { D3DCULL_NONE = 1, D3DCULL_CW = 2, D3DCULL_CCW = 3 } D3DCULL;

typedef enum _D3DSTENCILOP {
    D3DSTENCILOP_KEEP = 1, D3DSTENCILOP_ZERO = 2, D3DSTENCILOP_REPLACE = 3,
    D3DSTENCILOP_INCRSAT = 4, D3DSTENCILOP_DECRSAT = 5, D3DSTENCILOP_INVERT = 6,
    D3DSTENCILOP_INCR = 7, D3DSTENCILOP_DECR = 8
} D3DSTENCILOP;

typedef enum _D3DFILLMODE { D3DFILL_POINT = 1, D3DFILL_WIREFRAME = 2, D3DFILL_SOLID = 3 } D3DFILLMODE;
typedef enum _D3DZBUFFERTYPE { D3DZB_FALSE = 0, D3DZB_TRUE = 1, D3DZB_USEW = 2 } D3DZBUFFERTYPE;

#define D3DCOLORWRITEENABLE_RED   (1L<<0)
#define D3DCOLORWRITEENABLE_GREEN (1L<<1)
#define D3DCOLORWRITEENABLE_BLUE  (1L<<2)
#define D3DCOLORWRITEENABLE_ALPHA (1L<<3)

#define D3DCLEAR_TARGET  0x00000001l
#define D3DCLEAR_ZBUFFER 0x00000002l
#define D3DCLEAR_STENCIL 0x00000004l

typedef enum _D3DTRANSFORMSTATETYPE {
    D3DTS_VIEW = 2, D3DTS_PROJECTION = 3, D3DTS_WORLD = 256
} D3DTRANSFORMSTATETYPE;

typedef enum _D3DLIGHTTYPE { D3DLIGHT_POINT = 1, D3DLIGHT_SPOT = 2, D3DLIGHT_DIRECTIONAL = 3 } D3DLIGHTTYPE;

typedef enum _D3DQUERYTYPE { D3DQUERYTYPE_VCACHE = 4, D3DQUERYTYPE_EVENT = 8 } D3DQUERYTYPE;
#define D3DISSUE_END   (1 << 0)
#define D3DISSUE_BEGIN (1 << 1)
#define D3DGETDATA_FLUSH (1 << 0)

#define D3DSGR_NO_CALIBRATION 0x00000000L
#define D3DSGR_CALIBRATE      0x00000001L

/* Usage / lock / create flags */
#define D3DUSAGE_RENDERTARGET       0x00000001L
#define D3DUSAGE_DEPTHSTENCIL       0x00000002L
#define D3DUSAGE_WRITEONLY          0x00000008L
#define D3DUSAGE_SOFTWAREPROCESSING 0x00000010L
#define D3DUSAGE_DYNAMIC            0x00000200L
#define D3DLOCK_READONLY            0x00000010L
#define D3DLOCK_DISCARD             0x00002000L
#define D3DLOCK_NOOVERWRITE         0x00001000L
#define D3DLOCK_NO_DIRTY_UPDATE     0x00008000L
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x00000020L
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x00000040L
#define D3DCREATE_PUREDEVICE                0x00000010L
#define D3DPRESENT_INTERVAL_DEFAULT   0x00000000L
#define D3DPRESENT_INTERVAL_ONE       0x00000001L
#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000L
#define D3DPRESENT_RATE_DEFAULT       0x00000000L

/* Caps bits the engine tests */
#define D3DPTEXTURECAPS_POW2               0x00000002L
#define D3DPTEXTURECAPS_NONPOW2CONDITIONAL 0x00000100L
#define D3DPTEXTURECAPS_CUBEMAP            0x00000800L
#define D3DPTEXTURECAPS_MIPCUBEMAP         0x00010000L
#define D3DVS_VERSION(_Major,_Minor) (0xFFFE0000|((_Major)<<8)|(_Minor))
#define D3DPS_VERSION(_Major,_Minor) (0xFFFF0000|((_Major)<<8)|(_Minor))

/* FVF (TnL fallback only) */
#define D3DFVF_XYZ     0x002
#define D3DFVF_NORMAL  0x010
#define D3DFVF_DIFFUSE 0x040
#define D3DFVF_TEX1    0x100

/* Vertex declarations */
typedef enum _D3DDECLTYPE {
    D3DDECLTYPE_FLOAT1 = 0, D3DDECLTYPE_FLOAT2 = 1, D3DDECLTYPE_FLOAT3 = 2, D3DDECLTYPE_FLOAT4 = 3,
    D3DDECLTYPE_D3DCOLOR = 4, D3DDECLTYPE_UBYTE4 = 5, D3DDECLTYPE_SHORT2 = 6, D3DDECLTYPE_SHORT4 = 7,
    D3DDECLTYPE_UNUSED = 17
} D3DDECLTYPE;
typedef enum _D3DDECLMETHOD { D3DDECLMETHOD_DEFAULT = 0 } D3DDECLMETHOD;
typedef enum _D3DDECLUSAGE {
    D3DDECLUSAGE_POSITION = 0, D3DDECLUSAGE_BLENDWEIGHT = 1, D3DDECLUSAGE_BLENDINDICES = 2,
    D3DDECLUSAGE_NORMAL = 3, D3DDECLUSAGE_PSIZE = 4, D3DDECLUSAGE_TEXCOORD = 5,
    D3DDECLUSAGE_TANGENT = 6, D3DDECLUSAGE_BINORMAL = 7, D3DDECLUSAGE_TESSFACTOR = 8,
    D3DDECLUSAGE_POSITIONT = 9, D3DDECLUSAGE_COLOR = 10, D3DDECLUSAGE_FOG = 11,
    D3DDECLUSAGE_DEPTH = 12, D3DDECLUSAGE_SAMPLE = 13
} D3DDECLUSAGE;
typedef struct _D3DVERTEXELEMENT9 {
    WORD Stream;
    WORD Offset;
    BYTE Type;
    BYTE Method;
    BYTE Usage;
    BYTE UsageIndex;
} D3DVERTEXELEMENT9;
#define D3DDECL_END() {0xFF,0,D3DDECLTYPE_UNUSED,0,0,0}

/* ----- Structures --------------------------------------------------------- */
typedef struct _D3DDISPLAYMODE {
    UINT Width, Height, RefreshRate;
    D3DFORMAT Format;
} D3DDISPLAYMODE;

typedef struct _D3DPRESENT_PARAMETERS_ {
    UINT BackBufferWidth, BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD MultiSampleQuality;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct _D3DCAPS9 {
    D3DDEVTYPE DeviceType;
    UINT AdapterOrdinal;
    DWORD Caps, Caps2, Caps3;
    DWORD PresentationIntervals;
    DWORD CursorCaps;
    DWORD DevCaps;
    DWORD PrimitiveMiscCaps, RasterCaps, ZCmpCaps, SrcBlendCaps, DestBlendCaps, AlphaCmpCaps;
    DWORD ShadeCaps, TextureCaps, TextureFilterCaps, CubeTextureFilterCaps, VolumeTextureFilterCaps;
    DWORD TextureAddressCaps, VolumeTextureAddressCaps, LineCaps;
    DWORD MaxTextureWidth, MaxTextureHeight, MaxVolumeExtent, MaxTextureRepeat, MaxTextureAspectRatio;
    DWORD MaxAnisotropy;
    float MaxVertexW, GuardBandLeft, GuardBandTop, GuardBandRight, GuardBandBottom, ExtentsAdjust;
    DWORD StencilCaps, FVFCaps, TextureOpCaps;
    DWORD MaxTextureBlendStages, MaxSimultaneousTextures, VertexProcessingCaps;
    DWORD MaxActiveLights, MaxUserClipPlanes, MaxVertexBlendMatrices, MaxVertexBlendMatrixIndex;
    float MaxPointSize;
    DWORD MaxPrimitiveCount, MaxVertexIndex, MaxStreams, MaxStreamStride;
    DWORD VertexShaderVersion, MaxVertexShaderConst, PixelShaderVersion;
    float PixelShader1xMaxValue;
} D3DCAPS9;

typedef struct _D3DADAPTER_IDENTIFIER9 {
    char Driver[512];
    char Description[512];
    char DeviceName[32];
    DWORD VendorId, DeviceId, SubSysId, Revision;
} D3DADAPTER_IDENTIFIER9;

typedef struct _D3DLOCKED_RECT { INT Pitch; void *pBits; } D3DLOCKED_RECT;
typedef struct _D3DRECT { LONG x1, y1, x2, y2; } D3DRECT;
typedef struct _D3DSURFACE_DESC {
    D3DFORMAT Format; D3DRESOURCETYPE Type; DWORD Usage; D3DPOOL Pool;
    D3DMULTISAMPLE_TYPE MultiSampleType; DWORD MultiSampleQuality; UINT Width, Height;
} D3DSURFACE_DESC;
typedef struct _D3DMATRIX { float m[4][4]; } D3DMATRIX;
typedef struct _D3DCOLORVALUE { float r, g, b, a; } D3DCOLORVALUE;
typedef struct _D3DVECTOR { float x, y, z; } D3DVECTOR;
typedef struct _D3DLIGHT9 {
    D3DLIGHTTYPE Type; D3DCOLORVALUE Diffuse, Specular, Ambient; D3DVECTOR Position, Direction;
    float Range, Falloff, Attenuation0, Attenuation1, Attenuation2, Theta, Phi;
} D3DLIGHT9;
typedef struct _D3DMATERIAL9 { D3DCOLORVALUE Diffuse, Ambient, Specular, Emissive; float Power; } D3DMATERIAL9;
typedef struct _D3DGAMMARAMP { WORD red[256], green[256], blue[256]; } D3DGAMMARAMP;
typedef struct _D3DDEVINFO_VCACHE { DWORD Pattern, OptMethod, CacheSize, MagicNumber; } D3DDEVINFO_VCACHE;
typedef struct _D3DVIEWPORT9 { DWORD X, Y, Width, Height; float MinZ, MaxZ; } D3DVIEWPORT9;

/* ----- Objects ------------------------------------------------------------ */
/*  Reference-counted, like COM, so NWin32Helper::com_ptr<> works unchanged.  */
class IDirect3DUnknown9
{
public:
    IDirect3DUnknown9() : m_nRefCount( 1 ) {}
    virtual ~IDirect3DUnknown9() {}
    ULONG AddRef()  { return ++m_nRefCount; }
    ULONG Release() { ULONG n = --m_nRefCount; if ( n == 0 ) delete this; return n; }
private:
    ULONG m_nRefCount;
};

class IDirect3DDevice9;
class IDirect3DTexture9;
class IDirect3DCubeTexture9;

class IDirect3DSurface9 : public IDirect3DUnknown9
{
public:
    virtual HRESULT LockRect( D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags ) = 0;
    virtual HRESULT UnlockRect() = 0;
    virtual HRESULT GetDesc( D3DSURFACE_DESC *pDesc ) = 0;
};

class IDirect3DBaseTexture9 : public IDirect3DUnknown9
{
public:
    virtual D3DRESOURCETYPE GetType() = 0;
    virtual void AddDirtyRect( const RECT * ) {}
};

class IDirect3DTexture9 : public IDirect3DBaseTexture9
{
public:
    virtual HRESULT GetSurfaceLevel( UINT Level, IDirect3DSurface9 **ppSurfaceLevel ) = 0;
    virtual HRESULT LockRect( UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags ) = 0;
    virtual HRESULT UnlockRect( UINT Level ) = 0;
};

class IDirect3DCubeTexture9 : public IDirect3DBaseTexture9
{
public:
    virtual HRESULT GetCubeMapSurface( D3DCUBEMAP_FACES Face, UINT Level, IDirect3DSurface9 **ppSurface ) = 0;
};

class IDirect3DVertexBuffer9 : public IDirect3DUnknown9
{
public:
    virtual HRESULT Lock( UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags ) = 0;
    virtual HRESULT Unlock() = 0;
    /*  Not part of D3D: the engine's sub-allocator tells the shim which byte
     *  range of a whole-buffer lock it actually wrote, so only that is uploaded
     *  (see the MarkDirty rules in tools/prepare_sources.py). */
    virtual void MarkDirty( UINT nOffset, UINT nSize ) = 0;
};

class IDirect3DIndexBuffer9 : public IDirect3DUnknown9
{
public:
    virtual HRESULT Lock( UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags ) = 0;
    virtual HRESULT Unlock() = 0;
    virtual void MarkDirty( UINT nOffset, UINT nSize ) = 0;
};

class IDirect3DVertexShader9 : public IDirect3DUnknown9 {};
class IDirect3DPixelShader9 : public IDirect3DUnknown9 {};
class IDirect3DVertexDeclaration9 : public IDirect3DUnknown9 {};

class IDirect3DQuery9 : public IDirect3DUnknown9
{
public:
    virtual HRESULT Issue( DWORD dwIssueFlags ) = 0;
    virtual HRESULT GetData( void *pData, DWORD dwSize, DWORD dwGetDataFlags ) = 0;
};

class IDirect3DDevice9 : public IDirect3DUnknown9
{
public:
    /* lifecycle */
    virtual HRESULT TestCooperativeLevel() = 0;
    virtual HRESULT Reset( D3DPRESENT_PARAMETERS *pPresentationParameters ) = 0;
    virtual HRESULT Present( const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const void *pDirtyRegion ) = 0;
    virtual HRESULT BeginScene() = 0;
    virtual HRESULT EndScene() = 0;
    virtual HRESULT ValidateDevice( DWORD *pNumPasses ) = 0;
    virtual void    SetGammaRamp( UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp ) = 0;
    virtual void    GetGammaRamp( UINT iSwapChain, D3DGAMMARAMP *pRamp ) = 0;
    virtual HRESULT GetFrontBufferData( UINT iSwapChain, IDirect3DSurface9 *pDestSurface ) = 0;
    /* resources */
    virtual HRESULT CreateTexture( UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle ) = 0;
    virtual HRESULT CreateCubeTexture( UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle ) = 0;
    virtual HRESULT CreateVertexBuffer( UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle ) = 0;
    virtual HRESULT CreateIndexBuffer( UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle ) = 0;
    virtual HRESULT CreateDepthStencilSurface( UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle ) = 0;
    virtual HRESULT CreateOffscreenPlainSurface( UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle ) = 0;
    virtual HRESULT UpdateSurface( IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint ) = 0;
    virtual HRESULT CopyRects( IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRects, UINT cRects, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoints ) = 0;
    virtual HRESULT CreateVertexShader( const DWORD *pFunction, IDirect3DVertexShader9 **ppShader ) = 0;
    virtual HRESULT CreatePixelShader( const DWORD *pFunction, IDirect3DPixelShader9 **ppShader ) = 0;
    virtual HRESULT CreateVertexDeclaration( const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl ) = 0;
    virtual HRESULT CreateQuery( D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery ) = 0;
    /* render targets */
    virtual HRESULT SetRenderTarget( DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget ) = 0;
    virtual HRESULT GetRenderTarget( DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget ) = 0;
    virtual HRESULT SetDepthStencilSurface( IDirect3DSurface9 *pNewZStencil ) = 0;
    virtual HRESULT GetDepthStencilSurface( IDirect3DSurface9 **ppZStencilSurface ) = 0;
    virtual HRESULT Clear( DWORD Count, const D3DRECT *pRects, DWORD Flags, DWORD Color, float Z, DWORD Stencil ) = 0;
    /* state */
    virtual HRESULT SetRenderState( D3DRENDERSTATETYPE State, DWORD Value ) = 0;
    virtual HRESULT SetSamplerState( DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value ) = 0;
    virtual HRESULT SetTextureStageState( DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value ) = 0;
    virtual HRESULT SetTexture( DWORD Stage, IDirect3DBaseTexture9 *pTexture ) = 0;
    virtual HRESULT SetTransform( D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix ) = 0;
    virtual HRESULT SetMaterial( const D3DMATERIAL9 *pMaterial ) = 0;
    virtual HRESULT SetLight( DWORD Index, const D3DLIGHT9 *pLight ) = 0;
    virtual HRESULT LightEnable( DWORD Index, BOOL Enable ) = 0;
    virtual HRESULT SetSoftwareVertexProcessing( BOOL bSoftware ) = 0;
    virtual HRESULT SetFVF( DWORD FVF ) = 0;
    /* shaders and streams */
    virtual HRESULT SetVertexShader( IDirect3DVertexShader9 *pShader ) = 0;
    virtual HRESULT SetPixelShader( IDirect3DPixelShader9 *pShader ) = 0;
    virtual HRESULT SetVertexDeclaration( IDirect3DVertexDeclaration9 *pDecl ) = 0;
    virtual HRESULT SetVertexShaderConstantF( UINT StartRegister, const float *pConstantData, UINT Vector4fCount ) = 0;
    virtual HRESULT SetPixelShaderConstantF( UINT StartRegister, const float *pConstantData, UINT Vector4fCount ) = 0;
    virtual HRESULT SetStreamSource( UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride ) = 0;
    virtual HRESULT SetIndices( IDirect3DIndexBuffer9 *pIndexData ) = 0;
    /* draw */
    virtual HRESULT DrawPrimitive( D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount ) = 0;
    virtual HRESULT DrawIndexedPrimitive( D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount ) = 0;
};

class IDirect3D9 : public IDirect3DUnknown9
{
public:
    virtual HRESULT GetDeviceCaps( UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps ) = 0;
    virtual HRESULT GetAdapterIdentifier( UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier ) = 0;
    virtual HRESULT GetAdapterDisplayMode( UINT Adapter, D3DDISPLAYMODE *pMode ) = 0;
    virtual UINT    GetAdapterModeCount( UINT Adapter, D3DFORMAT Format ) = 0;
    virtual HRESULT EnumAdapterModes( UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode ) = 0;
    virtual HRESULT CheckDeviceFormat( UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat ) = 0;
    virtual HRESULT CheckDepthStencilMatch( UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat ) = 0;
    virtual HRESULT CreateDevice( UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface ) = 0;
};

IDirect3D9 *Direct3DCreate9( UINT SDKVersion );

/* ----- Platform hooks (not D3D) -------------------------------------------- */
/*  The platform layer owns EGL.  It tells the shim the window size and gives it
 *  a way to swap; the shim never touches EGL itself. */
struct A5D3DPlatformHooks
{
    int  (*getWindowWidth)();
    int  (*getWindowHeight)();
    void (*present)();                 /* eglSwapBuffers on the current surface */
    int  (*isSurfaceAlive)();          /* 0 while the window is gone            */
};
void A5D3DSetPlatformHooks( const A5D3DPlatformHooks *pHooks );

/*  Maps a window pixel to a back-buffer pixel through the Present scaling, for
 *  the input layer.  Returns 0 if the point falls in the letterbox. */
int  A5D3DWindowToBackBuffer( float fWindowX, float fWindowY, float *pfBackX, float *pfBackY );

#endif
