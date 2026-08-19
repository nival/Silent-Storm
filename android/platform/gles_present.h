/*
 *  gles_present.h -- the boot console's GLES 2.0 presenter.
 *
 *  The engine's own renderer is Direct3D 8/9 and is not ported yet (see
 *  docs/PORTING.md).  This is the minimum that stands in its place: enough GLES
 *  to prove a context comes up, to show the boot report on screen, and to give
 *  the eventual GLES backend a working EGL surface to build on.
 */
#ifndef A5_GLES_PRESENT_H
#define A5_GLES_PRESENT_H

#include "boot_harness.h"

class CBootConsole
{
public:
    CBootConsole();

    bool Init();                       /* needs a current EGL context */
    void Shutdown();
    bool IsReady() const { return m_bReady; }

    void SetReport( const SBootReport &report );
    void SetViewport( int nWidth, int nHeight );
    void Scroll( float fPixels );
    void Render( double fElapsedSeconds );

    /* Renderer identification, filled in by Init() from glGetString. */
    const char *GetRendererName() const { return m_szRenderer; }
    const char *GetGlVersion() const    { return m_szVersion; }

private:
    void DrawText( const char *pszText, float fX, float fY, float fScale,
                   float r, float g, float b, float a );
    void DrawRect( float fX, float fY, float fW, float fH,
                   float r, float g, float b, float a );
    void Flush();

    unsigned m_nProgram;
    unsigned m_nFontTexture;
    unsigned m_nWhiteTexture;
    int      m_nAttribPosition;
    int      m_nAttribTexCoord;
    int      m_nAttribColour;
    int      m_nUniformProjection;
    int      m_nUniformTexture;

    unsigned m_nBoundTexture;
    int      m_nVertexCount;

    int      m_nWidth;
    int      m_nHeight;
    float    m_fScrollY;
    float    m_fMaxScroll;
    bool     m_bReady;

    char        m_szRenderer[ 128 ];
    char        m_szVersion[ 128 ];
    SBootReport m_report;
};

#endif
