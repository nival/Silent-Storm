/*
 *  a5_package_api.h -- plain C access to the engine's .res packages.
 *
 *  Implemented by the facade that tools/prepare_sources.py appends to
 *  FileIO/FilesPackage.cpp, where IFilesPackage is a complete type.
 *
 *  Handles are opaque; every call is safe against a null handle.
 */
#ifndef A5_PACKAGE_API_H
#define A5_PACKAGE_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* Opens a .res package.  Returns an opaque handle, or NULL if the file is
 * missing or does not carry a recognised package signature. */
void *A5PackageOpen( const char *pszFileName );
void  A5PackageClose( void *pPackage );

/* Number of entries in the package's file table. */
int   A5PackageGetFileCount( void *pPackage );

/* Fills pnOut with up to nMaxCount file IDs; returns how many were written.
 * Entries are numeric IDs, matching the numbered files in the loose data dirs. */
int   A5PackageGetFileIDs( void *pPackage, int *pnOut, int nMaxCount );

/* Size in bytes of one entry, or -1 if the ID is not in the package. */
int   A5PackageGetFileSize( void *pPackage, int nFileID );

/* Reads up to nMaxSize bytes of an entry; returns bytes read, or -1. */
int   A5PackageReadFile( void *pPackage, int nFileID, void *pDest, int nMaxSize );

/* Chunk-serialiser diagnostics (FileIO/BasicChunk1.cpp): how many object-table
 * entries of the last load named a type ID with no registered class, and the
 * last such ID.  Non-zero means the file was written by a build with classes
 * this source does not have. */
int  a5_serializer_unknown_types( int *pnLastTypeID );
void a5_serializer_reset_unknown_types( void );

#ifdef __cplusplus
}
#endif

#endif
