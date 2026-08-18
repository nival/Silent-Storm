#include "d3d_selftest.h"
#include "d3d9.h"
#include "a5_log.h"

#include <GLES3/gl3.h>
#include <string.h>
#include <stdio.h>
#include <vector>

/* The engine's shader tables (Main/GfxShaders.cpp): the bytecode the device
 * receives at run time.  Compiling every entry from here is the real test of
 * the extract -> translate -> lookup chain. */
struct SVShader;
struct SPShader;
extern SVShader *vsAllShaders[ 77 ];
extern SPShader *psAllShaders[ 78 ];
#include "Main/GfxShadersDescr.h"

namespace {

typedef void ( *ReportFn )( void *, EBootStatus, double, const char * );

struct SReporter
{
    void *p; ReportFn fn;
    void operator()( EBootStatus s, const char *pszFormat, ... )
    {
        char szBuf[ 512 ];
        va_list args;
        va_start( args, pszFormat );
        vsnprintf( szBuf, sizeof( szBuf ), pszFormat, args );
        va_end( args );
        fn( p, s, 0.0, szBuf );
    }
};

int  g_nW = 256, g_nH = 256;
int  HookW() { return g_nW; }
int  HookH() { return g_nH; }
void HookPresent() {}
int  HookAlive() { return 1; }

/* A vertex in the engine's SGeomVecFull layout (32 bytes). */
struct SVec
{
    float x, y, z;
    unsigned char nz, ny, nx, nw;      /* SCompactVector: z,y,x,w */
    short u, v;
    short lu, lv;
    unsigned char t0[ 4 ], t1[ 4 ];
};

}  // namespace

void RunD3DSelfTest( void *pReporter, ReportFn pfnAdd )
{
    SReporter R = { pReporter, pfnAdd };
    R( BOOT_HEADING, "d3d9gles: the renderer's Direct3D 9 on this GPU" );

    A5D3DPlatformHooks hooks = { HookW, HookH, HookPresent, HookAlive };
    A5D3DSetPlatformHooks( &hooks );

    IDirect3D9 *pD3D = Direct3DCreate9( D3D_SDK_VERSION );
    D3DCAPS9 caps;
    pD3D->GetDeviceCaps( 0, D3DDEVTYPE_HAL, &caps );
    R( BOOT_OK, "IDirect3D9 created; caps: vs %d.%d ps %d.%d, %d modes",
       ( caps.VertexShaderVersion >> 8 ) & 0xff, caps.VertexShaderVersion & 0xff,
       ( caps.PixelShaderVersion >> 8 ) & 0xff, caps.PixelShaderVersion & 0xff,
       (int)pD3D->GetAdapterModeCount( 0, D3DFMT_X8R8G8B8 ) );

    D3DPRESENT_PARAMETERS pp;
    memset( &pp, 0, sizeof( pp ) );
    pp.BackBufferWidth = 256; pp.BackBufferHeight = 256;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9 *pDev = 0;
    if ( pD3D->CreateDevice( 0, D3DDEVTYPE_HAL, 0, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &pDev ) != D3D_OK || !pDev )
    {
        R( BOOT_FAIL, "CreateDevice failed" );
        pD3D->Release();
        return;
    }
    R( BOOT_OK, "device with a 256x256 virtual back buffer" );

    /* ---- every shader from the engine's bytecode ------------------------ */
    int nVSok = 0, nPSok = 0;
    std::vector< IDirect3DVertexShader9 * > vs( 77, (IDirect3DVertexShader9 *)0 );
    std::vector< IDirect3DPixelShader9 * > ps( 78, (IDirect3DPixelShader9 *)0 );
    for ( int i = 0; i < 77; ++i )
        if ( pDev->CreateVertexShader( vsAllShaders[ i ]->pShader, &vs[ i ] ) == D3D_OK ) ++nVSok;
    for ( int i = 0; i < 78; ++i )
        if ( pDev->CreatePixelShader( psAllShaders[ i ]->pShader14, &ps[ i ] ) == D3D_OK ) ++nPSok;
    R( nVSok == 77 && nPSok == 78 ? BOOT_OK : BOOT_FAIL,
       "bytecode -> GLSL lookup: %d/77 vertex, %d/78 pixel shaders found", nVSok, nPSok );

    /* Link every pixel shader against vsPureGeometry and every vertex shader
     * against psDiffuse: 155 GL programs compiled by this GPU's compiler.  A
     * shader the driver rejects shows up here, with its log in logcat. */
    IDirect3DVertexDeclaration9 *pDecl = 0;
    D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT3,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
        { 0, 16, D3DDECLTYPE_SHORT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 20, D3DDECLTYPE_SHORT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        { 0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0 },
        { 0, 28, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 1 },
        D3DDECL_END()
    };
    pDev->CreateVertexDeclaration( decl, &pDecl );
    pDev->SetVertexDeclaration( pDecl );

    /* one triangle covering the lower-left half of clip space, identity projection in c10..c13 */
    IDirect3DVertexBuffer9 *pVB = 0;
    pDev->CreateVertexBuffer( 3 * sizeof( SVec ), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &pVB, 0 );
    {
        void *p = 0;
        pVB->Lock( 0, 3 * sizeof( SVec ), &p, 0 );
        SVec *v = (SVec *)p;
        memset( v, 0, 3 * sizeof( SVec ) );
        v[ 0 ].x = -1; v[ 0 ].y = -1; v[ 1 ].x = 1; v[ 1 ].y = -1; v[ 2 ].x = -1; v[ 2 ].y = 1;
        for ( int i = 0; i < 3; ++i ) { v[ i ].z = 0.5f; v[ i ].nx = 128; v[ i ].ny = 128; v[ i ].nz = 255; }
        pVB->Unlock();
    }
    IDirect3DIndexBuffer9 *pIB = 0;
    pDev->CreateIndexBuffer( 6, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &pIB, 0 );
    {
        void *p = 0;
        pIB->Lock( 0, 6, &p, 0 );
        ( (unsigned short *)p )[ 0 ] = 0; ( (unsigned short *)p )[ 1 ] = 1; ( (unsigned short *)p )[ 2 ] = 2;
        pIB->Unlock();
    }
    pDev->SetStreamSource( 0, pVB, 0, sizeof( SVec ) );
    pDev->SetIndices( pIB );
    float identity[ 16 ] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    pDev->SetVertexShaderConstantF( 10, identity, 4 );
    float c16[ 4 ] = { 0.2f, 0.4f, 0.8f, 1.0f };      /* vsConstLight colour */
    pDev->SetVertexShaderConstantF( 16, c16, 1 );
    pDev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
    pDev->SetRenderState( D3DRS_ZENABLE, FALSE );

    int nLinked = 0, nTotal = 0;
    for ( int i = 0; i < 78; ++i )
    {
        if ( !ps[ i ] || !vs[ 0 ] ) continue;
        pDev->SetVertexShader( vs[ 0 ] ); pDev->SetPixelShader( ps[ i ] );
        pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0, 1, 0 );
        ++nTotal;
        while ( glGetError() != GL_NO_ERROR ) {}
        pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
        if ( glGetError() == GL_NO_ERROR ) ++nLinked;
    }
    for ( int i = 1; i < 77; ++i )
    {
        if ( !vs[ i ] || !ps[ 0 ] ) continue;
        pDev->SetVertexShader( vs[ i ] ); pDev->SetPixelShader( ps[ 0 ] );
        ++nTotal;
        while ( glGetError() != GL_NO_ERROR ) {}
        pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
        if ( glGetError() == GL_NO_ERROR ) ++nLinked;
    }
    R( nLinked == nTotal ? BOOT_OK : BOOT_FAIL, "%d/%d shader programs compiled and drew without GL errors", nLinked, nTotal );

    /* ---- pixels: vsConstLight + psDiffuse writes c16 into the covered half ---- */
    IDirect3DVertexShader9 *pVSConst = vs[ 13 - 1 ];   /* vsConstLight id 13 */
    pDev->SetVertexShader( pVSConst ); pDev->SetPixelShader( ps[ 0 ] );
    pDev->Clear( 0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1, 0 );
    pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );

    IDirect3DSurface9 *pShot = 0;
    pDev->CreateOffscreenPlainSurface( 256, 256, D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &pShot, 0 );
    pDev->GetFrontBufferData( 0, pShot );
    D3DLOCKED_RECT lr;
    pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
    /* D3D layout: row 0 = top.  The triangle covers clip-space lower-left ->
     * screen bottom-left (D3D y down).  So (10, 245) is inside, (245, 10) is not. */
    const unsigned char *pIn  = (const unsigned char *)lr.pBits + 245 * lr.Pitch + 10 * 4;
    const unsigned char *pOut = (const unsigned char *)lr.pBits + 10 * lr.Pitch + 245 * 4;
    const int bIn  = pIn[ 2 ], gIn = pIn[ 1 ], rIn = pIn[ 2 ];   /* memory b,g,r,a */
    (void)bIn; (void)gIn;
    const bool bInside  = pIn[ 2 ] > 40 && pIn[ 2 ] < 65 && pIn[ 1 ] > 90 && pIn[ 1 ] < 115 && pIn[ 0 ] > 190;
    const bool bOutside = pOut[ 0 ] == 0 && pOut[ 1 ] == 0 && pOut[ 2 ] == 0;
    R( bInside && bOutside ? BOOT_OK : BOOT_FAIL,
       "draw + read back: inside bgra(%d,%d,%d,%d) expect ~(204,102,51), outside bgra(%d,%d,%d) expect 0 -- D3D row order %s",
       pIn[ 0 ], pIn[ 1 ], pIn[ 2 ], pIn[ 3 ], pOut[ 0 ], pOut[ 1 ], pOut[ 2 ], bInside && bOutside ? "confirmed" : "WRONG" );
    (void)rIn;
    pShot->UnlockRect();

    /* ---- render target texture: draw into it, sample it back through psTextureCopyAlpha ---- */
    IDirect3DTexture9 *pRTTex = 0;
    pDev->CreateTexture( 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &pRTTex, 0 );
    IDirect3DSurface9 *pRTSurf = 0, *pBack = 0, *pBackZ = 0, *pRTZ = 0;
    pRTTex->GetSurfaceLevel( 0, &pRTSurf );
    pDev->GetRenderTarget( 0, &pBack );
    pDev->GetDepthStencilSurface( &pBackZ );
    pDev->CreateDepthStencilSurface( 64, 64, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &pRTZ, 0 );
    pDev->SetRenderTarget( 0, pRTSurf );
    pDev->SetDepthStencilSurface( pRTZ );
    pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0xFF00FF00, 1, 0 );   /* green */
    pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );   /* c16 colour, lower-left */
    pDev->SetRenderTarget( 0, pBack );
    pDev->SetDepthStencilSurface( pBackZ );
    D3DLOCKED_RECT rl;
    if ( pRTSurf->LockRect( &rl, 0, D3DLOCK_READONLY ) == D3D_OK )
    {
        const unsigned char *pTop = (const unsigned char *)rl.pBits + 2 * rl.Pitch + 60 * 4;      /* top-right: green */
        const unsigned char *pBot = (const unsigned char *)rl.pBits + 61 * rl.Pitch + 2 * 4;      /* bottom-left: c16 */
        const bool bOK = pTop[ 1 ] > 200 && pTop[ 2 ] < 30 && pBot[ 0 ] > 190 && pBot[ 2 ] < 70;
        R( bOK ? BOOT_OK : BOOT_FAIL, "render-to-texture + LockRect: top-right bgr(%d,%d,%d) green, bottom-left bgr(%d,%d,%d) c16",
           pTop[ 0 ], pTop[ 1 ], pTop[ 2 ], pBot[ 0 ], pBot[ 1 ], pBot[ 2 ] );
        pRTSurf->UnlockRect();
    }
    else
        R( BOOT_FAIL, "render-target LockRect failed" );

    /* ---- managed texture upload + sampling ---- */
    IDirect3DTexture9 *pTex = 0;
    pDev->CreateTexture( 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &pTex, 0 );
    {
        D3DLOCKED_RECT tl;
        pTex->LockRect( 0, &tl, 0, 0 );
        for ( int y = 0; y < 4; ++y )
            for ( int x = 0; x < 4; ++x )
            {
                unsigned char *p = (unsigned char *)tl.pBits + y * tl.Pitch + x * 4;
                p[ 0 ] = 255; p[ 1 ] = 128; p[ 2 ] = 0; p[ 3 ] = 255;    /* b,g,r,a = pure blue-ish (0,128,255) */
            }
        pTex->UnlockRect( 0 );
    }
    pDev->SetTexture( 0, pTex );
    pDev->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
    pDev->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
    pDev->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
    pDev->SetVertexShader( vs[ 4 - 1 ] );        /* vsTexture (id 4): oT0 = tex * c6.x */
    pDev->SetPixelShader( ps[ 3 - 1 ] );         /* psTextureCopyAlpha (id 3): r0 = t0 */
    float c6[ 4 ] = { 1.0f / 2048, 1.0f / 65536, 0.5f, 0 };
    pDev->SetVertexShaderConstantF( 6, c6, 1 );
    pDev->Clear( 0, 0, D3DCLEAR_TARGET, 0xFF000000, 1, 0 );
    pDev->DrawIndexedPrimitive( D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1 );
    pDev->GetFrontBufferData( 0, pShot );
    pShot->LockRect( &lr, 0, D3DLOCK_READONLY );
    {
        const unsigned char *p = (const unsigned char *)lr.pBits + 245 * lr.Pitch + 10 * 4;
        const bool bOK = p[ 0 ] > 240 && p[ 1 ] > 118 && p[ 1 ] < 138 && p[ 2 ] < 15;
        R( bOK ? BOOT_OK : BOOT_FAIL, "managed texture upload + vsTexture/psTextureCopyAlpha: bgr(%d,%d,%d) expect (255,128,0)",
           p[ 0 ], p[ 1 ], p[ 2 ] );
    }
    pShot->UnlockRect();

    /* ---- teardown ---- */
    pTex->Release(); pRTSurf->Release(); pRTTex->Release(); pRTZ->Release(); pBack->Release(); pBackZ->Release();
    pShot->Release(); pVB->Release(); pIB->Release(); pDecl->Release();
    for ( size_t i = 0; i < vs.size(); ++i ) if ( vs[ i ] ) vs[ i ]->Release();
    for ( size_t i = 0; i < ps.size(); ++i ) if ( ps[ i ] ) ps[ i ]->Release();
    pDev->Release();
    pD3D->Release();
    R( BOOT_OK, "device torn down cleanly" );
}
