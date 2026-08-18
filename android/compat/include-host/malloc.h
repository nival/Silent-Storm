/*
 *  malloc.h -- macOS has no top-level <malloc.h>, but MSVC, glibc and bionic all
 *  do, and the engine includes it.  This shim exists only for the host build: it
 *  lives in compat/include-host, which is on the include path only when NOT
 *  building for Android.  (Shadowing <malloc.h> on Android creates an include
 *  cycle with bionic's <stdlib.h> and breaks ldiv_t/lldiv_t.)
 */
#ifndef A5_COMPAT_MALLOC_H
#define A5_COMPAT_MALLOC_H

#include <stdlib.h>

#if defined( __APPLE__ )
#  include <malloc/malloc.h>
#  include <alloca.h>
#else
#  include_next <malloc.h>
#endif

#endif
