/*
 *  host_main.cpp -- run the engine bring-up checks on the development machine.
 *
 *  The same boot harness the app runs, minus Android: it links the ported
 *  engine modules against the compat layer and points them at a game data
 *  directory on disk.  Useful for debugging engine behaviour without a device,
 *  and for checking the port from CI.
 *
 *      android/build/host/silentstorm_hosttest ../../Complete
 */
#include "boot_harness.h"

#include <stdio.h>
#include <string.h>

int main( int argc, char **argv )
{
    const char *pszDataRoot = argc > 1 ? argv[ 1 ] : "";

    if ( argc > 1 && strcmp( argv[ 1 ], "--help" ) == 0 )
    {
        printf( "usage: %s [game-data-directory]\n", argv[ 0 ] );
        return 0;
    }

    printf( "Silent Storm -- engine core check (host build)\n" );
    printf( "data root: %s\n\n", pszDataRoot[ 0 ] ? pszDataRoot : "(none given)" );

    const SBootReport report = RunBootHarness( "", "", pszDataRoot );

    printf( "\n" );
    for ( size_t i = 0; i < report.lines.size(); ++i )
    {
        const SBootLine &line = report.lines[ i ];
        const char *pszPrefix = "";
        switch ( line.status )
        {
            case BOOT_OK:      pszPrefix = "  ok   "; break;
            case BOOT_WARN:    pszPrefix = "  warn "; break;
            case BOOT_FAIL:    pszPrefix = "  FAIL "; break;
            case BOOT_DETAIL:  pszPrefix = "       "; break;
            case BOOT_HEADING: pszPrefix = "== ";     break;
        }
        if ( line.fSeconds > 0.0 )
            printf( "%s%s (%.1f ms)\n", pszPrefix, line.szText.c_str(), line.fSeconds * 1000.0 );
        else
            printf( "%s%s\n", pszPrefix, line.szText.c_str() );
    }

    printf( "\n%d passed, %d failed, %d warnings\n",
            report.nPassed, report.nFailed, report.nWarnings );
    return report.nFailed ? 1 : 0;
}
