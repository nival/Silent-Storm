/*
 *  a5_msvc_compat.h -- MSVC 6/7 CRT compatibility for the Android (clang/libc++) build.
 *
 *  This header is force-included (-include) into every original engine translation
 *  unit by the CMake build, so the 2003 sources can keep calling the Microsoft CRT
 *  spellings they were written against.  It contains no game logic: only the
 *  vocabulary the old compiler provided for free.
 */
#ifndef A5_MSVC_COMPAT_H
#define A5_MSVC_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>

/* ----- MSVC keywords ------------------------------------------------------ */
#ifndef _MSC_VER
#  define __cdecl
#  define __stdcall
#  define __fastcall
#  define __forceinline inline
#  ifndef __declspec
#    define __declspec(x)
#  endif
#  define __int8   char
#  define __int16  short
#  define __int32  int
#  define __int64  long long
#endif

/* MSVC's "extern __declspec(dllimport)" marker used across the engine headers. */
#ifndef externA5
#  define externA5 extern
#endif

/* ----- RTTI: dynamic_cast from a pointer of unknown static type ------------ */
#ifdef __cplusplus
#include <typeinfo>
/*  p points somewhere into a polymorphic object (its complete address or any
 *  vptr-bearing subobject); returns the `dst` subobject or 0.  See
 *  compat/src/rtti_compat.cpp for why the engine needs this. */
void *a5_dynamic_cast_from_opaque( const void *p, const std::type_info &dst );
template< class T >
inline T *a5_cast_opaque( const void *p ) { return static_cast< T * >( a5_dynamic_cast_from_opaque( p, typeid( T ) ) ); }
#endif

/* ----- CRT spelling differences ------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/* Case-insensitive compares: POSIX spells these without the underscore. */
#ifndef stricmp
#  define stricmp  strcasecmp
#endif
#ifndef _stricmp
#  define _stricmp strcasecmp
#endif
#ifndef strnicmp
#  define strnicmp  strncasecmp
#endif
#ifndef _strnicmp
#  define _strnicmp strncasecmp
#endif
#ifndef memicmp
#  define memicmp  a5_memicmp
#  define _memicmp a5_memicmp
int a5_memicmp( const void *a, const void *b, size_t n );
#endif

/* Integer -> string.  itoa() is not in POSIX. */
char *a5_itoa( int value, char *buffer, int radix );
char *a5_ltoa( long value, char *buffer, int radix );
char *a5_ultoa( unsigned long value, char *buffer, int radix );
char *a5_i64toa( long long value, char *buffer, int radix );
#ifndef itoa
#  define itoa   a5_itoa
#  define _itoa  a5_itoa
#endif
#ifndef ltoa
#  define ltoa   a5_ltoa
#  define _ltoa  a5_ltoa
#endif
#ifndef ultoa
#  define ultoa  a5_ultoa
#  define _ultoa a5_ultoa
#endif
#ifndef _i64toa
#  define _i64toa a5_i64toa
#endif

/* In-place case conversion of a whole string. */
char *a5_strupr( char *s );
char *a5_strlwr( char *s );
#ifndef strupr
#  define strupr  a5_strupr
#  define _strupr a5_strupr
#endif
#ifndef strlwr
#  define strlwr  a5_strlwr
#  define _strlwr a5_strlwr
#endif

/* Path splitting helpers from <stdlib.h> on Windows. */
void a5_splitpath( const char *path, char *drive, char *dir, char *fname, char *ext );
void a5_makepath( char *path, const char *drive, const char *dir, const char *fname, const char *ext );
char *a5_fullpath( char *absPath, const char *relPath, size_t maxLength );
#ifndef _splitpath
#  define _splitpath a5_splitpath
#  define _makepath  a5_makepath
#  define _fullpath  a5_fullpath
#endif

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* printf family: the underscored MSVC names are the standard ones here. */
#ifndef _snprintf
#  define _snprintf  snprintf
#endif
#ifndef _vsnprintf
#  define _vsnprintf vsnprintf
#endif

/* File-mode / access helpers. */
#ifndef _access
#  define _access  access
#endif
#ifndef _unlink
#  define _unlink  unlink
#endif
#ifndef _mkdir
#  define _mkdir(p) mkdir((p), 0777)
#endif

/* Floating point classification. */
#ifndef _finite
#  define _finite(x) isfinite(x)
#endif
#ifndef _isnan
#  define _isnan(x)  isnan(x)
#endif

/* MSVC's alloca lives in <malloc.h>. */
#ifndef _alloca
#  define _alloca(n) __builtin_alloca(n)
#endif

/* ----- Wide characters ----------------------------------------------------
 *
 *  The engine's wide strings are UTF-16 (see compat/src/wide_char.cpp).  The
 *  staged sources use char16_t, so the wide CRT functions they call need
 *  char16_t overloads -- libc only provides the wchar_t ones.  These are
 *  overloads rather than macros, so std::swprintf and the C99 forms keep
 *  working unchanged.
 *
 *  A5_WIDE_FORMAT_LIMIT bounds the formatting wrappers, which MSVC's pre-C99
 *  signatures give no size for.  Every ported call site formats a few
 *  characters into a buffer of at least 32; new code should call snprintf and
 *  convert, rather than relying on this.
 */
#define A5_WIDE_FORMAT_LIMIT 1024

#ifdef __cplusplus
#include <stdarg.h>
#include <stddef.h>

extern "C" {
long      a5_u16_atol( const char16_t *psz );
long      a5_u16_strtol( const char16_t *psz, char16_t **ppEnd, int nRadix );
/*  swscanf( s, "%d%Ns", &n, buf ): parses an integer then up to nMaxSuffix
 *  non-space characters into pSuffix (NUL-terminated).  Returns the number of
 *  items assigned (0, 1 or 2), like scanf. */
int       a5_u16_scan_int_and_suffix( const char16_t *psz, int *pnValue, char16_t *pSuffix, int nMaxSuffix );
size_t    a5_u16len( const char16_t *psz );
/*  UTF-16 -> UTF-8 into pDest (nMax bytes, always NUL-terminated); returns pDest.
 *  For logging text through a narrow printf ("%S" meant wchar_t on MSVC). */
char     *a5_u16_to_utf8( const char16_t *pSrc, char *pDest, size_t nMax );
char16_t *a5_u16cpy( char16_t *pDest, const char16_t *pSrc );
char16_t *a5_u16cat( char16_t *pDest, const char16_t *pSrc );
int       a5_u16cmp( const char16_t *a, const char16_t *b );
int       a5_u16_sprintf( char16_t *pBuffer, const char16_t *pszFormat, ... );
int       a5_u16_vsprintf( char16_t *pBuffer, const char16_t *pszFormat, va_list args );
char16_t *a5_u16_itoa( int nValue, char16_t *pBuffer, int nRadix );
double    a5_u16_atof( const char16_t *psz );
int       a5_u16_atoi( const char16_t *psz );
}

inline size_t    wcslen( const char16_t *psz )                          { return a5_u16len( psz ); }
inline char16_t *wcscpy( char16_t *pDest, const char16_t *pSrc )        { return a5_u16cpy( pDest, pSrc ); }
inline char16_t *wcscat( char16_t *pDest, const char16_t *pSrc )        { return a5_u16cat( pDest, pSrc ); }
inline int       wcscmp( const char16_t *a, const char16_t *b )         { return a5_u16cmp( a, b ); }
inline const char16_t *wcschr( const char16_t *psz, char16_t c )
{
    for ( ; *psz; ++psz )
        if ( *psz == c )
            return psz;
    return c == 0 ? psz : 0;
}
inline long      wcstol( const char16_t *psz, char16_t **ppEnd, int nRadix ) { return a5_u16_strtol( psz, ppEnd, nRadix ); }
inline int       vswprintf( char16_t *pBuffer, const char16_t *pszFormat, va_list args )
                                                                        { return a5_u16_vsprintf( pBuffer, pszFormat, args ); }
inline int       swprintf( char16_t *pBuffer, const char16_t *pszFormat, ... )
{
    va_list args;
    va_start( args, pszFormat );
    const int nResult = a5_u16_vsprintf( pBuffer, pszFormat, args );
    va_end( args );
    return nResult;
}

#ifndef _itow
#  define _itow a5_u16_itoa
#  define _wtof a5_u16_atof
#  define _wtoi a5_u16_atoi
#  define _wtol a5_u16_atol
#endif
#endif /* __cplusplus */

/* ----- Name collisions with POSIX -----------------------------------------
 *
 *  The engine has a global `CRandomGenerator random;` (Misc/RandomGen.h), which
 *  collides with POSIX random(3) declared in <stdlib.h>.  Renaming the symbol
 *  here -- after the system headers above have been read -- keeps every engine
 *  translation unit consistent without editing the ~40 files that use it.
 *  Nothing in the port calls POSIX random(); the engine's own ISAAC generator
 *  is the RNG.
 */
#define random a5_engine_random

/* ----- Debugger hooks ------------------------------------------------------ */
#ifdef __cplusplus
extern "C" {
#endif
/* Traps into the debugger when one is attached, otherwise logs and continues. */
void a5_debugbreak( const char *file, int line );
#ifdef __cplusplus
}
#endif
#ifndef __debugbreak
#  define __debugbreak() a5_debugbreak( __FILE__, __LINE__ )
#endif

#endif /* A5_MSVC_COMPAT_H */
