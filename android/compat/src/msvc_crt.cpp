/*
 *  msvc_crt.cpp -- the handful of Microsoft CRT functions that have no POSIX
 *  equivalent, declared in compat/include/a5_msvc_compat.h.
 */
#include "a5_msvc_compat.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

extern "C" int a5_memicmp( const void *a, const void *b, size_t n )
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;
    for ( size_t i = 0; i < n; ++i )
    {
        int d = tolower( p[ i ] ) - tolower( q[ i ] );
        if ( d )
            return d;
    }
    return 0;
}

/* itoa() and friends: MSVC writes into a caller-supplied buffer and supports
 * any radix from 2 to 36.  Negative values are only sign-prefixed for radix 10,
 * matching Microsoft's documented behaviour. */
template < class T, class TUnsigned >
static char *IntegerToString( T value, char *pszBuffer, int nRadix, bool bSigned )
{
    if ( nRadix < 2 || nRadix > 36 )
    {
        pszBuffer[ 0 ] = 0;
        return pszBuffer;
    }

    char *pOut = pszBuffer;
    bool  bNegative = false;
    TUnsigned uValue;

    if ( bSigned && nRadix == 10 && value < 0 )
    {
        bNegative = true;
        uValue = (TUnsigned)( -(T)value );
        *pOut++ = '-';
    }
    else
        uValue = (TUnsigned)value;

    char *pStart = pOut;
    do
    {
        TUnsigned nDigit = uValue % (TUnsigned)nRadix;
        *pOut++ = (char)( nDigit < 10 ? '0' + nDigit : 'a' + nDigit - 10 );
        uValue /= (TUnsigned)nRadix;
    } while ( uValue );
    *pOut = 0;

    for ( char *pLeft = pStart, *pRight = pOut - 1; pLeft < pRight; ++pLeft, --pRight )
    {
        char c = *pLeft;
        *pLeft = *pRight;
        *pRight = c;
    }
    (void)bNegative;
    return pszBuffer;
}

extern "C" char *a5_itoa( int value, char *pszBuffer, int nRadix )
{
    return IntegerToString< int, unsigned int >( value, pszBuffer, nRadix, true );
}

extern "C" char *a5_ltoa( long value, char *pszBuffer, int nRadix )
{
    return IntegerToString< long, unsigned long >( value, pszBuffer, nRadix, true );
}

extern "C" char *a5_ultoa( unsigned long value, char *pszBuffer, int nRadix )
{
    return IntegerToString< unsigned long, unsigned long >( value, pszBuffer, nRadix, false );
}

extern "C" char *a5_i64toa( long long value, char *pszBuffer, int nRadix )
{
    return IntegerToString< long long, unsigned long long >( value, pszBuffer, nRadix, true );
}

extern "C" char *a5_strupr( char *s )
{
    for ( char *p = s; *p; ++p )
        *p = (char)toupper( (unsigned char)*p );
    return s;
}

extern "C" char *a5_strlwr( char *s )
{
    for ( char *p = s; *p; ++p )
        *p = (char)tolower( (unsigned char)*p );
    return s;
}

/* _splitpath / _makepath: DOS-style path decomposition.  Drive letters do not
 * exist here, but engine paths may still carry one, so keep the field. */
extern "C" void a5_splitpath( const char *pszPath, char *pszDrive, char *pszDir,
                              char *pszFileName, char *pszExtension )
{
    if ( pszDrive ) pszDrive[ 0 ] = 0;
    if ( pszDir ) pszDir[ 0 ] = 0;
    if ( pszFileName ) pszFileName[ 0 ] = 0;
    if ( pszExtension ) pszExtension[ 0 ] = 0;
    if ( !pszPath )
        return;

    const char *p = pszPath;
    if ( p[ 0 ] && p[ 1 ] == ':' )
    {
        if ( pszDrive )
        {
            pszDrive[ 0 ] = p[ 0 ];
            pszDrive[ 1 ] = ':';
            pszDrive[ 2 ] = 0;
        }
        p += 2;
    }

    const char *pLastSlash = 0;
    for ( const char *q = p; *q; ++q )
        if ( *q == '/' || *q == '\\' )
            pLastSlash = q;

    const char *pName = p;
    if ( pLastSlash )
    {
        if ( pszDir )
        {
            size_t n = (size_t)( pLastSlash - p + 1 );
            memcpy( pszDir, p, n );
            pszDir[ n ] = 0;
        }
        pName = pLastSlash + 1;
    }

    const char *pDot = strrchr( pName, '.' );
    if ( pDot )
    {
        if ( pszFileName )
        {
            size_t n = (size_t)( pDot - pName );
            memcpy( pszFileName, pName, n );
            pszFileName[ n ] = 0;
        }
        if ( pszExtension )
            strcpy( pszExtension, pDot );
    }
    else if ( pszFileName )
        strcpy( pszFileName, pName );
}

extern "C" void a5_makepath( char *pszPath, const char *pszDrive, const char *pszDir,
                             const char *pszFileName, const char *pszExtension )
{
    pszPath[ 0 ] = 0;
    if ( pszDrive && pszDrive[ 0 ] )
        strcat( pszPath, pszDrive );
    if ( pszDir && pszDir[ 0 ] )
        strcat( pszPath, pszDir );
    if ( pszFileName && pszFileName[ 0 ] )
        strcat( pszPath, pszFileName );
    if ( pszExtension && pszExtension[ 0 ] )
    {
        if ( pszExtension[ 0 ] != '.' )
            strcat( pszPath, "." );
        strcat( pszPath, pszExtension );
    }
}

extern "C" char *a5_fullpath( char *pszAbsolute, const char *pszRelative, size_t nMaxLength )
{
    extern char *a5_resolve_path( const char *, char *, size_t );
    return a5_resolve_path( pszRelative, pszAbsolute, nMaxLength );
}

/* ----- Wide-character formatting ------------------------------------------ */
/*  The swprintf/vswprintf wrappers are inline overloads in a5_msvc_compat.h;
 *  only _itow needs an out-of-line definition. */
extern "C" wchar_t *a5_itow( int nValue, wchar_t *pBuffer, int nRadix )
{
    char szNarrow[ 66 ];
    a5_itoa( nValue, szNarrow, nRadix );
    wchar_t *pOut = pBuffer;
    for ( const char *p = szNarrow; *p; ++p )
        *pOut++ = (wchar_t)(unsigned char)*p;
    *pOut = 0;
    return pBuffer;
}

/*  _wtof / _wtoi: MSVC's wide-character strtod/atoi.  The engine only ever
 *  passes ASCII digits through these (console variable values), so a narrowing
 *  conversion is sufficient and avoids a locale dependency. */
extern "C" double a5_wtof( const wchar_t *psz )
{
    char szNarrow[ 64 ];
    size_t i = 0;
    for ( ; psz && psz[ i ] && i < sizeof( szNarrow ) - 1; ++i )
        szNarrow[ i ] = psz[ i ] < 128 ? (char)psz[ i ] : '?';
    szNarrow[ i ] = 0;
    return atof( szNarrow );
}

extern "C" int a5_wtoi( const wchar_t *psz )
{
    return (int)a5_wtof( psz );
}
