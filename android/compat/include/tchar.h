/* tchar.h -- the engine is an ANSI build, so TCHAR is char throughout. */
#ifndef A5_COMPAT_TCHAR_H
#define A5_COMPAT_TCHAR_H
#include <string.h>
#include "a5_msvc_compat.h"
#define _T(x)      x
#define TEXT(x)    x
#define _tcslen    strlen
#define _tcscpy    strcpy
#define _tcscmp    strcmp
#define _tcsicmp   strcasecmp
#define _stprintf  sprintf
#endif
