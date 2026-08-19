/* direct.h -- MSVC directory functions; POSIX spellings live in <unistd.h>. */
#ifndef A5_COMPAT_DIRECT_H
#define A5_COMPAT_DIRECT_H
#include <unistd.h>
#include <sys/stat.h>
#include "a5_msvc_compat.h"
#define _getcwd getcwd
#define _chdir  chdir
#define _rmdir  rmdir
#endif
