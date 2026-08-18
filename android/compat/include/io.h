/* io.h -- MSVC low-level I/O declarations; POSIX puts these in <unistd.h>. */
#ifndef A5_COMPAT_IO_H
#define A5_COMPAT_IO_H
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include "a5_msvc_compat.h"

/*  MSVC's directory search API (_findfirst/_findnext/_findclose).  Implemented
 *  over the compat FindFirstFile family in win32compat.cpp, so backslashes and
 *  case-insensitive names work the same way here. */
#define _A_NORMAL 0x00
#define _A_RDONLY 0x01
#define _A_HIDDEN 0x02
#define _A_SYSTEM 0x04
#define _A_SUBDIR 0x10
#define _A_ARCH   0x20

struct _finddata_t
{
    unsigned attrib;
    time_t   time_create;
    time_t   time_access;
    time_t   time_write;
    unsigned long size;
    char     name[ 260 ];
};

#ifdef __cplusplus
extern "C" {
#endif
intptr_t _findfirst( const char *pszFileSpec, struct _finddata_t *pFileInfo );
int      _findnext( intptr_t hFile, struct _finddata_t *pFileInfo );
int      _findclose( intptr_t hFile );
#ifdef __cplusplus
}
#endif
#endif
