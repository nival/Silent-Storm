/*
 *  boot_harness.h -- brings up the ported engine subsystems and reports what
 *  worked.
 *
 *  This is the port's proof-of-life and its regression check in one: it drives
 *  the *original* engine code (Misc, FileIO, Script) against real game data on
 *  the device, and every step it performs is a step the full game will need.
 *  Each result is both logged (adb logcat -s SilentStorm) and drawn on screen.
 */
#ifndef A5_BOOT_HARNESS_H
#define A5_BOOT_HARNESS_H

#include <string>
#include <vector>

enum EBootStatus
{
    BOOT_OK,        /* the step did what it was supposed to           */
    BOOT_WARN,      /* nothing broke, but there was nothing to do     */
    BOOT_FAIL,      /* the step failed -- the port is not working     */
    BOOT_HEADING,   /* section label, not a step                      */
    BOOT_DETAIL,    /* supporting line under a step                   */
};

struct SBootLine
{
    EBootStatus status;
    std::string szText;
    double      fSeconds;    /* 0 when not timed */
};

struct SBootReport
{
    std::vector< SBootLine > lines;
    int                      nPassed;
    int                      nFailed;
    int                      nWarnings;
    bool                     bDataMounted;
    std::string              szDataRoot;
};

/*  Runs every check once.  Safe to call again (it is stateless apart from the
 *  engine singletons the modules themselves own). */
SBootReport RunBootHarness( const char *pszExternalFilesDir,
                            const char *pszInternalFilesDir,
                            const char *pszExplicitDataRoot = 0 );

#endif
