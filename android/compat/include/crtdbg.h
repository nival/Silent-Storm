/* crtdbg.h -- MSVC debug-heap header.  Android has no debug CRT; the macros the
 * engine uses degrade to no-ops (leak tracking is handled by ASan instead). */
#ifndef A5_COMPAT_CRTDBG_H
#define A5_COMPAT_CRTDBG_H

#define _CRTDBG_ALLOC_MEM_DF        0x01
#define _CRTDBG_LEAK_CHECK_DF       0x20
#define _CRTDBG_REPORT_FLAG        (-1)
#define _CRT_WARN   0
#define _CRT_ERROR  1
#define _CRT_ASSERT 2

#define _CrtSetDbgFlag(f)               ((int)(f))
#define _CrtCheckMemory()               (1)
#define _CrtDumpMemoryLeaks()           (0)
#define _CrtSetReportMode(t,m)          (0)
#define _CrtSetReportFile(t,f)          ((void*)0)
#define _CrtSetBreakAlloc(a)            (0)
#define _ASSERT(expr)                   ((void)0)
#define _ASSERTE(expr)                  ((void)0)
#define _RPT0(t,m)                      ((void)0)
#define _RPT1(t,m,a)                    ((void)0)
#define _RPT2(t,m,a,b)                  ((void)0)

#endif
