/*  d3d_selftest.h -- exercises compat/d3d9gles on the live GL context.  Called
 *  from the boot harness while a context is current; the results go into the
 *  same report.  What it proves: device + back buffer creation, that every one
 *  of the engine's 155 shaders compiles on this GPU from its real bytecode,
 *  that a (vs,ps) pair links and draws, and that render targets read back with
 *  the D3D memory layout the engine expects. */
#ifndef A5_D3D_SELFTEST_H
#define A5_D3D_SELFTEST_H
#include "boot_harness.h"
class CReportSink;   /* opaque: the harness passes its reporter */
void RunD3DSelfTest( void *pReporter,
                     void ( *pfnAdd )( void *pReporter, EBootStatus status, double fSeconds, const char *pszText ) );
#endif
