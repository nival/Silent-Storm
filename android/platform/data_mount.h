/*
 *  data_mount.h -- locating the game data on an Android device.
 *
 *  The engine expects to run with its working directory set to the game folder
 *  and reads paths like ".\\res\\Globals.res" or "Textures\\1234".  Android has
 *  no such notion, so the port picks a data root at start-up and hands it to the
 *  compat layer (a5_set_data_root), which resolves every engine path against it.
 */
#ifndef A5_DATA_MOUNT_H
#define A5_DATA_MOUNT_H

#include <string>
#include <vector>

struct SDataRootCandidate
{
    std::string szPath;
    std::string szWhy;       /* why we looked here */
    bool        bExists;
    int         nMarkersFound;
};

struct SDataMountResult
{
    bool                              bMounted;
    std::string                       szRoot;
    std::vector< SDataRootCandidate > candidates;
    std::vector< std::string >        packagesFound;   /* *.res in the root */
    std::vector< std::string >        assetDirsFound;  /* Textures/, Globals/, ... */
    bool                              bHasGameDb;
};

/*  Searches the usual locations, mounts the best one and returns what it found.
 *  pszExternalFilesDir comes from the Activity (getExternalFilesDir), which is
 *  writable without any runtime permission -- that is where users should push
 *  their own copy of the game data. */
SDataMountResult MountGameData( const char *pszExternalFilesDir,
                                const char *pszInternalFilesDir,
                                const char *pszExplicitRoot = 0 );

#endif
