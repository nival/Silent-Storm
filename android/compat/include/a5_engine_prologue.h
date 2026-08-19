/*
 *  a5_engine_prologue.h -- the environment every engine header expects.
 *
 *  Each engine module has its own StdAfx.h (all copies identical) which sets up
 *  windows.h, the STL, `using namespace std`, hash_map and the ASSERT macro
 *  before any engine header is read.  Port code that includes engine headers
 *  directly -- the platform layer -- needs the same prologue, so it includes
 *  this instead of picking one module's StdAfx.h arbitrarily.
 */
#ifndef A5_ENGINE_PROLOGUE_H
#define A5_ENGINE_PROLOGUE_H

#include "windows.h"

#include <algorithm>
#include <list>
#include <map>
#include <string>
#include <typeinfo>
#include <vector>
#include <hash_map>

using namespace std;

/*  The engine's ASSERT compiles to nothing in release builds.  In the port it
 *  logs instead of vanishing: a tripped assertion in ported code is a porting
 *  bug worth seeing in logcat, and unlike the original MessageBox it cannot
 *  block a device with no input focus. */
#ifndef ASSERT
#  include "a5_log.h"
#  define ASSERT( expression )                                                   \
      do {                                                                       \
          if ( !( expression ) )                                                 \
              a5_log( A5_PRIORITY_WARN, "ASSERT(%s) failed at %s(%d)",           \
                      #expression, __FILE__, __LINE__ );                         \
      } while ( 0 )
#endif

/*  Basic2.h and Tools.h are the foundation every other engine header assumes
 *  has already been read (int64, DWORD helpers, CObjectBase, CPtr<>). */
#include "Misc/Tools.h"
#include "Misc/Basic2.h"

#endif
