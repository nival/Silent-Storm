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

/* ----- Wide-character formatting ------------------------------------------
 *
 *  MSVC's swprintf/vswprintf predate the C99 signatures and take no buffer
 *  size.  These are *overloads* rather than macros, so std::swprintf and the
 *  C99 form keep working: an MSVC-style call passes a const wchar_t* where the
 *  C99 form wants a size_t, so overload resolution picks the wrapper.
 *
 *  A5_WIDE_FORMAT_LIMIT is the bound the wrappers supply.  Every ported call
 *  site formats a few characters into a buffer of at least 32 wide chars, so it
 *  never binds -- but that also means these are not safe for arbitrary new
 *  code, which should call swprintf with an explicit size.
 */
#define A5_WIDE_FORMAT_LIMIT 1024

#ifdef __cplusplus
#include <wchar.h>
#include <stdarg.h>

inline int vswprintf( wchar_t *pBuffer, const wchar_t *pszFormat, va_list args )
{
    return vswprintf( pBuffer, A5_WIDE_FORMAT_LIMIT, pszFormat, args );
}

inline int swprintf( wchar_t *pBuffer, const wchar_t *pszFormat, ... )
{
    va_list args;
    va_start( args, pszFormat );
    int nResult = vswprintf( pBuffer, A5_WIDE_FORMAT_LIMIT, pszFormat, args );
    va_end( args );
    return nResult;
}

extern "C" wchar_t *a5_itow( int nValue, wchar_t *pBuffer, int nRadix );
extern "C" double   a5_wtof( const wchar_t *psz );
extern "C" int      a5_wtoi( const wchar_t *psz );
#ifndef _wtof
#  define _wtof a5_wtof
#  define _wtoi a5_wtoi
#endif
#ifndef _itow
#  define _itow a5_itow
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
