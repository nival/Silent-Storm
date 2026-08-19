#include "gles_present.h"
#include "text_font.h"

#include <GLES2/gl2.h>
#include <android/log.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>

#define LOG_TAG "SilentStorm"
#define LOGI( ... ) __android_log_print( ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__ )
#define LOGE( ... ) __android_log_print( ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__ )

namespace {

/* One batched vertex: position, texture coordinate, colour. */
struct SVertex
{
    float x, y;
    float u, v;
    float r, g, b, a;
};

std::vector< SVertex > g_vertices;

const char *VERTEX_SHADER =
    "attribute vec2 aPosition;\n"
    "attribute vec2 aTexCoord;\n"
    "attribute vec4 aColour;\n"
    "uniform mat4 uProjection;\n"
    "varying vec2 vTexCoord;\n"
    "varying vec4 vColour;\n"
    "void main() {\n"
    "    vTexCoord = aTexCoord;\n"
    "    vColour = aColour;\n"
    "    gl_Position = uProjection * vec4( aPosition, 0.0, 1.0 );\n"
    "}\n";

const char *FRAGMENT_SHADER =
    "precision mediump float;\n"
    "uniform sampler2D uTexture;\n"
    "varying vec2 vTexCoord;\n"
    "varying vec4 vColour;\n"
    "void main() {\n"
    "    float coverage = texture2D( uTexture, vTexCoord ).a;\n"
    "    gl_FragColor = vec4( vColour.rgb, vColour.a * coverage );\n"
    "}\n";

GLuint CompileShader( GLenum type, const char *pszSource )
{
    GLuint nShader = glCreateShader( type );
    glShaderSource( nShader, 1, &pszSource, 0 );
    glCompileShader( nShader );

    GLint nCompiled = 0;
    glGetShaderiv( nShader, GL_COMPILE_STATUS, &nCompiled );
    if ( !nCompiled )
    {
        char szLog[ 1024 ] = { 0 };
        glGetShaderInfoLog( nShader, sizeof( szLog ), 0, szLog );
        LOGE( "shader compile failed: %s", szLog );
        glDeleteShader( nShader );
        return 0;
    }
    return nShader;
}

/* Colours, dark-terminal palette. */
struct SColour { float r, g, b; };
const SColour COLOUR_OK      = { 0.36f, 0.82f, 0.47f };
const SColour COLOUR_WARN    = { 0.95f, 0.75f, 0.29f };
const SColour COLOUR_FAIL    = { 0.94f, 0.38f, 0.36f };
const SColour COLOUR_HEADING = { 0.55f, 0.76f, 0.98f };
const SColour COLOUR_DETAIL  = { 0.62f, 0.65f, 0.70f };
const SColour COLOUR_TEXT    = { 0.86f, 0.89f, 0.92f };

}  // namespace

CBootConsole::CBootConsole()
    : m_nProgram( 0 ), m_nFontTexture( 0 ), m_nWhiteTexture( 0 ),
      m_nAttribPosition( -1 ), m_nAttribTexCoord( -1 ), m_nAttribColour( -1 ),
      m_nUniformProjection( -1 ), m_nUniformTexture( -1 ),
      m_nBoundTexture( 0 ), m_nVertexCount( 0 ),
      m_nWidth( 0 ), m_nHeight( 0 ), m_fScrollY( 0 ), m_fMaxScroll( 0 ),
      m_bReady( false )
{
    m_szRenderer[ 0 ] = 0;
    m_szVersion[ 0 ] = 0;
}

bool CBootConsole::Init()
{
    GLuint nVertex   = CompileShader( GL_VERTEX_SHADER, VERTEX_SHADER );
    GLuint nFragment = CompileShader( GL_FRAGMENT_SHADER, FRAGMENT_SHADER );
    if ( !nVertex || !nFragment )
        return false;

    m_nProgram = glCreateProgram();
    glAttachShader( m_nProgram, nVertex );
    glAttachShader( m_nProgram, nFragment );
    glLinkProgram( m_nProgram );
    glDeleteShader( nVertex );
    glDeleteShader( nFragment );

    GLint nLinked = 0;
    glGetProgramiv( m_nProgram, GL_LINK_STATUS, &nLinked );
    if ( !nLinked )
    {
        char szLog[ 1024 ] = { 0 };
        glGetProgramInfoLog( m_nProgram, sizeof( szLog ), 0, szLog );
        LOGE( "program link failed: %s", szLog );
        return false;
    }

    m_nAttribPosition    = glGetAttribLocation( m_nProgram, "aPosition" );
    m_nAttribTexCoord    = glGetAttribLocation( m_nProgram, "aTexCoord" );
    m_nAttribColour      = glGetAttribLocation( m_nProgram, "aColour" );
    m_nUniformProjection = glGetUniformLocation( m_nProgram, "uProjection" );
    m_nUniformTexture    = glGetUniformLocation( m_nProgram, "uTexture" );

    /* Font atlas: one byte of coverage per pixel. */
    std::vector< unsigned char > atlas( (size_t)FONT_ATLAS_W * FONT_ATLAS_H );
    FontBuildAtlas( &atlas[ 0 ] );

    glGenTextures( 1, &m_nFontTexture );
    glBindTexture( GL_TEXTURE_2D, m_nFontTexture );
    glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_ALPHA, FONT_ATLAS_W, FONT_ATLAS_H, 0,
                  GL_ALPHA, GL_UNSIGNED_BYTE, &atlas[ 0 ] );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

    /* A 1x1 opaque texel, so solid rectangles use the same shader path. */
    const unsigned char white = 0xFF;
    glGenTextures( 1, &m_nWhiteTexture );
    glBindTexture( GL_TEXTURE_2D, m_nWhiteTexture );
    glTexImage2D( GL_TEXTURE_2D, 0, GL_ALPHA, 1, 1, 0, GL_ALPHA, GL_UNSIGNED_BYTE, &white );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
    glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );

    const char *pszRenderer = (const char *)glGetString( GL_RENDERER );
    const char *pszVersion  = (const char *)glGetString( GL_VERSION );
    snprintf( m_szRenderer, sizeof( m_szRenderer ), "%s", pszRenderer ? pszRenderer : "?" );
    snprintf( m_szVersion, sizeof( m_szVersion ), "%s", pszVersion ? pszVersion : "?" );
    LOGI( "GLES ready: %s / %s", m_szRenderer, m_szVersion );

    glEnable( GL_BLEND );
    glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
    glDisable( GL_DEPTH_TEST );

    m_bReady = true;
    return true;
}

void CBootConsole::Shutdown()
{
    if ( m_nFontTexture )  glDeleteTextures( 1, &m_nFontTexture );
    if ( m_nWhiteTexture ) glDeleteTextures( 1, &m_nWhiteTexture );
    if ( m_nProgram )      glDeleteProgram( m_nProgram );
    m_nFontTexture = m_nWhiteTexture = m_nProgram = 0;
    m_bReady = false;
}

void CBootConsole::SetReport( const SBootReport &report )
{
    m_report = report;
}

void CBootConsole::SetViewport( int nWidth, int nHeight )
{
    m_nWidth  = nWidth;
    m_nHeight = nHeight;
}

void CBootConsole::Scroll( float fPixels )
{
    m_fScrollY += fPixels;
    if ( m_fScrollY < 0 )
        m_fScrollY = 0;
    if ( m_fScrollY > m_fMaxScroll )
        m_fScrollY = m_fMaxScroll;
}

void CBootConsole::DrawRect( float fX, float fY, float fW, float fH,
                             float r, float g, float b, float a )
{
    if ( m_nBoundTexture != m_nWhiteTexture )
    {
        Flush();
        m_nBoundTexture = m_nWhiteTexture;
    }
    const SVertex quad[ 6 ] = {
        { fX,      fY,      0, 0, r, g, b, a },
        { fX + fW, fY,      1, 0, r, g, b, a },
        { fX + fW, fY + fH, 1, 1, r, g, b, a },
        { fX,      fY,      0, 0, r, g, b, a },
        { fX + fW, fY + fH, 1, 1, r, g, b, a },
        { fX,      fY + fH, 0, 1, r, g, b, a },
    };
    for ( int i = 0; i < 6; ++i )
        g_vertices.push_back( quad[ i ] );
}

void CBootConsole::DrawText( const char *pszText, float fX, float fY, float fScale,
                             float r, float g, float b, float a )
{
    if ( m_nBoundTexture != m_nFontTexture )
    {
        Flush();
        m_nBoundTexture = m_nFontTexture;
    }

    const float fCellW = FONT_CELL_W * fScale;
    const float fCellH = FONT_CELL_H * fScale;
    const float fU = 1.0f / FONT_COLUMNS;
    const float fV = 1.0f / FONT_ROWS;

    float fPenX = fX;
    for ( const char *p = pszText; *p; ++p )
    {
        if ( *p == '\n' )
        {
            fPenX = fX;
            fY += fCellH;
            continue;
        }
        int nColumn, nRow;
        FontGetCell( *p, &nColumn, &nRow );

        const float u0 = nColumn * fU, v0 = nRow * fV;
        const float u1 = u0 + fU,      v1 = v0 + fV;

        const SVertex quad[ 6 ] = {
            { fPenX,          fY,          u0, v0, r, g, b, a },
            { fPenX + fCellW, fY,          u1, v0, r, g, b, a },
            { fPenX + fCellW, fY + fCellH, u1, v1, r, g, b, a },
            { fPenX,          fY,          u0, v0, r, g, b, a },
            { fPenX + fCellW, fY + fCellH, u1, v1, r, g, b, a },
            { fPenX,          fY + fCellH, u0, v1, r, g, b, a },
        };
        for ( int i = 0; i < 6; ++i )
            g_vertices.push_back( quad[ i ] );

        fPenX += fCellW;
    }
}

void CBootConsole::Flush()
{
    if ( g_vertices.empty() )
        return;

    glBindTexture( GL_TEXTURE_2D, m_nBoundTexture );
    const SVertex *pBase = &g_vertices[ 0 ];

    glVertexAttribPointer( m_nAttribPosition, 2, GL_FLOAT, GL_FALSE, sizeof( SVertex ),
                           &pBase->x );
    glVertexAttribPointer( m_nAttribTexCoord, 2, GL_FLOAT, GL_FALSE, sizeof( SVertex ),
                           &pBase->u );
    glVertexAttribPointer( m_nAttribColour, 4, GL_FLOAT, GL_FALSE, sizeof( SVertex ),
                           &pBase->r );
    glDrawArrays( GL_TRIANGLES, 0, (GLsizei)g_vertices.size() );

    m_nVertexCount += (int)g_vertices.size();
    g_vertices.clear();
}

void CBootConsole::Render( double fElapsedSeconds )
{
    if ( !m_bReady || m_nWidth <= 0 || m_nHeight <= 0 )
        return;

    glViewport( 0, 0, m_nWidth, m_nHeight );
    glClearColor( 0.055f, 0.063f, 0.078f, 1.0f );
    glClear( GL_COLOR_BUFFER_BIT );

    glUseProgram( m_nProgram );
    glEnableVertexAttribArray( m_nAttribPosition );
    glEnableVertexAttribArray( m_nAttribTexCoord );
    glEnableVertexAttribArray( m_nAttribColour );

    /* Orthographic projection with the origin top-left, in pixels. */
    const float fProjection[ 16 ] = {
        2.0f / m_nWidth, 0, 0, 0,
        0, -2.0f / m_nHeight, 0, 0,
        0, 0, -1, 0,
        -1, 1, 0, 1,
    };
    glUniformMatrix4fv( m_nUniformProjection, 1, GL_FALSE, fProjection );
    glUniform1i( m_nUniformTexture, 0 );
    glActiveTexture( GL_TEXTURE0 );

    g_vertices.clear();
    m_nVertexCount  = 0;
    m_nBoundTexture = m_nWhiteTexture;

    /* Scale so a line of ~54 characters fits the width, clamped for tablets. */
    float fScale = (float)m_nWidth / ( 54.0f * FONT_CELL_W );
    if ( fScale < 1.0f ) fScale = 1.0f;
    if ( fScale > 4.0f ) fScale = 4.0f;

    const float fLineHeight = FONT_CELL_H * fScale * 1.35f;
    const float fMargin     = 12.0f * fScale;

    /* Title bar.  Two text rows plus breathing room above and below; the body
     * starts below this, so it has to account for the larger title scale. */
    const float fTitleHeight = fLineHeight * 1.0f + FONT_CELL_H * fScale
                                                  + FONT_CELL_H * fScale * 0.75f
                                                  + fMargin * 1.2f;
    DrawRect( 0, 0, (float)m_nWidth, fTitleHeight, 0.10f, 0.13f, 0.18f, 1.0f );
    DrawRect( 0, fTitleHeight - 2.0f, (float)m_nWidth, 2.0f, 0.20f, 0.42f, 0.66f, 1.0f );
    const float fTitleY = fMargin * 0.5f;
    DrawText( "SILENT STORM", fMargin, fTitleY, fScale * 1.0f,
              0.92f, 0.94f, 0.96f, 1.0f );

    char szSubtitle[ 160 ];
    snprintf( szSubtitle, sizeof( szSubtitle ), "engine core on Android  -  %d ok, %d failed",
              m_report.nPassed, m_report.nFailed );
    DrawText( szSubtitle, fMargin, fTitleY + FONT_CELL_H * fScale * 1.25f, fScale * 0.75f,
              COLOUR_DETAIL.r, COLOUR_DETAIL.g, COLOUR_DETAIL.b, 1.0f );

    /* Report body. */
    float fY = fTitleHeight + fMargin - m_fScrollY;

    for ( size_t i = 0; i < m_report.lines.size(); ++i )
    {
        const SBootLine &line = m_report.lines[ i ];

        SColour colour = COLOUR_TEXT;
        const char *pszPrefix = "";
        float fIndent = 0;

        switch ( line.status )
        {
            case BOOT_OK:      colour = COLOUR_OK;      pszPrefix = "[ok]   "; break;
            case BOOT_WARN:    colour = COLOUR_WARN;    pszPrefix = "[warn] "; break;
            case BOOT_FAIL:    colour = COLOUR_FAIL;    pszPrefix = "[FAIL] "; break;
            case BOOT_DETAIL:  colour = COLOUR_DETAIL;  pszPrefix = "       "; break;
            case BOOT_HEADING: colour = COLOUR_HEADING; pszPrefix = "";        break;
        }

        if ( line.status == BOOT_HEADING && i > 0 )
            fY += fLineHeight * 0.5f;

        /* Only draw what is on screen. */
        if ( fY > -fLineHeight && fY < m_nHeight )
        {
            char szLine[ 256 ];
            if ( line.fSeconds > 0.0 )
                snprintf( szLine, sizeof( szLine ), "%s%s  (%.1f ms)", pszPrefix,
                          line.szText.c_str(), line.fSeconds * 1000.0 );
            else
                snprintf( szLine, sizeof( szLine ), "%s%s", pszPrefix, line.szText.c_str() );

            if ( line.status == BOOT_HEADING )
                DrawRect( fMargin, fY + fLineHeight * 0.85f,
                          (float)m_nWidth - fMargin * 2.0f, 1.0f,
                          COLOUR_HEADING.r, COLOUR_HEADING.g, COLOUR_HEADING.b, 0.25f );

            DrawText( szLine, fMargin + fIndent, fY, fScale * 0.8f,
                      colour.r, colour.g, colour.b, 1.0f );
        }
        fY += fLineHeight;
    }

    /* Footer: renderer identification and a scroll hint. */
    const float fFooterY = (float)m_nHeight - fLineHeight * 1.4f;
    DrawRect( 0, fFooterY - fMargin * 0.4f, (float)m_nWidth, fLineHeight * 2.0f,
              0.08f, 0.10f, 0.13f, 0.92f );
    char szFooter[ 192 ];
    snprintf( szFooter, sizeof( szFooter ), "GL: %.40s", m_szRenderer );
    DrawText( szFooter, fMargin, fFooterY, fScale * 0.7f,
              COLOUR_DETAIL.r, COLOUR_DETAIL.g, COLOUR_DETAIL.b, 1.0f );

    /* Total content height, for scroll clamping. */
    m_fMaxScroll = fY + m_fScrollY - (float)m_nHeight + fLineHeight * 3.0f;
    if ( m_fMaxScroll < 0 )
        m_fMaxScroll = 0;

    Flush();

    glDisableVertexAttribArray( m_nAttribPosition );
    glDisableVertexAttribArray( m_nAttribTexCoord );
    glDisableVertexAttribArray( m_nAttribColour );

    (void)fElapsedSeconds;
}
