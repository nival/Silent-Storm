/*
 *  wide_char.cpp -- UTF-16 string support for the ported engine.
 *
 *  The engine's wide strings are 16-bit: std::wstring on Win32 is UTF-16, and
 *  the on-disk format depends on that width (CStructureSaver::DataChunkString
 *  writes `str.size() * 2` bytes and reads `nLength / 2` characters).  Android's
 *  wchar_t is 32-bit, so the staged sources are rewritten to use char16_t and
 *  std::u16string -- see the "wide characters" pass in tools/prepare_sources.py.
 *
 *  That leaves the wide CRT functions, which libc only provides for wchar_t.
 *  This file supplies char16_t equivalents, plus the codepage conversion the
 *  engine performs through MultiByteToWideChar / WideCharToMultiByte.
 *
 *  Codepage note: the original reads `nCodePage = CP_ACP` (Misc/StrProc.cpp),
 *  the ANSI codepage of the machine it runs on -- windows-1251 for the Russian
 *  build the data was authored with, windows-1252 for the western ones.  Android
 *  has no system ANSI codepage, so the port defaults to 1251 and
 *  a5_set_ansi_codepage() selects 1252 for western data.
 */
#include "windows.h"
#include "a5_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

/*  Generated from Python's codecs: bytes([b]).decode("cp1251").  Index 0 is byte
 *  0x80; bytes below 0x80 map to themselves in both codepages.  0xFFFD marks the
 *  byte values a codepage leaves undefined. */
const unsigned short CP1251_TO_UNICODE[ 128 ] = {
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D, 0x041E, 0x041F,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A, 0x042B, 0x042C, 0x042D, 0x042E, 0x042F,
    0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437, 0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F,
    0x0440, 0x0441, 0x0442, 0x0443, 0x0444, 0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F,
};

const unsigned short CP1252_TO_UNICODE[ 128 ] = {
    0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD,
    0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

int g_nAnsiCodePage = 1251;

const unsigned short *ActiveTable( UINT nCodePage )
{
    if ( nCodePage == CP_ACP )
        nCodePage = (UINT)g_nAnsiCodePage;
    return nCodePage == 1252 ? CP1252_TO_UNICODE : CP1251_TO_UNICODE;
}

/*  Reverse lookup.  The table is 128 entries and conversions are not hot, so a
 *  scan is fine and avoids carrying a second table. */
int UnicodeToByte( const unsigned short *pTable, unsigned int nUnicode )
{
    if ( nUnicode < 0x80 )
        return (int)nUnicode;
    for ( int i = 0; i < 128; ++i )
        if ( pTable[ i ] == nUnicode )
            return 0x80 + i;
    return -1;
}

void NarrowAscii( const char16_t *pSrc, char *pDest, size_t nMax )
{
    size_t i = 0;
    for ( ; pSrc && pSrc[ i ] && i + 1 < nMax; ++i )
        pDest[ i ] = pSrc[ i ] < 0x80 ? (char)pSrc[ i ] : '?';
    pDest[ i ] = 0;
}

void WidenAscii( const char *pSrc, char16_t *pDest, size_t nMax )
{
    size_t i = 0;
    for ( ; pSrc[ i ] && i + 1 < nMax; ++i )
        pDest[ i ] = (char16_t)(unsigned char)pSrc[ i ];
    pDest[ i ] = 0;
}

}  // namespace

extern "C" void a5_set_ansi_codepage( int nCodePage )
{
    if ( nCodePage != 1251 && nCodePage != 1252 )
    {
        a5_log( A5_PRIORITY_WARN, "unsupported ANSI codepage %d, keeping %d",
                nCodePage, g_nAnsiCodePage );
        return;
    }
    g_nAnsiCodePage = nCodePage;
}

extern "C" int a5_get_ansi_codepage( void ) { return g_nAnsiCodePage; }

/* ------------------------------------------------------------------------- */
/*  Codepage conversion                                                        */
/* ------------------------------------------------------------------------- */
extern "C" int MultiByteToWideChar( UINT nCodePage, DWORD, LPCSTR pszMultiByte,
                                    int cbMultiByte, LPWSTR pszWideChar, int cchWideChar )
{
    if ( !pszMultiByte )
        return 0;
    if ( cbMultiByte < 0 )
        cbMultiByte = (int)strlen( pszMultiByte ) + 1;

    if ( nCodePage == CP_UTF8 )
    {
        /*  Minimal UTF-8 decoder.  The engine only takes this path for text that
         *  came from us, so malformed input is replaced rather than rejected. */
        int nOut = 0;
        for ( int i = 0; i < cbMultiByte; )
        {
            unsigned char c = (unsigned char)pszMultiByte[ i ];
            unsigned int  nCode;
            int           nExtra;
            if ( c < 0x80 )      { nCode = c;        nExtra = 0; }
            else if ( c < 0xE0 ) { nCode = c & 0x1F; nExtra = 1; }
            else if ( c < 0xF0 ) { nCode = c & 0x0F; nExtra = 2; }
            else                 { nCode = c & 0x07; nExtra = 3; }
            ++i;
            for ( int k = 0; k < nExtra && i < cbMultiByte; ++k, ++i )
                nCode = ( nCode << 6 ) | ( (unsigned char)pszMultiByte[ i ] & 0x3F );
            if ( nCode > 0xFFFF )
                nCode = 0xFFFD;   /* no surrogate pairs: the engine is BMP-only */
            if ( cchWideChar )
            {
                if ( nOut >= cchWideChar )
                    return nOut;
                pszWideChar[ nOut ] = (WCHAR)nCode;
            }
            ++nOut;
        }
        return nOut;
    }

    const unsigned short *pTable = ActiveTable( nCodePage );
    if ( cchWideChar == 0 )
        return cbMultiByte;               /* size query: one wide char per byte */

    const int n = cbMultiByte < cchWideChar ? cbMultiByte : cchWideChar;
    for ( int i = 0; i < n; ++i )
    {
        const unsigned char c = (unsigned char)pszMultiByte[ i ];
        pszWideChar[ i ] = (WCHAR)( c < 0x80 ? c : pTable[ c - 0x80 ] );
    }
    return n;
}

extern "C" int WideCharToMultiByte( UINT nCodePage, DWORD, LPCWSTR pszWideChar,
                                    int cchWideChar, LPSTR pszMultiByte, int cbMultiByte,
                                    LPCSTR pszDefaultChar, LPBOOL pbUsedDefault )
{
    if ( !pszWideChar )
        return 0;
    if ( cchWideChar < 0 )
    {
        cchWideChar = 0;
        while ( pszWideChar[ cchWideChar ] )
            ++cchWideChar;
        ++cchWideChar;
    }

    if ( nCodePage == CP_UTF8 )
    {
        int nOut = 0;
        for ( int i = 0; i < cchWideChar; ++i )
        {
            const unsigned int nCode = (unsigned short)pszWideChar[ i ];
            char encoded[ 3 ];
            int  nBytes;
            if ( nCode < 0x80 )
            {
                encoded[ 0 ] = (char)nCode;
                nBytes = 1;
            }
            else if ( nCode < 0x800 )
            {
                encoded[ 0 ] = (char)( 0xC0 | ( nCode >> 6 ) );
                encoded[ 1 ] = (char)( 0x80 | ( nCode & 0x3F ) );
                nBytes = 2;
            }
            else
            {
                encoded[ 0 ] = (char)( 0xE0 | ( nCode >> 12 ) );
                encoded[ 1 ] = (char)( 0x80 | ( ( nCode >> 6 ) & 0x3F ) );
                encoded[ 2 ] = (char)( 0x80 | ( nCode & 0x3F ) );
                nBytes = 3;
            }
            if ( cbMultiByte )
            {
                if ( nOut + nBytes > cbMultiByte )
                    return nOut;
                memcpy( pszMultiByte + nOut, encoded, nBytes );
            }
            nOut += nBytes;
        }
        return nOut;
    }

    const unsigned short *pTable = ActiveTable( nCodePage );
    if ( cbMultiByte == 0 )
        return cchWideChar;               /* size query: one byte per wide char */

    const char cDefault = pszDefaultChar && pszDefaultChar[ 0 ] ? pszDefaultChar[ 0 ] : '?';
    const int  n = cchWideChar < cbMultiByte ? cchWideChar : cbMultiByte;
    for ( int i = 0; i < n; ++i )
    {
        const int nByte = UnicodeToByte( pTable, (unsigned short)pszWideChar[ i ] );
        if ( nByte < 0 )
        {
            pszMultiByte[ i ] = cDefault;
            if ( pbUsedDefault )
                *pbUsedDefault = TRUE;
        }
        else
            pszMultiByte[ i ] = (char)nByte;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/*  char16_t string functions                                                  */
/* ------------------------------------------------------------------------- */
extern "C" size_t a5_u16len( const char16_t *psz )
{
    size_t n = 0;
    while ( psz && psz[ n ] )
        ++n;
    return n;
}

extern "C" char16_t *a5_u16cpy( char16_t *pDest, const char16_t *pSrc )
{
    char16_t *pOut = pDest;
    while ( ( *pOut++ = *pSrc++ ) != 0 )
        ;
    return pDest;
}

extern "C" char16_t *a5_u16cat( char16_t *pDest, const char16_t *pSrc )
{
    a5_u16cpy( pDest + a5_u16len( pDest ), pSrc );
    return pDest;
}

extern "C" int a5_u16cmp( const char16_t *a, const char16_t *b )
{
    while ( *a && *a == *b )
        ++a, ++b;
    return (int)*a - (int)*b;
}

/*  MSVC's pre-C99 swprintf/vswprintf, on char16_t.  Formatting is done narrow
 *  and widened afterwards, which is exact for the ASCII format strings and
 *  numeric arguments the engine passes.  A %s taking a wide string would not
 *  survive this -- there are no such call sites, and new code should convert
 *  explicitly with NStr::ToAscii. */
extern "C" int a5_u16_vsprintf( char16_t *pBuffer, const char16_t *pszFormat, va_list args )
{
    char szFormat[ A5_WIDE_FORMAT_LIMIT ];
    char szResult[ A5_WIDE_FORMAT_LIMIT ];
    NarrowAscii( pszFormat, szFormat, sizeof( szFormat ) );
    const int nResult = vsnprintf( szResult, sizeof( szResult ), szFormat, args );
    WidenAscii( szResult, pBuffer, A5_WIDE_FORMAT_LIMIT );
    return nResult;
}

extern "C" int a5_u16_sprintf( char16_t *pBuffer, const char16_t *pszFormat, ... )
{
    va_list args;
    va_start( args, pszFormat );
    const int nResult = a5_u16_vsprintf( pBuffer, pszFormat, args );
    va_end( args );
    return nResult;
}

extern "C" char16_t *a5_u16_itoa( int nValue, char16_t *pBuffer, int nRadix )
{
    char szNarrow[ 66 ];
    a5_itoa( nValue, szNarrow, nRadix );
    WidenAscii( szNarrow, pBuffer, sizeof( szNarrow ) );
    return pBuffer;
}

extern "C" double a5_u16_atof( const char16_t *psz )
{
    char szNarrow[ 64 ];
    NarrowAscii( psz, szNarrow, sizeof( szNarrow ) );
    return atof( szNarrow );
}

extern "C" int a5_u16_atoi( const char16_t *psz )
{
    char szNarrow[ 64 ];
    NarrowAscii( psz, szNarrow, sizeof( szNarrow ) );
    return atoi( szNarrow );
}

extern "C" long a5_u16_atol( const char16_t *psz )
{
    char szNarrow[ 64 ];
    NarrowAscii( psz, szNarrow, sizeof( szNarrow ) );
    return atol( szNarrow );
}

extern "C" long a5_u16_strtol( const char16_t *psz, char16_t **ppEnd, int nRadix )
{
    char  szNarrow[ 64 ];
    char *pNarrowEnd = 0;
    NarrowAscii( psz, szNarrow, sizeof( szNarrow ) );
    const long nResult = strtol( szNarrow, &pNarrowEnd, nRadix );
    if ( ppEnd )
        *ppEnd = const_cast< char16_t * >( psz ) + ( pNarrowEnd - szNarrow );
    return nResult;
}
