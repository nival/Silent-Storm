#include "data_mount.h"

#include "windows.h"          /* compat layer: a5_set_data_root */

#include "a5_log.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

namespace {

/*  Directories the retail game ships (see Complete/ in the repository).  Their
 *  presence is what tells us a candidate directory really holds game data
 *  rather than being an empty app folder. */
const char *ASSET_DIRECTORIES[] = {
    "Textures", "Geometries", "Animations", "Sounds", "Buildings", "Terrain",
    "Globals", "Chapters", "Scripts", "Units", "Skeletons", "Fonts", "Effects",
};

/*  The .res packages the engine mounts as resource directories.  Note that
 *  Complete/regs.res is deliberately absent: despite the extension it is not a
 *  FilesPackage (its signature is 0x019CE23C, not 0x96948A22) and belongs to
 *  the multiplayer registration tooling, not the resource system. */
const char *PACKAGE_FILES[] = {
    "Globals.res", "Chapters.res", "Terrain.res", "Buildings.res",
    "Waypoints.res", "Effects.res",
};

bool DirectoryExists( const std::string &szPath )
{
    struct stat st;
    return stat( szPath.c_str(), &st ) == 0 && S_ISDIR( st.st_mode );
}

bool FileExists( const std::string &szPath )
{
    struct stat st;
    return stat( szPath.c_str(), &st ) == 0 && S_ISREG( st.st_mode );
}

int CountMarkers( const std::string &szRoot )
{
    int nFound = 0;
    for ( size_t i = 0; i < sizeof( ASSET_DIRECTORIES ) / sizeof( ASSET_DIRECTORIES[ 0 ] ); ++i )
        if ( DirectoryExists( szRoot + "/" + ASSET_DIRECTORIES[ i ] ) )
            ++nFound;
    for ( size_t i = 0; i < sizeof( PACKAGE_FILES ) / sizeof( PACKAGE_FILES[ 0 ] ); ++i )
        if ( FileExists( szRoot + "/" + PACKAGE_FILES[ i ] ) )
            ++nFound;
    if ( FileExists( szRoot + "/game.db" ) )
        ++nFound;
    return nFound;
}

/*  When a directory exists but holds nothing we recognise, log what is actually
 *  in it.  The usual causes are data copied one level too deep and a directory
 *  the app cannot read, and the two look identical without this. */
void LogDirectoryContents( const std::string &szPath )
{
    DIR *pDir = opendir( szPath.c_str() );
    if ( !pDir )
    {
        a5_log( A5_PRIORITY_INFO, "       %s: cannot list (%s)",
                szPath.c_str(), strerror( errno ) );
        return;
    }
    std::string szEntries;
    int nCount = 0;
    struct dirent *pEntry;
    while ( ( pEntry = readdir( pDir ) ) != 0 )
    {
        if ( pEntry->d_name[ 0 ] == '.' )
            continue;
        if ( nCount < 8 )
        {
            if ( !szEntries.empty() )
                szEntries += " ";
            szEntries += pEntry->d_name;
        }
        ++nCount;
    }
    closedir( pDir );
    a5_log( A5_PRIORITY_INFO, "       %s: %d entries [%s]",
            szPath.c_str(), nCount, szEntries.c_str() );
}

void AddCandidate( std::vector< SDataRootCandidate > *pOut,
                   const std::string &szPath, const char *pszWhy )
{
    if ( szPath.empty() )
        return;
    SDataRootCandidate candidate;
    candidate.szPath        = szPath;
    candidate.szWhy         = pszWhy;
    candidate.bExists       = DirectoryExists( szPath );
    candidate.nMarkersFound = candidate.bExists ? CountMarkers( szPath ) : 0;
    if ( candidate.bExists && candidate.nMarkersFound == 0 )
        LogDirectoryContents( szPath );
    pOut->push_back( candidate );
}

}  // namespace

SDataMountResult MountGameData( const char *pszExternalFilesDir,
                                const char *pszInternalFilesDir,
                                const char *pszExplicitRoot )
{
    SDataMountResult result;
    result.bMounted   = false;
    result.bHasGameDb = false;

    const std::string szExternal = pszExternalFilesDir ? pszExternalFilesDir : "";
    const std::string szInternal = pszInternalFilesDir ? pszInternalFilesDir : "";

    /* An explicit root always wins: the host harness passes the repository's
     * Complete/ directory this way, and it gives a device build somewhere to
     * plug a user-chosen folder in later. */
    if ( pszExplicitRoot && pszExplicitRoot[ 0 ] )
        AddCandidate( &result.candidates, pszExplicitRoot, "explicitly requested" );

    /* Ordered best-first.  The app-private external directory is the one users
     * can write to over adb/MTP without any runtime permission, so it is both
     * the documented location and the first we try. */
    if ( !szExternal.empty() )
    {
        AddCandidate( &result.candidates, szExternal + "/SilentStorm",
                      "app external files dir, recommended location" );
        AddCandidate( &result.candidates, szExternal,
                      "app external files dir root" );
    }
    if ( !szInternal.empty() )
        AddCandidate( &result.candidates, szInternal + "/SilentStorm",
                      "app internal storage" );
    AddCandidate( &result.candidates, "/sdcard/SilentStorm",
                  "shared storage (needs storage permission)" );
    AddCandidate( &result.candidates, "/data/local/tmp/SilentStorm",
                  "adb push target, for development" );

    /* Pick the candidate with the most game-data markers. */
    const SDataRootCandidate *pBest = 0;
    for ( size_t i = 0; i < result.candidates.size(); ++i )
        if ( result.candidates[ i ].nMarkersFound > 0 &&
             ( !pBest || result.candidates[ i ].nMarkersFound > pBest->nMarkersFound ) )
            pBest = &result.candidates[ i ];

    if ( !pBest )
        return result;

    result.bMounted = true;
    result.szRoot   = pBest->szPath;
    a5_set_data_root( result.szRoot.c_str() );

    for ( size_t i = 0; i < sizeof( PACKAGE_FILES ) / sizeof( PACKAGE_FILES[ 0 ] ); ++i )
        if ( FileExists( result.szRoot + "/" + PACKAGE_FILES[ i ] ) )
            result.packagesFound.push_back( PACKAGE_FILES[ i ] );

    for ( size_t i = 0; i < sizeof( ASSET_DIRECTORIES ) / sizeof( ASSET_DIRECTORIES[ 0 ] ); ++i )
        if ( DirectoryExists( result.szRoot + "/" + ASSET_DIRECTORIES[ i ] ) )
            result.assetDirsFound.push_back( ASSET_DIRECTORIES[ i ] );

    result.bHasGameDb = FileExists( result.szRoot + "/game.db" );
    return result;
}
