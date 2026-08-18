/*
 *  d3d9gles.cpp -- the Direct3D 9 subset the engine uses, on OpenGL ES 3.0.
 *  See d3d9.h for what this is and why.
 *
 *  Layout of this file:
 *    1. GL helpers, capabilities, format tables
 *    2. Resources: surfaces, textures, cube textures, buffers, shaders, decls
 *    3. The device: state, framebuffer cache, program cache, draw path
 *    4. IDirect3D9 (adapter queries, device creation)
 *    5. Platform hooks
 */
#include "d3d9.h"
#include "a5_log.h"
#include "a5_glsl_table.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

/* The software DXT decoder lives in platform/, next to the boot harness. */
bool   DxtDecode( int nDxtVersion, const uint8_t *pIn, size_t nInSize, int nWidth, int nHeight, uint8_t *pOut );
size_t DxtLevelSize( int nDxtVersion, int nWidth, int nHeight );

static A5D3DFrameStats g_stats;
#define D3DGL_LOG( ... )  a5_log( A5_PRIORITY_INFO,  __VA_ARGS__ )
#define D3DGL_WARN( ... ) a5_log( A5_PRIORITY_WARN,  __VA_ARGS__ )
#define D3DGL_ERR( ... )  a5_log( A5_PRIORITY_ERROR, __VA_ARGS__ )

/* Assertions here are about the shim's own contract with the engine: an
 * unhandled format or call.  They log and continue rather than abort, so a
 * missing corner shows up in logcat instead of killing the app. */
#define D3DGL_ASSERT( x ) do { if ( !( x ) ) D3DGL_ERR( "d3d9gles: assertion failed: %s (%s:%d)", #x, __FILE__, __LINE__ ); } while ( 0 )

#ifndef GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif

namespace {

/* ========================================================================== */
/*  1. GL helpers, capabilities, formats                                       */
/* ========================================================================== */

struct SGLCaps
{
    bool bS3TC;
    bool bAnisotropy;
    int  nMaxTextureSize;
    bool bChecked;
    SGLCaps() : bS3TC( false ), bAnisotropy( false ), nMaxTextureSize( 2048 ), bChecked( false ) {}
};
SGLCaps g_caps;

void CheckGLCaps()
{
    if ( g_caps.bChecked )
        return;
    g_caps.bChecked = true;
    const char *pszExt = (const char *)glGetString( GL_EXTENSIONS );
    if ( pszExt )
    {
        g_caps.bS3TC       = strstr( pszExt, "GL_EXT_texture_compression_s3tc" ) != 0;
        g_caps.bAnisotropy = strstr( pszExt, "GL_EXT_texture_filter_anisotropic" ) != 0;
    }
    glGetIntegerv( GL_MAX_TEXTURE_SIZE, &g_caps.nMaxTextureSize );
    D3DGL_LOG( "d3d9gles: %s / %s; S3TC=%d anisotropic=%d maxtex=%d",
               (const char *)glGetString( GL_RENDERER ), (const char *)glGetString( GL_VERSION ),
               (int)g_caps.bS3TC, (int)g_caps.bAnisotropy, g_caps.nMaxTextureSize );
}

void CheckGLError( const char *pszWhere )
{
    GLenum e = glGetError();
    if ( e != GL_NO_ERROR )
        D3DGL_WARN( "d3d9gles: GL error 0x%04x at %s", e, pszWhere );
}

/* ---- format description --------------------------------------------------- */
enum EFormatClass { FC_COLOR, FC_DXT, FC_DEPTH, FC_INDEX, FC_UNKNOWN };

struct SFormatInfo
{
    D3DFORMAT    d3d;
    EFormatClass cls;
    int          nBytesPerPixel;    /* for FC_COLOR */
    int          nDxt;              /* 1/3/5 for FC_DXT (DXT2->3, DXT4->5) */
    GLenum       internalFormat;    /* what the GL texture is stored as */
    GLenum       glFormat, glType;  /* upload format/type for FC_COLOR */
};

SFormatInfo DescribeFormat( D3DFORMAT f )
{
    SFormatInfo i;
    memset( &i, 0, sizeof( i ) );
    i.d3d = f;
    i.cls = FC_UNKNOWN;
    switch ( f )
    {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            i.cls = FC_COLOR; i.nBytesPerPixel = 4; i.internalFormat = GL_RGBA8;
            i.glFormat = GL_RGBA; i.glType = GL_UNSIGNED_BYTE;
            break;
        case D3DFMT_R5G6B5:
            i.cls = FC_COLOR; i.nBytesPerPixel = 2; i.internalFormat = GL_RGB565;
            i.glFormat = GL_RGB; i.glType = GL_UNSIGNED_SHORT_5_6_5;
            break;
        case D3DFMT_A1R5G5B5:
        case D3DFMT_X1R5G5B5:
            i.cls = FC_COLOR; i.nBytesPerPixel = 2; i.internalFormat = GL_RGB5_A1;
            i.glFormat = GL_RGBA; i.glType = GL_UNSIGNED_SHORT_5_5_5_1;
            break;
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
            i.cls = FC_COLOR; i.nBytesPerPixel = 2; i.internalFormat = GL_RGBA4;
            i.glFormat = GL_RGBA; i.glType = GL_UNSIGNED_SHORT_4_4_4_4;
            break;
        case D3DFMT_DXT1: i.cls = FC_DXT; i.nDxt = 1; break;
        case D3DFMT_DXT2:
        case D3DFMT_DXT3: i.cls = FC_DXT; i.nDxt = 3; break;
        case D3DFMT_DXT4:
        case D3DFMT_DXT5: i.cls = FC_DXT; i.nDxt = 5; break;
        case D3DFMT_D24S8:
        case D3DFMT_D24X4S4:
            i.cls = FC_DEPTH; i.internalFormat = GL_DEPTH24_STENCIL8; break;
        case D3DFMT_D24X8:
        case D3DFMT_D32:
            i.cls = FC_DEPTH; i.internalFormat = GL_DEPTH_COMPONENT24; break;
        case D3DFMT_D16:
        case D3DFMT_D16_LOCKABLE:
            i.cls = FC_DEPTH; i.internalFormat = GL_DEPTH_COMPONENT16; break;
        case D3DFMT_INDEX16: i.cls = FC_INDEX; i.nBytesPerPixel = 2; break;
        case D3DFMT_INDEX32: i.cls = FC_INDEX; i.nBytesPerPixel = 4; break;
        default: break;
    }
    if ( i.cls == FC_DXT )
    {
        i.internalFormat = i.nDxt == 1 ? GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                         : i.nDxt == 3 ? GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                                       : GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    }
    return i;
}

/* Bytes in one row of a level, in the D3D layout the engine reads and writes. */
int LevelPitch( const SFormatInfo &fi, int nWidth )
{
    if ( fi.cls == FC_DXT )
        return ( ( nWidth + 3 ) / 4 ) * ( fi.nDxt == 1 ? 8 : 16 );
    return nWidth * fi.nBytesPerPixel;
}
int LevelRows( const SFormatInfo &fi, int nHeight )
{
    if ( fi.cls == FC_DXT )
        return ( nHeight + 3 ) / 4;
    return nHeight;
}
int LevelBytes( const SFormatInfo &fi, int nWidth, int nHeight )
{
    return LevelPitch( fi, nWidth ) * LevelRows( fi, nHeight );
}

/* ---- pixel conversions, D3D memory layout -> what GL takes ---------------- */
void ConvertRowToGL( const SFormatInfo &fi, const uint8_t *pSrc, uint8_t *pDst, int nPixels )
{
    switch ( fi.d3d )
    {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            /* memory b,g,r,a -> r,g,b,a */
            for ( int x = 0; x < nPixels; ++x )
            {
                pDst[ 0 ] = pSrc[ 2 ]; pDst[ 1 ] = pSrc[ 1 ]; pDst[ 2 ] = pSrc[ 0 ];
                pDst[ 3 ] = fi.d3d == D3DFMT_X8R8G8B8 ? 255 : pSrc[ 3 ];
                pSrc += 4; pDst += 4;
            }
            break;
        case D3DFMT_R5G6B5:
            memcpy( pDst, pSrc, (size_t)nPixels * 2 );
            break;
        case D3DFMT_A1R5G5B5:
        case D3DFMT_X1R5G5B5:
        {
            /* a15 r14..10 g9..5 b4..0  ->  r15..11 g10..6 b5..1 a0 */
            const uint16_t *s = (const uint16_t *)pSrc;
            uint16_t *d = (uint16_t *)pDst;
            for ( int x = 0; x < nPixels; ++x )
            {
                uint16_t v = s[ x ];
                uint16_t a = fi.d3d == D3DFMT_X1R5G5B5 ? 1 : ( ( v >> 15 ) & 1 );
                d[ x ] = (uint16_t)( ( ( v & 0x7FFF ) << 1 ) | a );
            }
            break;
        }
        case D3DFMT_A4R4G4B4:
        case D3DFMT_X4R4G4B4:
        {
            /* a15..12 r11..8 g7..4 b3..0 -> r15..12 g11..8 b7..4 a3..0 */
            const uint16_t *s = (const uint16_t *)pSrc;
            uint16_t *d = (uint16_t *)pDst;
            for ( int x = 0; x < nPixels; ++x )
            {
                uint16_t v = s[ x ];
                uint16_t a = fi.d3d == D3DFMT_X4R4G4B4 ? 0xF : ( ( v >> 12 ) & 0xF );
                d[ x ] = (uint16_t)( ( ( v & 0x0FFF ) << 4 ) | a );
            }
            break;
        }
        default:
            memcpy( pDst, pSrc, (size_t)nPixels * fi.nBytesPerPixel );
            break;
    }
}

/* RGBA8 (as read back from GL) -> D3D A8R8G8B8 memory. */
void ConvertRowFromRgba8( uint8_t *pDst, const uint8_t *pSrc, int nPixels )
{
    for ( int x = 0; x < nPixels; ++x )
    {
        pDst[ 0 ] = pSrc[ 2 ]; pDst[ 1 ] = pSrc[ 1 ]; pDst[ 2 ] = pSrc[ 0 ]; pDst[ 3 ] = pSrc[ 3 ];
        pSrc += 4; pDst += 4;
    }
}

int MipCount( int w, int h )
{
    int n = 1;
    while ( w > 1 || h > 1 )
    {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        ++n;
    }
    return n;
}

}  // namespace

/* ========================================================================== */
/*  2. Resources                                                               */
/* ========================================================================== */
namespace {

class CDevice;
CDevice *g_pDevice = 0;

/*  A texture image: one GL texture object (2D or cube) with, for everything
 *  that is not a render target, a CPU shadow of every level in D3D memory
 *  layout.  The shadow is what LockRect hands out; UnlockRect uploads the
 *  locked rectangle.  Render targets have no shadow -- their content lives on
 *  the GPU and read-locks go through glReadPixels.
 *
 *  System-memory textures (D3DPOOL_SYSTEMMEM / SCRATCH) and offscreen plain
 *  surfaces are shadow only, no GL object: the engine uses them as staging
 *  buffers for UpdateSurface and screenshots. */
struct CTexImage
{
    GLuint      nGL;
    GLenum      target;           /* GL_TEXTURE_2D or GL_TEXTURE_CUBE_MAP */
    SFormatInfo fi;
    int         nWidth, nHeight, nLevels, nFaces;
    bool        bRenderTarget;
    bool        bSysMem;
    bool        bDecodeDxt;       /* no S3TC on this GPU: stored as RGBA8 */
    std::vector< std::vector< uint8_t > > shadow;   /* [face * nLevels + level] */

    CTexImage() : nGL( 0 ), target( GL_TEXTURE_2D ), nWidth( 0 ), nHeight( 0 ), nLevels( 1 ), nFaces( 1 ),
                  bRenderTarget( false ), bSysMem( false ), bDecodeDxt( false ) {}
    ~CTexImage() { if ( nGL ) glDeleteTextures( 1, &nGL ); }

    int  LevelW( int nLevel ) const { int w = nWidth  >> nLevel; return w < 1 ? 1 : w; }
    int  LevelH( int nLevel ) const { int h = nHeight >> nLevel; return h < 1 ? 1 : h; }
    std::vector< uint8_t > &Shadow( int nFace, int nLevel ) { return shadow[ nFace * nLevels + nLevel ]; }

    bool Create( GLenum _target, int w, int h, int levels, D3DFORMAT format, bool bRT, bool bSys )
    {
        target = _target;
        fi = DescribeFormat( format );
        if ( fi.cls != FC_COLOR && fi.cls != FC_DXT )
        {
            D3DGL_ERR( "d3d9gles: unsupported texture format 0x%x", (unsigned)format );
            return false;
        }
        nWidth = w; nHeight = h;
        nLevels = levels > 0 ? levels : MipCount( w, h );
        nFaces = target == GL_TEXTURE_CUBE_MAP ? 6 : 1;
        bRenderTarget = bRT;
        bSysMem = bSys;
        bDecodeDxt = ( fi.cls == FC_DXT ) && !g_caps.bS3TC;

        if ( !bRenderTarget )
        {
            shadow.resize( (size_t)nFaces * nLevels );
            for ( int f = 0; f < nFaces; ++f )
                for ( int l = 0; l < nLevels; ++l )
                    Shadow( f, l ).assign( (size_t)LevelBytes( fi, LevelW( l ), LevelH( l ) ), 0 );
        }
        if ( bSysMem )
            return true;

        glGenTextures( 1, &nGL );
        glBindTexture( target, nGL );
        GLenum internal = bDecodeDxt ? GL_RGBA8 : fi.internalFormat;
        glTexStorage2D( target, nLevels, internal, w, h );
        glTexParameteri( target, GL_TEXTURE_BASE_LEVEL, 0 );
        glTexParameteri( target, GL_TEXTURE_MAX_LEVEL, nLevels - 1 );
        CheckGLError( "CTexImage::Create" );
        return true;
    }

    GLenum FaceTarget( int nFace ) const
    {
        return target == GL_TEXTURE_CUBE_MAP ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + nFace : GL_TEXTURE_2D;
    }

    /* Upload a rectangle (in texels, level coordinates) of a level from D3D-layout memory. */
    void Upload( int nFace, int nLevel, int x, int y, int w, int h, const uint8_t *pSrc, int nSrcPitch )
    {
        if ( bSysMem || !nGL )
            return;
        glBindTexture( target, nGL );
        glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
        const GLenum face = FaceTarget( nFace );

        if ( fi.cls == FC_DXT )
        {
            /* DXT rectangles are whole blocks; the engine only ever locks whole levels. */
            const int bw = ( w + 3 ) / 4, bh = ( h + 3 ) / 4;
            const int nBlockBytes = fi.nDxt == 1 ? 8 : 16;
            if ( !bDecodeDxt )
            {
                std::vector< uint8_t > packed( (size_t)bw * bh * nBlockBytes );
                for ( int by = 0; by < bh; ++by )
                    memcpy( &packed[ (size_t)by * bw * nBlockBytes ], pSrc + (size_t)by * nSrcPitch, (size_t)bw * nBlockBytes );
                glCompressedTexSubImage2D( face, nLevel, x, y, w, h, fi.internalFormat,
                                           (GLsizei)packed.size(), &packed[ 0 ] );
            }
            else
            {
                std::vector< uint8_t > packed( (size_t)bw * bh * nBlockBytes );
                for ( int by = 0; by < bh; ++by )
                    memcpy( &packed[ (size_t)by * bw * nBlockBytes ], pSrc + (size_t)by * nSrcPitch, (size_t)bw * nBlockBytes );
                std::vector< uint8_t > rgba( (size_t)w * h * 4 );
                DxtDecode( fi.nDxt, &packed[ 0 ], packed.size(), w, h, &rgba[ 0 ] );
                glTexSubImage2D( face, nLevel, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[ 0 ] );
            }
        }
        else
        {
            std::vector< uint8_t > converted( (size_t)w * h * fi.nBytesPerPixel );
            for ( int row = 0; row < h; ++row )
                ConvertRowToGL( fi, pSrc + (size_t)row * nSrcPitch,
                                &converted[ (size_t)row * w * fi.nBytesPerPixel ], w );
            glTexSubImage2D( face, nLevel, x, y, w, h, fi.glFormat, fi.glType, &converted[ 0 ] );
        }
        CheckGLError( "CTexImage::Upload" );
    }
};

/* Forward: the device does FBO work for surfaces (readback). */
void DeviceReadbackTexture( CTexImage *pImage, int nFace, int nLevel, int x, int y, int w, int h, uint8_t *pDst, int nDstPitch );

/*  Surface kinds. */
enum ESurfaceKind { SK_TEXTURE, SK_RENDERBUFFER, SK_OFFSCREEN };

class CSurface : public IDirect3DSurface9
{
public:
    ESurfaceKind kind;
    /* SK_TEXTURE */
    IDirect3DUnknown9 *pOwner;      /* the texture object, kept alive */
    CTexImage         *pImage;
    int                nFace, nLevel;
    /* SK_RENDERBUFFER */
    GLuint             nRenderbuffer;
    SFormatInfo        fi;
    int                nWidth, nHeight;
    /* SK_OFFSCREEN */
    std::vector< uint8_t > pixels;
    /* lock state */
    bool                   bLocked;
    RECT                   lockRect;
    bool                   bLockReadOnly;
    std::vector< uint8_t > lockScratch;   /* for render-target locks */

    CSurface() : kind( SK_OFFSCREEN ), pOwner( 0 ), pImage( 0 ), nFace( 0 ), nLevel( 0 ),
                 nRenderbuffer( 0 ), nWidth( 0 ), nHeight( 0 ), bLocked( false ), bLockReadOnly( false )
    { memset( &fi, 0, sizeof( fi ) ); }
    ~CSurface();

    int Width() const  { return kind == SK_TEXTURE ? pImage->LevelW( nLevel ) : nWidth; }
    int Height() const { return kind == SK_TEXTURE ? pImage->LevelH( nLevel ) : nHeight; }
    const SFormatInfo &Format() const { return kind == SK_TEXTURE ? pImage->fi : fi; }

    virtual HRESULT LockRect( D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags )
    {
        if ( bLocked )
        {
            D3DGL_WARN( "d3d9gles: LockRect on an already locked surface (%dx%d, kind %d)", Width(), Height(), (int)kind );
            return D3DERR_INVALIDCALL;
        }
        const SFormatInfo &f = Format();
        if ( pRect )
            lockRect = *pRect;
        else
        {
            lockRect.left = 0; lockRect.top = 0; lockRect.right = Width(); lockRect.bottom = Height();
        }
        bLockReadOnly = ( Flags & D3DLOCK_READONLY ) != 0;
        bLocked = true;

        const int nPitch = LevelPitch( f, Width() );
        if ( kind == SK_OFFSCREEN )
        {
            pLockedRect->Pitch = nPitch;
            pLockedRect->pBits = &pixels[ 0 ] + (size_t)lockRect.top * nPitch + (size_t)lockRect.left * f.nBytesPerPixel;
            return D3D_OK;
        }
        if ( kind != SK_TEXTURE )
        {
            D3DGL_WARN( "d3d9gles: LockRect on a surface of kind %d", (int)kind );
            return D3DERR_INVALIDCALL;
        }

        if ( !pImage->bRenderTarget )
        {
            std::vector< uint8_t > &s = pImage->Shadow( nFace, nLevel );
            pLockedRect->Pitch = nPitch;
            if ( f.cls == FC_DXT )
                pLockedRect->pBits = &s[ 0 ] + (size_t)( lockRect.top / 4 ) * nPitch + (size_t)( lockRect.left / 4 ) * ( f.nDxt == 1 ? 8 : 16 );
            else
                pLockedRect->pBits = &s[ 0 ] + (size_t)lockRect.top * nPitch + (size_t)lockRect.left * f.nBytesPerPixel;
            return D3D_OK;
        }

        /* Render target: read the rectangle back into scratch memory. */
        const int w = lockRect.right - lockRect.left, h = lockRect.bottom - lockRect.top;
        const int nScratchPitch = w * f.nBytesPerPixel;
        lockScratch.assign( (size_t)nScratchPitch * h, 0 );
        DeviceReadbackTexture( pImage, nFace, nLevel, lockRect.left, lockRect.top, w, h, &lockScratch[ 0 ], nScratchPitch );
        pLockedRect->Pitch = nScratchPitch;
        pLockedRect->pBits = &lockScratch[ 0 ];
        return D3D_OK;
    }

    virtual HRESULT UnlockRect()
    {
        if ( !bLocked )
            return D3DERR_INVALIDCALL;
        bLocked = false;
        if ( kind != SK_TEXTURE || bLockReadOnly )
            return D3D_OK;
        const SFormatInfo &f = pImage->fi;
        const int w = lockRect.right - lockRect.left, h = lockRect.bottom - lockRect.top;
        if ( !pImage->bRenderTarget )
        {
            std::vector< uint8_t > &s = pImage->Shadow( nFace, nLevel );
            const int nPitch = LevelPitch( f, Width() );
            const uint8_t *pSrc;
            if ( f.cls == FC_DXT )
                pSrc = &s[ 0 ] + (size_t)( lockRect.top / 4 ) * nPitch + (size_t)( lockRect.left / 4 ) * ( f.nDxt == 1 ? 8 : 16 );
            else
                pSrc = &s[ 0 ] + (size_t)lockRect.top * nPitch + (size_t)lockRect.left * f.nBytesPerPixel;
            pImage->Upload( nFace, nLevel, lockRect.left, lockRect.top, w, h, pSrc, nPitch );
        }
        else
            pImage->Upload( nFace, nLevel, lockRect.left, lockRect.top, w, h, &lockScratch[ 0 ], w * f.nBytesPerPixel );
        return D3D_OK;
    }

    virtual HRESULT GetDesc( D3DSURFACE_DESC *pDesc )
    {
        memset( pDesc, 0, sizeof( *pDesc ) );
        pDesc->Format = Format().d3d;
        pDesc->Type = D3DRTYPE_SURFACE;
        pDesc->Width = Width();
        pDesc->Height = Height();
        pDesc->Usage = ( kind == SK_TEXTURE && pImage->bRenderTarget ) ? D3DUSAGE_RENDERTARGET
                     : ( kind == SK_RENDERBUFFER ? D3DUSAGE_DEPTHSTENCIL : 0 );
        return D3D_OK;
    }
};

void DeviceForgetSurface( CSurface *pSurface );   /* purge FBO cache entries */

CSurface::~CSurface()
{
    DeviceForgetSurface( this );
    if ( nRenderbuffer )
        glDeleteRenderbuffers( 1, &nRenderbuffer );
    if ( pOwner )
        pOwner->Release();
}

/*  2D texture. */
class CTexture2D : public IDirect3DTexture9
{
public:
    CTexImage image;
    std::vector< CSurface * > surfaces;    /* one per level, created lazily; we hold one ref */

    ~CTexture2D()
    {
        for ( size_t i = 0; i < surfaces.size(); ++i )
            if ( surfaces[ i ] )
            {
                surfaces[ i ]->pOwner = 0;     /* we are going away; do not Release us again */
                surfaces[ i ]->Release();
            }
    }
    virtual D3DRESOURCETYPE GetType() { return D3DRTYPE_TEXTURE; }
    virtual HRESULT GetSurfaceLevel( UINT Level, IDirect3DSurface9 **ppSurfaceLevel )
    {
        if ( (int)Level >= image.nLevels )
            return D3DERR_INVALIDCALL;
        if ( surfaces.size() < (size_t)image.nLevels )
            surfaces.resize( image.nLevels, 0 );
        if ( !surfaces[ Level ] )
        {
            CSurface *p = new CSurface;
            p->kind = SK_TEXTURE;
            p->pOwner = this; AddRef();
            p->pImage = &image;
            p->nFace = 0;
            p->nLevel = Level;
            surfaces[ Level ] = p;          /* holds the creation ref */
        }
        surfaces[ Level ]->AddRef();
        *ppSurfaceLevel = surfaces[ Level ];
        return D3D_OK;
    }
    virtual HRESULT LockRect( UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags )
    {
        IDirect3DSurface9 *pS = 0;
        if ( GetSurfaceLevel( Level, &pS ) != D3D_OK )
            return D3DERR_INVALIDCALL;
        HRESULT hr = pS->LockRect( pLockedRect, pRect, Flags );
        pS->Release();
        return hr;
    }
    virtual HRESULT UnlockRect( UINT Level )
    {
        IDirect3DSurface9 *pS = 0;
        if ( GetSurfaceLevel( Level, &pS ) != D3D_OK )
            return D3DERR_INVALIDCALL;
        HRESULT hr = pS->UnlockRect();
        pS->Release();
        return hr;
    }
};

/*  Cube texture.  Face order is D3D's: +X -X +Y -Y +Z -Z, which is also GL's. */
class CCubeTexture : public IDirect3DCubeTexture9
{
public:
    CTexImage image;
    std::vector< CSurface * > surfaces;    /* [face * levels + level] */

    ~CCubeTexture()
    {
        for ( size_t i = 0; i < surfaces.size(); ++i )
            if ( surfaces[ i ] )
            {
                surfaces[ i ]->pOwner = 0;
                surfaces[ i ]->Release();
            }
    }
    virtual D3DRESOURCETYPE GetType() { return D3DRTYPE_CUBETEXTURE; }
    virtual HRESULT GetCubeMapSurface( D3DCUBEMAP_FACES Face, UINT Level, IDirect3DSurface9 **ppSurface )
    {
        if ( (int)Level >= image.nLevels || (int)Face > 5 )
            return D3DERR_INVALIDCALL;
        if ( surfaces.size() < (size_t)6 * image.nLevels )
            surfaces.resize( (size_t)6 * image.nLevels, 0 );
        const size_t idx = (size_t)Face * image.nLevels + Level;
        if ( !surfaces[ idx ] )
        {
            CSurface *p = new CSurface;
            p->kind = SK_TEXTURE;
            p->pOwner = this; AddRef();
            p->pImage = &image;
            p->nFace = (int)Face;
            p->nLevel = Level;
            surfaces[ idx ] = p;
        }
        surfaces[ idx ]->AddRef();
        *ppSurface = surfaces[ idx ];
        return D3D_OK;
    }
};

/*  Vertex / index buffers: a GL buffer plus a full CPU shadow.  The engine
 *  locks whole buffers (offset 0, size 0) and writes sub-ranges it knows about;
 *  MarkDirty() tells the shim which ranges, and Unlock() uploads only those.
 *  Without that hint a lock would upload the whole buffer, which for the 16 MB
 *  static vertex pool would be unusable. */
class CBufferBase
{
public:
    GLuint  nGL;
    GLenum  target;
    UINT    nSize;
    std::vector< uint8_t > shadow;
    std::vector< std::pair< UINT, UINT > > dirty;
    int     nLockCount;
    bool    bDiscardPending;

    CBufferBase() : nGL( 0 ), target( GL_ARRAY_BUFFER ), nSize( 0 ), nLockCount( 0 ), bDiscardPending( false ) {}
    ~CBufferBase() { if ( nGL ) glDeleteBuffers( 1, &nGL ); }

    void Create( GLenum _target, UINT size )
    {
        target = _target;
        nSize = size;
        shadow.assign( size, 0 );
        glGenBuffers( 1, &nGL );
        glBindBuffer( target, nGL );
        glBufferData( target, size, 0, GL_DYNAMIC_DRAW );
        glBindBuffer( target, 0 );
    }
    HRESULT Lock( UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags )
    {
        if ( Flags & D3DLOCK_DISCARD )
            bDiscardPending = true;
        ++nLockCount;
        *ppbData = &shadow[ 0 ] + OffsetToLock;
        /* An explicit sub-range lock is a dirty hint in itself. */
        if ( SizeToLock )
            MarkDirty( OffsetToLock, SizeToLock );
        return D3D_OK;
    }
    void MarkDirty( UINT nOffset, UINT nBytes )
    {
        if ( nBytes == 0 )
            return;
        if ( nOffset + nBytes > nSize )
            nBytes = nSize - nOffset;
        /* merge with the previous range if contiguous or overlapping */
        if ( !dirty.empty() )
        {
            std::pair< UINT, UINT > &last = dirty.back();
            if ( nOffset <= last.first + last.second && nOffset + nBytes >= last.first )
            {
                UINT a = last.first < nOffset ? last.first : nOffset;
                UINT b = ( last.first + last.second ) > ( nOffset + nBytes ) ? ( last.first + last.second ) : ( nOffset + nBytes );
                last.first = a; last.second = b - a;
                return;
            }
        }
        dirty.push_back( std::make_pair( nOffset, nBytes ) );
    }
    HRESULT Unlock();
};

/* Buffer uploads need to know the device's bound buffers; declared below. */
void DeviceUploadBuffer( CBufferBase *pBuffer );

HRESULT CBufferBase::Unlock()
{
    if ( nLockCount > 0 )
        --nLockCount;
    if ( nLockCount == 0 )
        DeviceUploadBuffer( this );
    return D3D_OK;
}

class CVertexBuffer : public IDirect3DVertexBuffer9
{
public:
    CBufferBase buf;
    virtual HRESULT Lock( UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags ) { return buf.Lock( OffsetToLock, SizeToLock, ppbData, Flags ); }
    virtual HRESULT Unlock() { return buf.Unlock(); }
    virtual void MarkDirty( UINT nOffset, UINT nSize ) { buf.MarkDirty( nOffset, nSize ); }
};

class CIndexBuffer : public IDirect3DIndexBuffer9
{
public:
    CBufferBase buf;
    GLenum      indexType;     /* GL_UNSIGNED_SHORT / GL_UNSIGNED_INT */
    int         nIndexSize;
    virtual HRESULT Lock( UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags ) { return buf.Lock( OffsetToLock, SizeToLock, ppbData, Flags ); }
    virtual HRESULT Unlock() { return buf.Unlock(); }
    virtual void MarkDirty( UINT nOffset, UINT nSize ) { buf.MarkDirty( nOffset, nSize ); }
};

/*  Shaders: the GLSL source found in the table by hashing the assembly text
 *  inside the D3D bytecode, compiled per (vertex, pixel, cube-mask) program. */
uint64_t Fnv1a64( const uint8_t *p, size_t n )
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for ( size_t i = 0; i < n; ++i )
    {
        h ^= p[ i ];
        h *= 0x100000001b3ULL;
    }
    return h;
}

const A5GlslEntry *FindShaderEntry( const DWORD *pFunction, bool bVertex )
{
    /* Bytecode ends with 0x0000FFFF; scan up to that for the DBUG text. */
    size_t nDwords = 0;
    while ( pFunction[ nDwords ] != 0x0000FFFF && nDwords < 65536 )
        ++nDwords;
    const uint8_t *pBytes = (const uint8_t *)pFunction;
    const size_t nBytes = ( nDwords + 1 ) * 4;
    static const char *TAGS[] = { "vs.1.1", "ps.1.1", "ps.1.4", "vs_1_1", "ps_1_1", "ps_1_4" };
    const uint8_t *pText = 0;
    for ( size_t t = 0; t < 6 && !pText; ++t )
    {
        const size_t nTag = strlen( TAGS[ t ] );
        for ( size_t i = 0; i + nTag <= nBytes; ++i )
            if ( memcmp( pBytes + i, TAGS[ t ], nTag ) == 0 )
            {
                pText = pBytes + i;
                break;
            }
    }
    if ( !pText )
        return 0;
    size_t nLen = 0;
    while ( pText + nLen < pBytes + nBytes && pText[ nLen ] )
        ++nLen;
    const uint64_t h = Fnv1a64( pText, nLen );
    for ( int i = 0; i < a5GlslTableSize; ++i )
        if ( a5GlslTable[ i ].hash == h && ( a5GlslTable[ i ].isVertex != 0 ) == bVertex )
            return &a5GlslTable[ i ];
    return 0;
}

class CVertexShader : public IDirect3DVertexShader9
{
public:
    const A5GlslEntry *pEntry;
    GLuint nGL;    /* compiled once; programs link it with each pixel shader */
    CVertexShader() : pEntry( 0 ), nGL( 0 ) {}
    ~CVertexShader() { if ( nGL ) glDeleteShader( nGL ); }
};

class CPixelShader : public IDirect3DPixelShader9
{
public:
    const A5GlslEntry *pEntry;
    std::map< int, GLuint > compiled;   /* per cube mask */
    ~CPixelShader()
    {
        for ( std::map< int, GLuint >::iterator i = compiled.begin(); i != compiled.end(); ++i )
            glDeleteShader( i->second );
    }
};

class CVertexDecl : public IDirect3DVertexDeclaration9
{
public:
    std::vector< D3DVERTEXELEMENT9 > elements;
};

class CQuery : public IDirect3DQuery9
{
public:
    virtual HRESULT Issue( DWORD ) { return D3D_OK; }
    virtual HRESULT GetData( void *pData, DWORD dwSize, DWORD )
    {
        if ( dwSize >= sizeof( D3DDEVINFO_VCACHE ) )
        {
            D3DDEVINFO_VCACHE *p = (D3DDEVINFO_VCACHE *)pData;
            p->Pattern = 0x48434143;    /* 'CACH' */
            p->OptMethod = 1;
            p->CacheSize = 24;          /* a modern post-transform cache; the mesh optimiser uses it */
            p->MagicNumber = 20;
        }
        return S_OK;
    }
};

}  // namespace

/* ========================================================================== */
/*  3. The device                                                              */
/* ========================================================================== */
namespace {

A5D3DPlatformHooks g_hooks = { 0, 0, 0, 0 };

/*  Attribute locations by D3D usage -- the convention the generated GLSL
 *  declares (tools/d3dasm2glsl.py: v0..v6). */
int AttribLocation( int nUsage, int nUsageIndex )
{
    switch ( nUsage )
    {
        case D3DDECLUSAGE_POSITION: return 0;
        case D3DDECLUSAGE_NORMAL:   return 1;
        case D3DDECLUSAGE_COLOR:    return 2;
        case D3DDECLUSAGE_TEXCOORD: return nUsageIndex == 0 ? 3 : 6;
        case D3DDECLUSAGE_TANGENT:  return nUsageIndex == 0 ? 4 : 5;
    }
    return -1;
}

struct SProgram
{
    GLuint   nGL;
    /*  Uniform arrays are trimmed by the GLSL compiler to the highest index a
     *  shader actually reads, so a location is looked up per register and
     *  -1 means "this shader does not use it". */
    GLint    locVc[ 96 ], locPc[ 8 ];
    GLint    locPosFixup, locAlphaFunc, locAlphaRef;
    GLint    locSampler[ 8 ];
    unsigned vsGen[ 96 ], psGen[ 8 ];
    int      nLastAlphaFunc;
    float    fLastAlphaRef;
    float    lastPosFixup[ 4 ];
    SProgram() : nGL( 0 ), locPosFixup( -1 ), locAlphaFunc( -1 ), locAlphaRef( -1 ),
                 nLastAlphaFunc( -1 ), fLastAlphaRef( -1 )
    {
        for ( int i = 0; i < 96; ++i ) locVc[ i ] = -1;
        for ( int i = 0; i < 8; ++i ) { locPc[ i ] = -1; locSampler[ i ] = -1; }
        memset( vsGen, 0, sizeof( vsGen ) ); memset( psGen, 0, sizeof( psGen ) );
        lastPosFixup[ 0 ] = lastPosFixup[ 1 ] = lastPosFixup[ 2 ] = lastPosFixup[ 3 ] = -12345.f;
    }
};

struct SProgramKey
{
    CVertexShader *pVS; CPixelShader *pPS; int nCubeMask;
    bool operator<( const SProgramKey &o ) const
    {
        if ( pVS != o.pVS ) return pVS < o.pVS;
        if ( pPS != o.pPS ) return pPS < o.pPS;
        return nCubeMask < o.nCubeMask;
    }
};

struct SRenderStates
{
    DWORD zEnable, zWrite, zFunc;
    DWORD alphaBlend, srcBlend, dstBlend;
    DWORD cull, fill;
    DWORD stencilEnable, stencilFunc, stencilRef, stencilMask, stencilWriteMask, stencilFail, stencilZFail, stencilPass;
    DWORD colorWrite;
    DWORD alphaTest, alphaFunc, alphaRef;
    SRenderStates()
    {
        zEnable = TRUE; zWrite = TRUE; zFunc = D3DCMP_LESSEQUAL;
        alphaBlend = FALSE; srcBlend = D3DBLEND_ONE; dstBlend = D3DBLEND_ZERO;
        cull = D3DCULL_CCW; fill = D3DFILL_SOLID;
        stencilEnable = FALSE; stencilFunc = D3DCMP_ALWAYS; stencilRef = 0; stencilMask = 0xffffffff;
        stencilWriteMask = 0xffffffff; stencilFail = stencilZFail = stencilPass = D3DSTENCILOP_KEEP;
        colorWrite = 0xf;
        alphaTest = FALSE; alphaFunc = D3DCMP_ALWAYS; alphaRef = 0;
    }
};

struct SSamplerState
{
    GLuint nGL;
    DWORD minFilter, magFilter, mipFilter, addressU, addressV, maxAniso;
    bool  bDirty;
    SSamplerState() : nGL( 0 ), minFilter( D3DTEXF_POINT ), magFilter( D3DTEXF_POINT ), mipFilter( D3DTEXF_NONE ),
                      addressU( D3DTADDRESS_WRAP ), addressV( D3DTADDRESS_WRAP ), maxAniso( 1 ), bDirty( true ) {}
};

GLenum GLCompare( DWORD d3d )
{
    switch ( d3d )
    {
        case D3DCMP_NEVER: return GL_NEVER;         case D3DCMP_LESS: return GL_LESS;
        case D3DCMP_EQUAL: return GL_EQUAL;         case D3DCMP_LESSEQUAL: return GL_LEQUAL;
        case D3DCMP_GREATER: return GL_GREATER;     case D3DCMP_NOTEQUAL: return GL_NOTEQUAL;
        case D3DCMP_GREATEREQUAL: return GL_GEQUAL; default: return GL_ALWAYS;
    }
}
GLenum GLBlend( DWORD d3d )
{
    switch ( d3d )
    {
        case D3DBLEND_ZERO: return GL_ZERO;                 case D3DBLEND_ONE: return GL_ONE;
        case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;        case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
        case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;        case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;       case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
        case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;       case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
        case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
        default: return GL_ONE;
    }
}
GLenum GLStencilOp( DWORD d3d )
{
    switch ( d3d )
    {
        case D3DSTENCILOP_ZERO: return GL_ZERO;        case D3DSTENCILOP_REPLACE: return GL_REPLACE;
        case D3DSTENCILOP_INCRSAT: return GL_INCR;     case D3DSTENCILOP_DECRSAT: return GL_DECR;
        case D3DSTENCILOP_INVERT: return GL_INVERT;    case D3DSTENCILOP_INCR: return GL_INCR_WRAP;
        case D3DSTENCILOP_DECR: return GL_DECR_WRAP;   default: return GL_KEEP;
    }
}

class CDevice : public IDirect3DDevice9
{
public:
    D3DPRESENT_PARAMETERS pp;
    CTexture2D *pBackColor;         /* the virtual back buffer, a render-target texture */
    CSurface   *pBackColorSurface;
    CSurface   *pBackDepth;         /* its depth/stencil renderbuffer */
    bool        bNeedReset;
    int         nLastSurfaceAlive;
    DWORD       dwFVF = 0;

    SRenderStates rs;
    bool          bStatesDirty;
    SSamplerState samplers[ 8 ];
    IDirect3DBaseTexture9 *textures[ 8 ];
    float    vsConst[ 96 ][ 4 ]; unsigned vsGen[ 96 ];
    float    psConst[ 8 ][ 4 ];  unsigned psGen[ 8 ];
    unsigned nGenCounter;
    CVertexShader *pVS; CPixelShader *pPS; CVertexDecl *pDecl;
    CVertexBuffer *pVB; UINT nVBOffset, nVBStride;
    CIndexBuffer  *pIB;
    CSurface *pRT, *pDS;
    bool      bFramebufferDirty;
    GLuint    nCurrentFBO;
    int       nRTWidth, nRTHeight;

    std::map< std::pair< CSurface *, CSurface * >, GLuint > fbos;
    std::map< SProgramKey, SProgram * > programs;
    SProgram *pCurrentProgram;
    GLuint    nScratchFBO;
    D3DGAMMARAMP gamma;
    bool      bInScene;

    CDevice() : pBackColor( 0 ), pBackColorSurface( 0 ), pBackDepth( 0 ), bNeedReset( false ), nLastSurfaceAlive( 1 ),
                bStatesDirty( true ), nGenCounter( 1 ), pVS( 0 ), pPS( 0 ), pDecl( 0 ), pVB( 0 ), nVBOffset( 0 ), nVBStride( 0 ),
                pIB( 0 ), pRT( 0 ), pDS( 0 ), bFramebufferDirty( true ), nCurrentFBO( 0 ), nRTWidth( 0 ), nRTHeight( 0 ),
                pCurrentProgram( 0 ), nScratchFBO( 0 ), bInScene( false )
    {
        memset( &pp, 0, sizeof( pp ) );
        memset( textures, 0, sizeof( textures ) );
        memset( vsConst, 0, sizeof( vsConst ) ); memset( vsGen, 0, sizeof( vsGen ) );
        memset( psConst, 0, sizeof( psConst ) ); memset( psGen, 0, sizeof( psGen ) );
        for ( int i = 0; i < 256; ++i )
            gamma.red[ i ] = gamma.green[ i ] = gamma.blue[ i ] = (WORD)( i << 8 );
        for ( int i = 0; i < 8; ++i )
        {
            glGenSamplers( 1, &samplers[ i ].nGL );
            samplers[ i ].bDirty = true;
        }
        glGenFramebuffers( 1, &nScratchFBO );
    }
    ~CDevice()
    {
        ReleaseBackBuffer();
        for ( std::map< std::pair< CSurface *, CSurface * >, GLuint >::iterator i = fbos.begin(); i != fbos.end(); ++i )
            glDeleteFramebuffers( 1, &i->second );
        for ( std::map< SProgramKey, SProgram * >::iterator i = programs.begin(); i != programs.end(); ++i )
        {
            glDeleteProgram( i->second->nGL );
            delete i->second;
        }
        for ( int i = 0; i < 8; ++i )
        {
            glDeleteSamplers( 1, &samplers[ i ].nGL );
            if ( textures[ i ] ) textures[ i ]->Release();
        }
        if ( pVS ) pVS->Release(); if ( pPS ) pPS->Release(); if ( pDecl ) pDecl->Release();
        if ( pVB ) pVB->Release(); if ( pIB ) pIB->Release();
        if ( pRT ) pRT->Release(); if ( pDS ) pDS->Release();
        glDeleteFramebuffers( 1, &nScratchFBO );
        if ( g_pDevice == this )
            g_pDevice = 0;
    }

    /* ---- back buffer -------------------------------------------------- */
    void ReleaseBackBuffer()
    {
        if ( pRT == pBackColorSurface && pRT ) { pRT->Release(); pRT = 0; }
        if ( pDS == pBackDepth && pDS ) { pDS->Release(); pDS = 0; }
        if ( pBackColorSurface ) { pBackColorSurface->Release(); pBackColorSurface = 0; }
        if ( pBackColor ) { pBackColor->Release(); pBackColor = 0; }
        if ( pBackDepth ) { pBackDepth->Release(); pBackDepth = 0; }
    }
    bool CreateBackBuffer( const D3DPRESENT_PARAMETERS *pParams )
    {
        ReleaseBackBuffer();
        pp = *pParams;
        if ( pp.BackBufferWidth == 0 || pp.BackBufferHeight == 0 )
        {
            pp.BackBufferWidth = g_hooks.getWindowWidth ? g_hooks.getWindowWidth() : 800;
            pp.BackBufferHeight = g_hooks.getWindowHeight ? g_hooks.getWindowHeight() : 600;
        }
        pBackColor = new CTexture2D;
        pBackColor->image.Create( GL_TEXTURE_2D, pp.BackBufferWidth, pp.BackBufferHeight, 1, D3DFMT_A8R8G8B8, true, false );
        IDirect3DSurface9 *pS = 0;
        pBackColor->GetSurfaceLevel( 0, &pS );
        pBackColorSurface = (CSurface *)pS;

        pBackDepth = new CSurface;
        pBackDepth->kind = SK_RENDERBUFFER;
        pBackDepth->fi = DescribeFormat( D3DFMT_D24S8 );
        pBackDepth->nWidth = pp.BackBufferWidth; pBackDepth->nHeight = pp.BackBufferHeight;
        glGenRenderbuffers( 1, &pBackDepth->nRenderbuffer );
        glBindRenderbuffer( GL_RENDERBUFFER, pBackDepth->nRenderbuffer );
        glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, pp.BackBufferWidth, pp.BackBufferHeight );
        glBindRenderbuffer( GL_RENDERBUFFER, 0 );

        /* the back buffer is the current target after Create/Reset, as in D3D */
        if ( pRT ) pRT->Release();
        if ( pDS ) pDS->Release();
        pRT = pBackColorSurface; pRT->AddRef();
        pDS = pBackDepth; pDS->AddRef();
        bFramebufferDirty = true;
        a5_set_client_size( (int)pp.BackBufferWidth, (int)pp.BackBufferHeight );
        D3DGL_LOG( "d3d9gles: back buffer %ux%u", pp.BackBufferWidth, pp.BackBufferHeight );
        return true;
    }

    /* ---- framebuffers ---------------------------------------------------- */
    GLuint GetFBO( CSurface *pColor, CSurface *pDepth )
    {
        std::pair< CSurface *, CSurface * > key( pColor, pDepth );
        std::map< std::pair< CSurface *, CSurface * >, GLuint >::iterator i = fbos.find( key );
        if ( i != fbos.end() )
            return i->second;
        GLuint fbo = 0;
        glGenFramebuffers( 1, &fbo );
        glBindFramebuffer( GL_FRAMEBUFFER, fbo );
        if ( pColor && pColor->kind == SK_TEXTURE )
            glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, pColor->pImage->FaceTarget( pColor->nFace ),
                                    pColor->pImage->nGL, pColor->nLevel );
        if ( pDepth && pDepth->kind == SK_RENDERBUFFER )
        {
            const bool bStencil = pDepth->fi.internalFormat == GL_DEPTH24_STENCIL8;
            glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, pDepth->nRenderbuffer );
            glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, bStencil ? pDepth->nRenderbuffer : 0 );
        }
        GLenum status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
        if ( status != GL_FRAMEBUFFER_COMPLETE )
            D3DGL_ERR( "d3d9gles: framebuffer incomplete 0x%04x (color %p %dx%d, depth %p %dx%d)", status,
                       (void *)pColor, pColor ? pColor->Width() : 0, pColor ? pColor->Height() : 0,
                       (void *)pDepth, pDepth ? pDepth->Width() : 0, pDepth ? pDepth->Height() : 0 );
        fbos[ key ] = fbo;
        return fbo;
    }
    void ForgetSurface( CSurface *pSurface )
    {
        for ( std::map< std::pair< CSurface *, CSurface * >, GLuint >::iterator i = fbos.begin(); i != fbos.end(); )
        {
            if ( i->first.first == pSurface || i->first.second == pSurface )
            {
                if ( nCurrentFBO == i->second ) { nCurrentFBO = 0; bFramebufferDirty = true; }
                glDeleteFramebuffers( 1, &i->second );
                fbos.erase( i++ );
            }
            else
                ++i;
        }
        if ( pRT == pSurface ) pRT = 0;     /* the surface is dying; it held no ref for us to drop */
        if ( pDS == pSurface ) pDS = 0;
    }
    void ApplyFramebuffer()
    {
        if ( !bFramebufferDirty )
            return;
        bFramebufferDirty = false;
        nCurrentFBO = GetFBO( pRT, pDS );
        glBindFramebuffer( GL_FRAMEBUFFER, nCurrentFBO );
        nRTWidth = pRT ? pRT->Width() : ( pDS ? pDS->Width() : 1 );
        nRTHeight = pRT ? pRT->Height() : ( pDS ? pDS->Height() : 1 );
        glViewport( 0, 0, nRTWidth, nRTHeight );
        glDisable( GL_SCISSOR_TEST );
    }

    /* ---- state ---------------------------------------------------------- */
    void ApplyRenderStates()
    {
        if ( !bStatesDirty )
            return;
        bStatesDirty = false;
        /* A5_D3D_FORCE=nocull,nodepth,noblend -- bring-up experiments */
        static int nForce = -1;
        if ( nForce < 0 )
        {
            const char *e = getenv( "A5_D3D_FORCE" );
            nForce = 0;
            if ( e && strstr( e, "nocull" ) ) nForce |= 1;
            if ( e && strstr( e, "nodepth" ) ) nForce |= 2;
            if ( e && strstr( e, "noblend" ) ) nForce |= 4;
        }
        if ( nForce & 2 ) rs.zEnable = FALSE;
        if ( nForce & 4 ) rs.alphaBlend = FALSE;
        if ( nForce & 1 ) rs.cull = D3DCULL_NONE;
        if ( rs.zEnable ) glEnable( GL_DEPTH_TEST ); else glDisable( GL_DEPTH_TEST );
        glDepthMask( rs.zWrite ? GL_TRUE : GL_FALSE );
        glDepthFunc( GLCompare( rs.zFunc ) );
        if ( rs.alphaBlend ) glEnable( GL_BLEND ); else glDisable( GL_BLEND );
        glBlendFunc( GLBlend( rs.srcBlend ), GLBlend( rs.dstBlend ) );
        /*  Culling.  D3D measures the winding in projected space, y up --
         *  same as GL window space -- so D3DCULL_CW would be glFrontFace(GL_CCW)
         *  + cull back.  Every draw here goes to a y-flipped framebuffer (see
         *  d3d9.h), which mirrors the winding, hence the opposite.  (Verified
         *  on the engine's 2D quads: drawn with D3DCULL_CW, visible on Windows,
         *  culled here until this was swapped.) */
        if ( rs.cull == D3DCULL_NONE )
            glDisable( GL_CULL_FACE );
        else
        {
            glEnable( GL_CULL_FACE );
            glCullFace( GL_BACK );
            glFrontFace( rs.cull == D3DCULL_CW ? GL_CW : GL_CCW );
        }
        if ( rs.stencilEnable ) glEnable( GL_STENCIL_TEST ); else glDisable( GL_STENCIL_TEST );
        glStencilFunc( GLCompare( rs.stencilFunc ), (GLint)rs.stencilRef, rs.stencilMask );
        glStencilOp( GLStencilOp( rs.stencilFail ), GLStencilOp( rs.stencilZFail ), GLStencilOp( rs.stencilPass ) );
        glStencilMask( rs.stencilWriteMask );
        glColorMask( ( rs.colorWrite & 1 ) != 0, ( rs.colorWrite & 2 ) != 0, ( rs.colorWrite & 4 ) != 0, ( rs.colorWrite & 8 ) != 0 );
    }
    void ApplySampler( int n )
    {
        SSamplerState &s = samplers[ n ];
        if ( !s.bDirty )
            return;
        s.bDirty = false;
        GLenum mag = s.magFilter == D3DTEXF_POINT ? GL_NEAREST : GL_LINEAR;
        GLenum min;
        const bool bPoint = s.minFilter == D3DTEXF_POINT;
        if ( s.mipFilter == D3DTEXF_NONE )
            min = bPoint ? GL_NEAREST : GL_LINEAR;
        else if ( s.mipFilter == D3DTEXF_POINT )
            min = bPoint ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
        else
            min = bPoint ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
        glSamplerParameteri( s.nGL, GL_TEXTURE_MAG_FILTER, mag );
        glSamplerParameteri( s.nGL, GL_TEXTURE_MIN_FILTER, min );
        glSamplerParameteri( s.nGL, GL_TEXTURE_WRAP_S, s.addressU == D3DTADDRESS_WRAP ? GL_REPEAT : GL_CLAMP_TO_EDGE );
        glSamplerParameteri( s.nGL, GL_TEXTURE_WRAP_T, s.addressV == D3DTADDRESS_WRAP ? GL_REPEAT : GL_CLAMP_TO_EDGE );
        if ( g_caps.bAnisotropy )
        {
            const bool bAniso = s.minFilter == D3DTEXF_ANISOTROPIC || s.magFilter == D3DTEXF_ANISOTROPIC;
            glSamplerParameterf( s.nGL, GL_TEXTURE_MAX_ANISOTROPY_EXT, bAniso ? (float)( s.maxAniso < 1 ? 1 : s.maxAniso ) : 1.0f );
        }
    }

    /* ---- programs ------------------------------------------------------- */
    GLuint CompileShader( GLenum type, const char *pszSource, const char *pszDefines, const char *pszName )
    {
        /* #version must stay the first line: split it off and put the defines after it. */
        const char *pszNewline = strchr( pszSource, '\n' );
        std::string src;
        if ( pszNewline )
        {
            src.assign( pszSource, pszNewline + 1 );
            src += pszDefines;
            src += pszNewline + 1;
        }
        else
            src = pszSource;
        const char *pszSrc = src.c_str();
        GLuint sh = glCreateShader( type );
        glShaderSource( sh, 1, &pszSrc, 0 );
        glCompileShader( sh );
        GLint ok = 0;
        glGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
        if ( !ok )
        {
            char szLog[ 2048 ] = { 0 };
            glGetShaderInfoLog( sh, sizeof( szLog ), 0, szLog );
            D3DGL_ERR( "d3d9gles: %s failed to compile:\n%s", pszName, szLog );
            glDeleteShader( sh );
            return 0;
        }
        return sh;
    }
    SProgram *GetProgram( int nCubeMask )
    {
        if ( !pVS || !pPS || !pVS->pEntry || !pPS->pEntry )
            return 0;
        SProgramKey key = { pVS, pPS, nCubeMask };
        std::map< SProgramKey, SProgram * >::iterator i = programs.find( key );
        if ( i != programs.end() )
            return i->second;

        if ( !pVS->nGL )
            pVS->nGL = CompileShader( GL_VERTEX_SHADER, pVS->pEntry->glsl, "", pVS->pEntry->name );
        std::map< int, GLuint >::iterator f = pPS->compiled.find( nCubeMask );
        GLuint fs = 0;
        if ( f == pPS->compiled.end() )
        {
            std::string defines;
            for ( int s = 0; s < 8; ++s )
                if ( nCubeMask & ( 1 << s ) )
                {
                    char szBuf[ 32 ];
                    snprintf( szBuf, sizeof( szBuf ), "#define A5_CUBE%d 1\n", s );
                    defines += szBuf;
                }
            fs = CompileShader( GL_FRAGMENT_SHADER, pPS->pEntry->glsl, defines.c_str(), pPS->pEntry->name );
            pPS->compiled[ nCubeMask ] = fs;
        }
        else
            fs = f->second;

        SProgram *p = new SProgram;
        if ( pVS->nGL && fs )
        {
            p->nGL = glCreateProgram();
            glAttachShader( p->nGL, pVS->nGL );
            glAttachShader( p->nGL, fs );
            glLinkProgram( p->nGL );
            GLint ok = 0;
            glGetProgramiv( p->nGL, GL_LINK_STATUS, &ok );
            if ( !ok )
            {
                char szLog[ 2048 ] = { 0 };
                glGetProgramInfoLog( p->nGL, sizeof( szLog ), 0, szLog );
                D3DGL_ERR( "d3d9gles: link %s + %s failed:\n%s", pVS->pEntry->name, pPS->pEntry->name, szLog );
                glDeleteProgram( p->nGL );
                p->nGL = 0;
            }
        }
        if ( p->nGL )
        {
            glUseProgram( p->nGL );
            char szName[ 16 ];
            for ( int r = 0; r < 96; ++r )
            {
                snprintf( szName, sizeof( szName ), "vc[%d]", r );
                p->locVc[ r ] = glGetUniformLocation( p->nGL, szName );
            }
            for ( int r = 0; r < 8; ++r )
            {
                snprintf( szName, sizeof( szName ), "pc[%d]", r );
                p->locPc[ r ] = glGetUniformLocation( p->nGL, szName );
            }
            p->locPosFixup = glGetUniformLocation( p->nGL, "posFixup" );
            p->locAlphaFunc = glGetUniformLocation( p->nGL, "alphaFunc" );
            p->locAlphaRef = glGetUniformLocation( p->nGL, "alphaRef" );
            for ( int s = 0; s < 8; ++s )
            {
                snprintf( szName, sizeof( szName ), "s%d", s );
                p->locSampler[ s ] = glGetUniformLocation( p->nGL, szName );
                if ( p->locSampler[ s ] >= 0 )
                    glUniform1i( p->locSampler[ s ], s );
            }
            pCurrentProgram = 0;   /* glUseProgram changed */
        }
        programs[ key ] = p;
        return p;
    }
    int CurrentCubeMask()
    {
        int nMask = 0;
        for ( int s = 0; s < 8; ++s )
            if ( textures[ s ] && textures[ s ]->GetType() == D3DRTYPE_CUBETEXTURE )
                nMask |= 1 << s;
        return nMask;
    }
    void ApplyProgramAndUniforms()
    {
        SProgram *p = GetProgram( CurrentCubeMask() );
        if ( !p || !p->nGL )
        {
            ++g_stats.nDrawsNoProgram;
            if ( g_stats.nDrawsNoProgram <= 8 )
                D3DGL_WARN( "d3d9gles: draw without a usable program (vs %s, ps %s, fvf 0x%x)",
                            pVS ? ( pVS->pEntry ? pVS->pEntry->name : "unknown-asm" ) : "none",
                            pPS ? ( pPS->pEntry ? pPS->pEntry->name : "unknown-asm" ) : "none",
                            (unsigned)dwFVF );
            return;
        }
        if ( p != pCurrentProgram )
        {
            glUseProgram( p->nGL );
            pCurrentProgram = p;
        }
        /* constants: only registers this program has, only when they changed */
        for ( int r = 0; r < 96; ++r )
            if ( p->locVc[ r ] >= 0 && p->vsGen[ r ] != vsGen[ r ] )
            {
                glUniform4fv( p->locVc[ r ], 1, &vsConst[ r ][ 0 ] );
                p->vsGen[ r ] = vsGen[ r ];
            }
        for ( int r = 0; r < 8; ++r )
            if ( p->locPc[ r ] >= 0 && p->psGen[ r ] != psGen[ r ] )
            {
                glUniform4fv( p->locPc[ r ], 1, &psConst[ r ][ 0 ] );
                p->psGen[ r ] = psGen[ r ];
            }
        const int nAlphaFunc = rs.alphaTest ? (int)rs.alphaFunc : 0;
        const float fAlphaRef = (float)rs.alphaRef / 255.0f;
        if ( p->locAlphaFunc >= 0 && nAlphaFunc != p->nLastAlphaFunc ) { glUniform1i( p->locAlphaFunc, nAlphaFunc ); p->nLastAlphaFunc = nAlphaFunc; }
        if ( p->locAlphaRef >= 0 && fAlphaRef != p->fLastAlphaRef ) { glUniform1f( p->locAlphaRef, fAlphaRef ); p->fLastAlphaRef = fAlphaRef; }
        /* clip fixup: y flip always (framebuffer memory is D3D layout), half-pixel offset */
        float fix[ 4 ] = { -1.0f, 0.0f, 1.0f / (float)nRTWidth, 1.0f / (float)nRTHeight };
        if ( p->locPosFixup >= 0 && memcmp( fix, p->lastPosFixup, sizeof( fix ) ) != 0 )
        {
            glUniform4fv( p->locPosFixup, 1, fix );
            memcpy( p->lastPosFixup, fix, sizeof( fix ) );
        }
        /* textures the program samples */
        for ( int s = 0; s < 8; ++s )
        {
            if ( p->locSampler[ s ] < 0 )
                continue;
            glActiveTexture( GL_TEXTURE0 + s );
            IDirect3DBaseTexture9 *pT = textures[ s ];
            if ( pT && pT->GetType() == D3DRTYPE_CUBETEXTURE )
                glBindTexture( GL_TEXTURE_CUBE_MAP, ((CCubeTexture *)pT)->image.nGL );
            else if ( pT )
                glBindTexture( GL_TEXTURE_2D, ((CTexture2D *)pT)->image.nGL );
            else
                glBindTexture( GL_TEXTURE_2D, 0 );
            ApplySampler( s );
            glBindSampler( s, samplers[ s ].nGL );
        }
    }
    void ApplyVertexLayout( int nBaseVertex )
    {
        if ( !pVB || !pDecl )
            return;
        glBindBuffer( GL_ARRAY_BUFFER, pVB->buf.nGL );
        bool used[ 8 ] = { false, false, false, false, false, false, false, false };
        for ( size_t i = 0; i < pDecl->elements.size(); ++i )
        {
            const D3DVERTEXELEMENT9 &e = pDecl->elements[ i ];
            const int loc = AttribLocation( e.Usage, e.UsageIndex );
            if ( loc < 0 )
                continue;
            GLint size; GLenum type; GLboolean norm;
            switch ( e.Type )
            {
                case D3DDECLTYPE_FLOAT1: size = 1; type = GL_FLOAT; norm = GL_FALSE; break;
                case D3DDECLTYPE_FLOAT2: size = 2; type = GL_FLOAT; norm = GL_FALSE; break;
                case D3DDECLTYPE_FLOAT3: size = 3; type = GL_FLOAT; norm = GL_FALSE; break;
                case D3DDECLTYPE_FLOAT4: size = 4; type = GL_FLOAT; norm = GL_FALSE; break;
                case D3DDECLTYPE_D3DCOLOR: size = 4; type = GL_UNSIGNED_BYTE; norm = GL_TRUE; break;
                case D3DDECLTYPE_UBYTE4: size = 4; type = GL_UNSIGNED_BYTE; norm = GL_FALSE; break;
                case D3DDECLTYPE_SHORT2: size = 2; type = GL_SHORT; norm = GL_FALSE; break;
                case D3DDECLTYPE_SHORT4: size = 4; type = GL_SHORT; norm = GL_FALSE; break;
                default: continue;
            }
            const size_t nOffset = (size_t)nVBOffset + (size_t)nBaseVertex * nVBStride + e.Offset;
            glVertexAttribPointer( loc, size, type, norm, nVBStride, (const void *)nOffset );
            glEnableVertexAttribArray( loc );
            used[ loc ] = true;
        }
        for ( int loc = 0; loc < 8; ++loc )
            if ( !used[ loc ] )
                glDisableVertexAttribArray( loc );
    }
    void PrepareDraw( int nBaseVertex )
    {
        ApplyFramebuffer();
        ApplyRenderStates();
        ApplyProgramAndUniforms();
        ApplyVertexLayout( nBaseVertex );
    }
    /*  A5_D3D_TRACE=<n>: log every draw of the first n Present()s. */
    void TraceDraw( const char *pszKind, D3DPRIMITIVETYPE type, UINT nPrims, UINT nStart )
    {
        static int nTraceFrames = -1;
        if ( nTraceFrames < 0 ) { const char *e = getenv( "A5_D3D_TRACE" ); nTraceFrames = e ? atoi( e ) : 0; }
        if ( g_stats.nPresents >= nTraceFrames )
            return;
        D3DGL_LOG( "d3d9gles: [f%d] %s type %d prims %u start %u | vs %s ps %s | rt %s %dx%d z %s | blend %d(%d,%d) ztest %d zwrite %d cull %d alphatest %d | tex0 %s | c10 %.4f %.4f %.4f %.4f",
                   g_stats.nPresents, pszKind, (int)type, nPrims, nStart,
                   pVS && pVS->pEntry ? pVS->pEntry->name : "?", pPS && pPS->pEntry ? pPS->pEntry->name : "?",
                   pRT == pBackColorSurface ? "backbuffer" : ( pRT ? "texture" : "none" ), nRTWidth, nRTHeight, pDS ? "yes" : "no",
                   (int)rs.alphaBlend, (int)rs.srcBlend, (int)rs.dstBlend, (int)rs.zEnable, (int)rs.zWrite, (int)rs.cull, (int)rs.alphaTest,
                   textures[ 0 ] ? "set" : "none",
                   vsConst[ 10 ][ 0 ], vsConst[ 10 ][ 1 ], vsConst[ 10 ][ 2 ], vsConst[ 10 ][ 3 ] );
    }

    /* ---- IDirect3DDevice9 --------------------------------------------- */
    virtual HRESULT TestCooperativeLevel()
    {
        const int nAlive = g_hooks.isSurfaceAlive ? g_hooks.isSurfaceAlive() : 1;
        if ( !nAlive )
        {
            if ( nLastSurfaceAlive )
                D3DGL_LOG( "d3d9gles: surface gone - device lost" );
            nLastSurfaceAlive = 0;
            return D3DERR_DEVICELOST;
        }
        if ( !nLastSurfaceAlive )
        {
            D3DGL_LOG( "d3d9gles: surface back - device needs a Reset" );
            nLastSurfaceAlive = 1;
            bNeedReset = true;
        }
        return bNeedReset ? D3DERR_DEVICENOTRESET : D3D_OK;
    }
    virtual HRESULT Reset( D3DPRESENT_PARAMETERS *pParams )
    {
        bNeedReset = false;
        pCurrentProgram = 0;
        bStatesDirty = true;
        for ( int i = 0; i < 8; ++i ) samplers[ i ].bDirty = true;
        CreateBackBuffer( pParams );
        return D3D_OK;
    }
    virtual HRESULT Present( const RECT *, const RECT *, HWND, const void * )
    {
        const int nWinW = g_hooks.getWindowWidth ? g_hooks.getWindowWidth() : (int)pp.BackBufferWidth;
        const int nWinH = g_hooks.getWindowHeight ? g_hooks.getWindowHeight() : (int)pp.BackBufferHeight;
        /* letterbox: largest rectangle of the back buffer's aspect inside the window */
        int dw = nWinW, dh = (int)( (long long)nWinW * pp.BackBufferHeight / pp.BackBufferWidth );
        if ( dh > nWinH ) { dh = nWinH; dw = (int)( (long long)nWinH * pp.BackBufferWidth / pp.BackBufferHeight ); }
        const int dx = ( nWinW - dw ) / 2, dy = ( nWinH - dh ) / 2;

        GLuint src = GetFBO( pBackColorSurface, pBackDepth );
        glBindFramebuffer( GL_READ_FRAMEBUFFER, src );
        {
            /* A5_D3D_TRACE: histogram of the back buffer so "nothing visible" can be
             * told apart from "drawn but not shown" */
            static int nTraceFrames = -1;
            if ( nTraceFrames < 0 ) { const char *e = getenv( "A5_D3D_TRACE" ); nTraceFrames = e ? atoi( e ) : 0; }
            if ( g_stats.nPresents < nTraceFrames && ( g_stats.nPresents % 20 ) == 0 )
            {
                const int w = pp.BackBufferWidth, h = pp.BackBufferHeight;
                std::vector< unsigned char > px( (size_t)w * h * 4 );
                glReadPixels( 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &px[ 0 ] );
                std::map< unsigned, int > hist;
                for ( int i = 0; i < w * h; i += 7 )
                    hist[ ( px[ i * 4 ] << 16 ) | ( px[ i * 4 + 1 ] << 8 ) | px[ i * 4 + 2 ] ]++;
                std::string sz;
                int nShown = 0;
                for ( std::map< unsigned, int >::iterator it = hist.begin(); it != hist.end() && nShown < 6; ++it, ++nShown )
                {
                    char b[ 48 ]; snprintf( b, sizeof( b ), " %06x:%d", it->first, it->second ); sz += b;
                }
                D3DGL_LOG( "d3d9gles: [f%d] back buffer %dx%d: %d distinct colours (of %d samples):%s%s",
                           g_stats.nPresents, w, h, (int)hist.size(), w * h / 7, sz.c_str(), hist.size() > 6 ? " ..." : "" );
            }
        }
        glBindFramebuffer( GL_DRAW_FRAMEBUFFER, 0 );
        glDisable( GL_SCISSOR_TEST );
        glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
        glViewport( 0, 0, nWinW, nWinH );
        glClearColor( 0, 0, 0, 1 );
        glClear( GL_COLOR_BUFFER_BIT );
        /* back buffer memory is D3D layout (row 0 = top); the window's row 0 is the
         * bottom, so the blit flips vertically. */
        glBlitFramebuffer( 0, 0, pp.BackBufferWidth, pp.BackBufferHeight,
                           dx, dy + dh, dx + dw, dy, GL_COLOR_BUFFER_BIT, GL_LINEAR );
        if ( g_hooks.present )
            g_hooks.present();
        ++g_stats.nPresents;
        bFramebufferDirty = true;
        bStatesDirty = true;
        return D3D_OK;
    }
    virtual HRESULT BeginScene() { bInScene = true; return D3D_OK; }
    virtual HRESULT EndScene()   { bInScene = false; return D3D_OK; }
    virtual HRESULT ValidateDevice( DWORD *pNumPasses ) { if ( pNumPasses ) *pNumPasses = 1; return D3D_OK; }
    virtual void    SetGammaRamp( UINT, DWORD, const D3DGAMMARAMP *pRamp ) { if ( pRamp ) gamma = *pRamp; }
    virtual void    GetGammaRamp( UINT, D3DGAMMARAMP *pRamp ) { if ( pRamp ) *pRamp = gamma; }
    virtual HRESULT GetFrontBufferData( UINT, IDirect3DSurface9 *pDestSurface )
    {
        CSurface *pDst = (CSurface *)pDestSurface;
        if ( !pDst || pDst->kind != SK_OFFSCREEN || !pBackColorSurface )
            return D3DERR_INVALIDCALL;
        const int w = (int)pp.BackBufferWidth < pDst->nWidth ? (int)pp.BackBufferWidth : pDst->nWidth;
        const int h = (int)pp.BackBufferHeight < pDst->nHeight ? (int)pp.BackBufferHeight : pDst->nHeight;
        std::vector< uint8_t > rgba( (size_t)w * h * 4 );
        DeviceReadbackTexture( &pBackColor->image, 0, 0, 0, 0, w, h, &rgba[ 0 ], w * 4 );
        /* DeviceReadbackTexture already returns D3D A8R8G8B8 rows */
        const int nDstPitch = pDst->nWidth * 4;
        for ( int y = 0; y < h; ++y )
            memcpy( &pDst->pixels[ (size_t)y * nDstPitch ], &rgba[ (size_t)y * w * 4 ], (size_t)w * 4 );
        return D3D_OK;
    }

    virtual HRESULT CreateTexture( UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE * )
    {
        CTexture2D *p = new CTexture2D;
        const bool bRT = ( Usage & D3DUSAGE_RENDERTARGET ) != 0;
        const bool bSys = Pool == D3DPOOL_SYSTEMMEM || Pool == D3DPOOL_SCRATCH;
        if ( !p->image.Create( GL_TEXTURE_2D, (int)Width, (int)Height, (int)Levels, Format, bRT, bSys ) )
        {
            p->Release();
            return D3DERR_INVALIDCALL;
        }
        *ppTexture = p;
        return D3D_OK;
    }
    virtual HRESULT CreateCubeTexture( UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE * )
    {
        CCubeTexture *p = new CCubeTexture;
        const bool bRT = ( Usage & D3DUSAGE_RENDERTARGET ) != 0;
        const bool bSys = Pool == D3DPOOL_SYSTEMMEM || Pool == D3DPOOL_SCRATCH;
        if ( !p->image.Create( GL_TEXTURE_CUBE_MAP, (int)EdgeLength, (int)EdgeLength, (int)Levels, Format, bRT, bSys ) )
        {
            p->Release();
            return D3DERR_INVALIDCALL;
        }
        *ppCubeTexture = p;
        return D3D_OK;
    }
    virtual HRESULT CreateVertexBuffer( UINT Length, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE * )
    {
        CVertexBuffer *p = new CVertexBuffer;
        p->buf.Create( GL_ARRAY_BUFFER, Length );
        *ppVertexBuffer = p;
        return D3D_OK;
    }
    virtual HRESULT CreateIndexBuffer( UINT Length, DWORD, D3DFORMAT Format, D3DPOOL, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE * )
    {
        CIndexBuffer *p = new CIndexBuffer;
        p->buf.Create( GL_ELEMENT_ARRAY_BUFFER, Length );
        p->indexType = Format == D3DFMT_INDEX32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        p->nIndexSize = Format == D3DFMT_INDEX32 ? 4 : 2;
        *ppIndexBuffer = p;
        return D3D_OK;
    }
    virtual HRESULT CreateDepthStencilSurface( UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9 **ppSurface, HANDLE * )
    {
        CSurface *p = new CSurface;
        p->kind = SK_RENDERBUFFER;
        p->fi = DescribeFormat( Format );
        if ( p->fi.cls != FC_DEPTH )
        {
            p->Release();
            return D3DERR_INVALIDCALL;
        }
        p->nWidth = (int)Width; p->nHeight = (int)Height;
        glGenRenderbuffers( 1, &p->nRenderbuffer );
        glBindRenderbuffer( GL_RENDERBUFFER, p->nRenderbuffer );
        glRenderbufferStorage( GL_RENDERBUFFER, p->fi.internalFormat, Width, Height );
        glBindRenderbuffer( GL_RENDERBUFFER, 0 );
        CheckGLError( "CreateDepthStencilSurface" );
        *ppSurface = p;
        return D3D_OK;
    }
    virtual HRESULT CreateOffscreenPlainSurface( UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL, IDirect3DSurface9 **ppSurface, HANDLE * )
    {
        CSurface *p = new CSurface;
        p->kind = SK_OFFSCREEN;
        p->fi = DescribeFormat( Format );
        /* GetFrontBufferData copies the whole back buffer in; make sure it fits. */
        p->nWidth = (int)( Width > pp.BackBufferWidth ? Width : pp.BackBufferWidth );
        p->nHeight = (int)( Height > pp.BackBufferHeight ? Height : pp.BackBufferHeight );
        p->pixels.assign( (size_t)LevelBytes( p->fi, p->nWidth, p->nHeight ), 0 );
        *ppSurface = p;
        return D3D_OK;
    }
    virtual HRESULT UpdateSurface( IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, const POINT *pDestPoint )
    {
        CSurface *pSrc = (CSurface *)pSourceSurface, *pDst = (CSurface *)pDestinationSurface;
        if ( !pSrc || !pDst || pDst->kind != SK_TEXTURE )
            return D3DERR_INVALIDCALL;
        RECT r;
        if ( pSourceRect ) r = *pSourceRect;
        else { r.left = 0; r.top = 0; r.right = pSrc->Width(); r.bottom = pSrc->Height(); }
        const int dx = pDestPoint ? (int)pDestPoint->x : 0, dy = pDestPoint ? (int)pDestPoint->y : 0;
        const int w = r.right - r.left, h = r.bottom - r.top;
        const SFormatInfo &f = pDst->pImage->fi;
        if ( f.d3d != pSrc->Format().d3d || f.cls == FC_DXT )
        {
            D3DGL_ASSERT( 0 && "UpdateSurface format mismatch" );
            return D3DERR_INVALIDCALL;
        }
        /* source rows */
        const uint8_t *pSrcBits;
        int nSrcPitch;
        if ( pSrc->kind == SK_OFFSCREEN ) { pSrcBits = &pSrc->pixels[ 0 ]; nSrcPitch = LevelPitch( f, pSrc->nWidth ); }
        else if ( pSrc->kind == SK_TEXTURE && !pSrc->pImage->bRenderTarget )
        { pSrcBits = &pSrc->pImage->Shadow( pSrc->nFace, pSrc->nLevel )[ 0 ]; nSrcPitch = LevelPitch( f, pSrc->Width() ); }
        else
            return D3DERR_INVALIDCALL;
        pSrcBits += (size_t)r.top * nSrcPitch + (size_t)r.left * f.nBytesPerPixel;
        /* into the destination shadow (if any), then the GPU */
        if ( !pDst->pImage->bRenderTarget )
        {
            std::vector< uint8_t > &s = pDst->pImage->Shadow( pDst->nFace, pDst->nLevel );
            const int nDstPitch = LevelPitch( f, pDst->Width() );
            for ( int y = 0; y < h; ++y )
                memcpy( &s[ (size_t)( dy + y ) * nDstPitch + (size_t)dx * f.nBytesPerPixel ],
                        pSrcBits + (size_t)y * nSrcPitch, (size_t)w * f.nBytesPerPixel );
        }
        pDst->pImage->Upload( pDst->nFace, pDst->nLevel, dx, dy, w, h, pSrcBits, nSrcPitch );
        return D3D_OK;
    }
    virtual HRESULT CopyRects( IDirect3DSurface9 *, const RECT *, UINT, IDirect3DSurface9 *, const POINT * )
    {
        D3DGL_ASSERT( 0 && "CopyRects is not used by the engine" );
        return D3DERR_INVALIDCALL;
    }
    virtual HRESULT CreateVertexShader( const DWORD *pFunction, IDirect3DVertexShader9 **ppShader )
    {
        CVertexShader *p = new CVertexShader;
        p->pEntry = FindShaderEntry( pFunction, true );
        if ( !p->pEntry )
            D3DGL_ERR( "d3d9gles: vertex shader bytecode not in the GLSL table" );
        *ppShader = p;
        return p->pEntry ? D3D_OK : D3DERR_INVALIDCALL;
    }
    virtual HRESULT CreatePixelShader( const DWORD *pFunction, IDirect3DPixelShader9 **ppShader )
    {
        CPixelShader *p = new CPixelShader;
        p->pEntry = FindShaderEntry( pFunction, false );
        if ( !p->pEntry )
            D3DGL_ERR( "d3d9gles: pixel shader bytecode not in the GLSL table" );
        *ppShader = p;
        return p->pEntry ? D3D_OK : D3DERR_INVALIDCALL;
    }
    virtual HRESULT CreateVertexDeclaration( const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl )
    {
        CVertexDecl *p = new CVertexDecl;
        for ( const D3DVERTEXELEMENT9 *e = pVertexElements; e->Stream != 0xFF; ++e )
            p->elements.push_back( *e );
        *ppDecl = p;
        return D3D_OK;
    }
    virtual HRESULT CreateQuery( D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery )
    {
        if ( Type != D3DQUERYTYPE_VCACHE )
            return D3DERR_NOTAVAILABLE;
        *ppQuery = new CQuery;
        return D3D_OK;
    }

    virtual HRESULT SetRenderTarget( DWORD, IDirect3DSurface9 *pRenderTarget )
    {
        CSurface *p = (CSurface *)pRenderTarget;
        if ( p == pRT )
            return D3D_OK;
        if ( p ) p->AddRef();
        if ( pRT ) pRT->Release();
        pRT = p;
        bFramebufferDirty = true;
        return D3D_OK;
    }
    virtual HRESULT GetRenderTarget( DWORD, IDirect3DSurface9 **ppRenderTarget )
    {
        if ( pRT ) pRT->AddRef();
        *ppRenderTarget = pRT;
        return pRT ? D3D_OK : D3DERR_INVALIDCALL;
    }
    virtual HRESULT SetDepthStencilSurface( IDirect3DSurface9 *pNewZStencil )
    {
        CSurface *p = (CSurface *)pNewZStencil;
        if ( p == pDS )
            return D3D_OK;
        if ( p ) p->AddRef();
        if ( pDS ) pDS->Release();
        pDS = p;
        bFramebufferDirty = true;
        return D3D_OK;
    }
    virtual HRESULT GetDepthStencilSurface( IDirect3DSurface9 **ppZStencilSurface )
    {
        if ( pDS ) pDS->AddRef();
        *ppZStencilSurface = pDS;
        return pDS ? D3D_OK : D3DERR_INVALIDCALL;
    }
    virtual HRESULT Clear( DWORD, const D3DRECT *, DWORD Flags, DWORD Color, float Z, DWORD Stencil )
    {
        ++g_stats.nClears;
        ApplyFramebuffer();
        GLbitfield mask = 0;
        if ( Flags & D3DCLEAR_TARGET )
        {
            glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
            glClearColor( ( ( Color >> 16 ) & 0xff ) / 255.0f, ( ( Color >> 8 ) & 0xff ) / 255.0f,
                          ( Color & 0xff ) / 255.0f, ( ( Color >> 24 ) & 0xff ) / 255.0f );
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if ( Flags & D3DCLEAR_ZBUFFER )
        {
            glDepthMask( GL_TRUE );
            glClearDepthf( Z );
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        if ( Flags & D3DCLEAR_STENCIL )
        {
            glStencilMask( 0xffffffff );
            glClearStencil( (GLint)Stencil );
            mask |= GL_STENCIL_BUFFER_BIT;
        }
        glDisable( GL_SCISSOR_TEST );
        glClear( mask );
        bStatesDirty = true;    /* masks were touched */
        return D3D_OK;
    }

    virtual HRESULT SetRenderState( D3DRENDERSTATETYPE State, DWORD Value )
    {
        switch ( State )
        {
            case D3DRS_ZENABLE: rs.zEnable = Value != D3DZB_FALSE; break;
            case D3DRS_ZWRITEENABLE: rs.zWrite = Value; break;
            case D3DRS_ZFUNC: rs.zFunc = Value; break;
            case D3DRS_ALPHABLENDENABLE: rs.alphaBlend = Value; break;
            case D3DRS_SRCBLEND: rs.srcBlend = Value; break;
            case D3DRS_DESTBLEND: rs.dstBlend = Value; break;
            case D3DRS_CULLMODE: rs.cull = Value; break;
            case D3DRS_FILLMODE: rs.fill = Value; break;     /* no wireframe in GLES */
            case D3DRS_STENCILENABLE: rs.stencilEnable = Value; break;
            case D3DRS_STENCILFUNC: rs.stencilFunc = Value; break;
            case D3DRS_STENCILREF: rs.stencilRef = Value; break;
            case D3DRS_STENCILMASK: rs.stencilMask = Value; break;
            case D3DRS_STENCILWRITEMASK: rs.stencilWriteMask = Value; break;
            case D3DRS_STENCILFAIL: rs.stencilFail = Value; break;
            case D3DRS_STENCILZFAIL: rs.stencilZFail = Value; break;
            case D3DRS_STENCILPASS: rs.stencilPass = Value; break;
            case D3DRS_COLORWRITEENABLE: rs.colorWrite = Value; break;
            case D3DRS_ALPHATESTENABLE: rs.alphaTest = Value; break;
            case D3DRS_ALPHAFUNC: rs.alphaFunc = Value; break;
            case D3DRS_ALPHAREF: rs.alphaRef = Value; break;
            default: return D3D_OK;      /* LIGHTING, DITHERENABLE, TEXTUREFACTOR...: no effect here */
        }
        bStatesDirty = true;
        return D3D_OK;
    }
    virtual HRESULT SetSamplerState( DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value )
    {
        if ( Sampler >= 8 ) return D3DERR_INVALIDCALL;
        SSamplerState &s = samplers[ Sampler ];
        switch ( Type )
        {
            case D3DSAMP_MINFILTER: s.minFilter = Value; break;
            case D3DSAMP_MAGFILTER: s.magFilter = Value; break;
            case D3DSAMP_MIPFILTER: s.mipFilter = Value; break;
            case D3DSAMP_ADDRESSU: s.addressU = Value; break;
            case D3DSAMP_ADDRESSV: s.addressV = Value; break;
            case D3DSAMP_MAXANISOTROPY: s.maxAniso = Value; break;
            default: return D3D_OK;      /* MIPMAPLODBIAS: no GLES sampler equivalent; the engine sets 0 */
        }
        s.bDirty = true;
        return D3D_OK;
    }
    virtual HRESULT SetTextureStageState( DWORD, D3DTEXTURESTAGESTATETYPE, DWORD ) { return D3D_OK; }   /* fixed function: never taken */
    virtual HRESULT SetTexture( DWORD Stage, IDirect3DBaseTexture9 *pTexture )
    {
        if ( Stage >= 8 ) return D3DERR_INVALIDCALL;
        if ( textures[ Stage ] == pTexture ) return D3D_OK;
        if ( pTexture ) pTexture->AddRef();
        if ( textures[ Stage ] ) textures[ Stage ]->Release();
        textures[ Stage ] = pTexture;
        return D3D_OK;
    }
    virtual HRESULT SetTransform( D3DTRANSFORMSTATETYPE, const D3DMATRIX * ) { return D3D_OK; }
    virtual HRESULT SetMaterial( const D3DMATERIAL9 * ) { return D3D_OK; }
    virtual HRESULT SetLight( DWORD, const D3DLIGHT9 * ) { return D3D_OK; }
    virtual HRESULT LightEnable( DWORD, BOOL ) { return D3D_OK; }
    virtual HRESULT SetSoftwareVertexProcessing( BOOL ) { return D3D_OK; }
    virtual HRESULT SetFVF( DWORD dw ) { dwFVF = dw; return D3D_OK; }

    virtual HRESULT SetVertexShader( IDirect3DVertexShader9 *pShader )
    {
        CVertexShader *p = (CVertexShader *)pShader;
        if ( p == pVS ) return D3D_OK;
        if ( p ) p->AddRef();
        if ( pVS ) pVS->Release();
        pVS = p;
        return D3D_OK;
    }
    virtual HRESULT SetPixelShader( IDirect3DPixelShader9 *pShader )
    {
        CPixelShader *p = (CPixelShader *)pShader;
        if ( p == pPS ) return D3D_OK;
        if ( p ) p->AddRef();
        if ( pPS ) pPS->Release();
        pPS = p;
        return D3D_OK;
    }
    virtual HRESULT SetVertexDeclaration( IDirect3DVertexDeclaration9 *pDeclIn )
    {
        CVertexDecl *p = (CVertexDecl *)pDeclIn;
        if ( p == pDecl ) return D3D_OK;
        if ( p ) p->AddRef();
        if ( pDecl ) pDecl->Release();
        pDecl = p;
        return D3D_OK;
    }
    virtual HRESULT SetVertexShaderConstantF( UINT StartRegister, const float *pConstantData, UINT Vector4fCount )
    {
        for ( UINT i = 0; i < Vector4fCount && StartRegister + i < 96; ++i )
        {
            memcpy( vsConst[ StartRegister + i ], pConstantData + i * 4, 16 );
            vsGen[ StartRegister + i ] = ++nGenCounter;
        }
        return D3D_OK;
    }
    virtual HRESULT SetPixelShaderConstantF( UINT StartRegister, const float *pConstantData, UINT Vector4fCount )
    {
        for ( UINT i = 0; i < Vector4fCount && StartRegister + i < 8; ++i )
        {
            memcpy( psConst[ StartRegister + i ], pConstantData + i * 4, 16 );
            psGen[ StartRegister + i ] = ++nGenCounter;
        }
        return D3D_OK;
    }
    virtual HRESULT SetStreamSource( UINT, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride )
    {
        CVertexBuffer *p = (CVertexBuffer *)pStreamData;
        if ( p ) p->AddRef();
        if ( pVB ) pVB->Release();
        pVB = p; nVBOffset = OffsetInBytes; nVBStride = Stride;
        return D3D_OK;
    }
    virtual HRESULT SetIndices( IDirect3DIndexBuffer9 *pIndexData )
    {
        CIndexBuffer *p = (CIndexBuffer *)pIndexData;
        if ( p == pIB ) return D3D_OK;
        if ( p ) p->AddRef();
        if ( pIB ) pIB->Release();
        pIB = p;
        return D3D_OK;
    }
    virtual HRESULT DrawPrimitive( D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount )
    {
        ++g_stats.nDraws;
        PrepareDraw( 0 );
        TraceDraw( "DrawPrimitive", PrimitiveType, PrimitiveCount, StartVertex );
        GLenum mode; GLsizei count;
        switch ( PrimitiveType )
        {
            case D3DPT_LINELIST: mode = GL_LINES; count = PrimitiveCount * 2; break;
            case D3DPT_LINESTRIP: mode = GL_LINE_STRIP; count = PrimitiveCount + 1; break;
            case D3DPT_TRIANGLESTRIP: mode = GL_TRIANGLE_STRIP; count = PrimitiveCount + 2; break;
            default: mode = GL_TRIANGLES; count = PrimitiveCount * 3; break;
        }
        glDrawArrays( mode, (GLint)StartVertex, count );
        return D3D_OK;
    }
    virtual HRESULT DrawIndexedPrimitive( D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT, UINT, UINT StartIndex, UINT PrimitiveCount )
    {
        if ( !pIB )
            return D3DERR_INVALIDCALL;
        ++g_stats.nDraws;
        PrepareDraw( BaseVertexIndex );
        TraceDraw( "DrawIndexedPrimitive", PrimitiveType, PrimitiveCount, StartIndex );
        glBindBuffer( GL_ELEMENT_ARRAY_BUFFER, pIB->buf.nGL );
        GLenum mode; GLsizei count;
        switch ( PrimitiveType )
        {
            case D3DPT_LINELIST: mode = GL_LINES; count = PrimitiveCount * 2; break;
            case D3DPT_LINESTRIP: mode = GL_LINE_STRIP; count = PrimitiveCount + 1; break;
            case D3DPT_TRIANGLESTRIP: mode = GL_TRIANGLE_STRIP; count = PrimitiveCount + 2; break;
            default: mode = GL_TRIANGLES; count = PrimitiveCount * 3; break;
        }
        glDrawElements( mode, count, pIB->indexType, (const void *)( (size_t)StartIndex * pIB->nIndexSize ) );
        return D3D_OK;
    }
};

/* ---- helpers the resources call back into ---------------------------------- */
void DeviceForgetSurface( CSurface *pSurface )
{
    if ( g_pDevice )
        g_pDevice->ForgetSurface( pSurface );
}

void DeviceUploadBuffer( CBufferBase *pBuffer )
{
    if ( pBuffer->dirty.empty() && !pBuffer->bDiscardPending )
        return;
    glBindBuffer( pBuffer->target, pBuffer->nGL );
    if ( pBuffer->bDiscardPending )
    {
        /* D3DLOCK_DISCARD: orphan, so the driver need not wait for in-flight draws */
        glBufferData( pBuffer->target, pBuffer->nSize, 0, GL_DYNAMIC_DRAW );
        pBuffer->bDiscardPending = false;
    }
    for ( size_t i = 0; i < pBuffer->dirty.size(); ++i )
        glBufferSubData( pBuffer->target, pBuffer->dirty[ i ].first, pBuffer->dirty[ i ].second,
                         &pBuffer->shadow[ pBuffer->dirty[ i ].first ] );
    pBuffer->dirty.clear();
    /* the device's array/element bindings are re-established at the next draw */
}

void DeviceReadbackTexture( CTexImage *pImage, int nFace, int nLevel, int x, int y, int w, int h, uint8_t *pDst, int nDstPitch )
{
    if ( !g_pDevice || !pImage->nGL )
        return;
    glBindFramebuffer( GL_READ_FRAMEBUFFER, g_pDevice->nScratchFBO );
    glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, pImage->FaceTarget( nFace ), pImage->nGL, nLevel );
    std::vector< uint8_t > rgba( (size_t)w * h * 4 );
    glPixelStorei( GL_PACK_ALIGNMENT, 1 );
    glReadPixels( x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[ 0 ] );
    for ( int row = 0; row < h; ++row )
        ConvertRowFromRgba8( pDst + (size_t)row * nDstPitch, &rgba[ (size_t)row * w * 4 ], w );
    glFramebufferTexture2D( GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0 );
    g_pDevice->bFramebufferDirty = true;   /* GL_FRAMEBUFFER binding was disturbed */
    CheckGLError( "DeviceReadbackTexture" );
}

}  // namespace

/* ========================================================================== */
/*  4. IDirect3D9                                                              */
/* ========================================================================== */
namespace {

/*  Modes offered to the engine.  The game was designed for 4:3 desktop modes
 *  (-640/-1024/-1280 on its command line); the phone renders at one of these
 *  into the virtual back buffer and Present scales it to the window with a
 *  letterbox.  The window's own size is offered too. */
struct SMode { UINT w, h; };
const SMode STANDARD_MODES[] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 960 }, { 1280, 1024 } };

class CDirect3D : public IDirect3D9
{
public:
    std::vector< SMode > modes;

    CDirect3D()
    {
        for ( size_t i = 0; i < sizeof( STANDARD_MODES ) / sizeof( STANDARD_MODES[ 0 ] ); ++i )
            modes.push_back( STANDARD_MODES[ i ] );
        SMode native = { (UINT)( g_hooks.getWindowWidth ? g_hooks.getWindowWidth() : 1280 ),
                         (UINT)( g_hooks.getWindowHeight ? g_hooks.getWindowHeight() : 720 ) };
        bool bHave = false;
        for ( size_t i = 0; i < modes.size(); ++i )
            if ( modes[ i ].w == native.w && modes[ i ].h == native.h )
                bHave = true;
        if ( !bHave )
            modes.push_back( native );
    }
    virtual HRESULT GetDeviceCaps( UINT, D3DDEVTYPE, D3DCAPS9 *pCaps )
    {
        memset( pCaps, 0, sizeof( *pCaps ) );
        pCaps->DeviceType = D3DDEVTYPE_HAL;
        pCaps->PresentationIntervals = D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_IMMEDIATE;
        pCaps->TextureCaps = D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPCUBEMAP;   /* NPOT allowed: no POW2 flags */
        pCaps->MaxTextureWidth = pCaps->MaxTextureHeight = (DWORD)g_caps.nMaxTextureSize;
        pCaps->MaxAnisotropy = g_caps.bAnisotropy ? 16 : 1;
        pCaps->MaxTextureBlendStages = 8;
        pCaps->MaxSimultaneousTextures = 8;
        pCaps->MaxVertexIndex = 0x00FFFFFF;
        pCaps->MaxPrimitiveCount = 0x000FFFFF;
        pCaps->MaxStreams = 1;
        pCaps->MaxStreamStride = 256;
        pCaps->VertexShaderVersion = D3DVS_VERSION( 1, 1 );
        pCaps->MaxVertexShaderConst = 96;
        pCaps->PixelShaderVersion = D3DPS_VERSION( 1, 4 );
        pCaps->PixelShader1xMaxValue = 1.0f;
        return D3D_OK;
    }
    virtual HRESULT GetAdapterIdentifier( UINT, DWORD, D3DADAPTER_IDENTIFIER9 *pIdentifier )
    {
        memset( pIdentifier, 0, sizeof( *pIdentifier ) );
        snprintf( pIdentifier->Driver, sizeof( pIdentifier->Driver ), "d3d9gles" );
        snprintf( pIdentifier->Description, sizeof( pIdentifier->Description ), "%s",
                  (const char *)( glGetString( GL_RENDERER ) ? glGetString( GL_RENDERER ) : (const GLubyte *)"GLES" ) );
        pIdentifier->VendorId = 0;   /* not NVIDIA: the engine's NP2 workaround stays off */
        return D3D_OK;
    }
    virtual HRESULT GetAdapterDisplayMode( UINT, D3DDISPLAYMODE *pMode )
    {
        /* "the desktop": the virtual back buffer if there is one, else the window */
        if ( g_pDevice && g_pDevice->pp.BackBufferWidth )
        {
            pMode->Width = g_pDevice->pp.BackBufferWidth;
            pMode->Height = g_pDevice->pp.BackBufferHeight;
        }
        else
        {
            pMode->Width = g_hooks.getWindowWidth ? g_hooks.getWindowWidth() : 1280;
            pMode->Height = g_hooks.getWindowHeight ? g_hooks.getWindowHeight() : 720;
        }
        pMode->RefreshRate = 60;
        pMode->Format = D3DFMT_X8R8G8B8;
        return D3D_OK;
    }
    virtual UINT GetAdapterModeCount( UINT, D3DFORMAT Format )
    {
        return Format == D3DFMT_X8R8G8B8 ? (UINT)modes.size() : 0;
    }
    virtual HRESULT EnumAdapterModes( UINT, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode )
    {
        if ( Format != D3DFMT_X8R8G8B8 || Mode >= modes.size() )
            return D3DERR_INVALIDCALL;
        pMode->Width = modes[ Mode ].w;
        pMode->Height = modes[ Mode ].h;
        pMode->RefreshRate = 60;
        pMode->Format = D3DFMT_X8R8G8B8;
        return D3D_OK;
    }
    virtual HRESULT CheckDeviceFormat( UINT, D3DDEVTYPE, D3DFORMAT, DWORD Usage, D3DRESOURCETYPE, D3DFORMAT CheckFormat )
    {
        const SFormatInfo fi = DescribeFormat( CheckFormat );
        if ( Usage & D3DUSAGE_DEPTHSTENCIL )
            return ( CheckFormat == D3DFMT_D24S8 || CheckFormat == D3DFMT_D16 ) ? D3D_OK : D3DERR_NOTAVAILABLE;
        if ( Usage & D3DUSAGE_RENDERTARGET )
            return ( CheckFormat == D3DFMT_A8R8G8B8 || CheckFormat == D3DFMT_X8R8G8B8 ) ? D3D_OK : D3DERR_NOTAVAILABLE;
        return ( fi.cls == FC_COLOR || fi.cls == FC_DXT ) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    virtual HRESULT CheckDepthStencilMatch( UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT DepthStencilFormat )
    {
        return ( DepthStencilFormat == D3DFMT_D24S8 || DepthStencilFormat == D3DFMT_D16 ) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    virtual HRESULT CreateDevice( UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *pParams, IDirect3DDevice9 **ppDevice )
    {
        CheckGLCaps();
        CDevice *p = new CDevice;
        g_pDevice = p;
        p->CreateBackBuffer( pParams );
        *ppDevice = p;
        return D3D_OK;
    }
};

}  // namespace

void A5D3DGetFrameStats( A5D3DFrameStats *pOut, int bReset )
{
    *pOut = g_stats;
    if ( bReset )
    {
        const int nPresents = g_stats.nPresents;
        memset( &g_stats, 0, sizeof( g_stats ) );
        g_stats.nPresents = nPresents;
    }
}

IDirect3D9 *Direct3DCreate9( UINT )
{
    CheckGLCaps();
    return new CDirect3D;
}

/* ========================================================================== */
/*  5. Platform hooks                                                          */
/* ========================================================================== */
void A5D3DSetPlatformHooks( const A5D3DPlatformHooks *pHooks )
{
    if ( pHooks )
        g_hooks = *pHooks;
}

int A5D3DWindowToBackBuffer( float fWindowX, float fWindowY, float *pfBackX, float *pfBackY )
{
    if ( !g_pDevice || !g_pDevice->pp.BackBufferWidth )
        return 0;
    const int nWinW = g_hooks.getWindowWidth ? g_hooks.getWindowWidth() : (int)g_pDevice->pp.BackBufferWidth;
    const int nWinH = g_hooks.getWindowHeight ? g_hooks.getWindowHeight() : (int)g_pDevice->pp.BackBufferHeight;
    const UINT bw = g_pDevice->pp.BackBufferWidth, bh = g_pDevice->pp.BackBufferHeight;
    int dw = nWinW, dh = (int)( (long long)nWinW * bh / bw );
    if ( dh > nWinH ) { dh = nWinH; dw = (int)( (long long)nWinH * bw / bh ); }
    const int dx = ( nWinW - dw ) / 2, dy = ( nWinH - dh ) / 2;
    if ( fWindowX < dx || fWindowY < dy || fWindowX >= dx + dw || fWindowY >= dy + dh )
        return 0;
    if ( pfBackX ) *pfBackX = ( fWindowX - dx ) * (float)bw / (float)dw;
    if ( pfBackY ) *pfBackY = ( fWindowY - dy ) * (float)bh / (float)dh;
    return 1;
}
