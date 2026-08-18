/*
 *  win32compat.cpp -- POSIX/Android implementations of the Win32 subset declared
 *  in compat/include/windows.h.
 *
 *  Design notes
 *  ------------
 *  * Handles are small heap objects with a type tag, so CloseHandle() can tell a
 *    file from an event from a directory search.
 *  * Path handling is the interesting part: game data was authored on a
 *    case-insensitive volume and every engine path uses backslashes.  See
 *    a5_resolve_path() below.
 *  * Nothing here knows about the game; it is a pure platform shim.
 */
#include "windows.h"
#include "a5_log.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

/* ------------------------------------------------------------------------- */
/*  Logging                                                                    */
/* ------------------------------------------------------------------------- */
extern "C" void a5_log_write( int nPriority, const char *pszMessage )
{
#ifdef __ANDROID__
    __android_log_write( nPriority, A5_LOG_TAG, pszMessage );
#else
    fprintf( stderr, "[%d] %s\n", nPriority, pszMessage );
#endif
}

extern "C" void a5_log( int nPriority, const char *pszFormat, ... )
{
    va_list args;
    va_start( args, pszFormat );
#ifdef __ANDROID__
    __android_log_vprint( nPriority, A5_LOG_TAG, pszFormat, args );
#else
    fprintf( stderr, "[%d] ", nPriority );
    vfprintf( stderr, pszFormat, args );
    fputc( '\n', stderr );
#endif
    va_end( args );
}

/* ------------------------------------------------------------------------- */
/*  Handles                                                                    */
/* ------------------------------------------------------------------------- */
namespace {

enum EHandleType { HT_FILE = 1, HT_EVENT, HT_FIND, HT_THREAD };

struct SHandle
{
    EHandleType type;
    explicit SHandle( EHandleType t ) : type( t ) {}
    virtual ~SHandle() {}
};

struct SFileHandle : SHandle
{
    int fd;
    SFileHandle( int f ) : SHandle( HT_FILE ), fd( f ) {}
    ~SFileHandle() { if ( fd >= 0 ) close( fd ); }
};

struct SEventHandle : SHandle
{
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    bool            bSignalled;
    bool            bManualReset;
    SEventHandle( bool bManual, bool bInitial )
        : SHandle( HT_EVENT ), bSignalled( bInitial ), bManualReset( bManual )
    {
        pthread_mutex_init( &mutex, 0 );
        pthread_cond_init( &cond, 0 );
    }
    ~SEventHandle()
    {
        pthread_cond_destroy( &cond );
        pthread_mutex_destroy( &mutex );
    }
};

struct SFindHandle : SHandle
{
    DIR                     *pDir;
    std::string              szDirectory;   /* resolved, native separators */
    std::string              szPattern;     /* fnmatch pattern            */
    SFindHandle() : SHandle( HT_FIND ), pDir( 0 ) {}
    ~SFindHandle() { if ( pDir ) closedir( pDir ); }
};

struct SThreadHandle : SHandle
{
    pthread_t thread;
    SThreadHandle() : SHandle( HT_THREAD ), thread( 0 ) {}
};

inline SHandle *AsHandle( HANDLE h )
{
    if ( h == 0 || h == INVALID_HANDLE_VALUE )
        return 0;
    return static_cast< SHandle * >( h );
}

__thread DWORD g_dwLastError = ERROR_SUCCESS;

}  // namespace

extern "C" DWORD GetLastError( void )          { return g_dwLastError; }
extern "C" void  SetLastError( DWORD dwError ) { g_dwLastError = dwError; }

static DWORD ErrnoToWin32( int nErrno )
{
    switch ( nErrno )
    {
        case 0:       return ERROR_SUCCESS;
        case ENOENT:  return ERROR_FILE_NOT_FOUND;
        case ENOTDIR: return ERROR_PATH_NOT_FOUND;
        case EACCES:  return ERROR_ACCESS_DENIED;
        case EEXIST:  return ERROR_ALREADY_EXISTS;
        default:      return (DWORD)nErrno;
    }
}

/* ------------------------------------------------------------------------- */
/*  Path resolution                                                            */
/* ------------------------------------------------------------------------- */
/*
 *  Engine paths look like  "Complete\Textures\1234"  or  ".\cfg\game.cfg".
 *  On Android we must (a) swap separators and (b) cope with the fact that the
 *  original data was authored case-insensitively -- "TEXTURES" in a config file
 *  may refer to a directory named "Textures" on disk.
 *
 *  Strategy: try the literal translated path first (the fast path, one stat()).
 *  Only if that misses do we walk the path component by component, scanning each
 *  directory for a case-insensitive match.  Directory listings and successful
 *  resolutions are memoised because the engine reopens the same paths constantly.
 */
namespace {

pthread_mutex_t             g_pathMutex = PTHREAD_MUTEX_INITIALIZER;
std::map< std::string, std::string > g_pathCache;
std::string                 g_szDataRoot;

std::string NormaliseSeparators( const char *pszPath )
{
    std::string s( pszPath ? pszPath : "" );
    for ( size_t i = 0; i < s.size(); ++i )
        if ( s[ i ] == '\\' )
            s[ i ] = '/';
    /* Strip a leading "./" so cache keys stay canonical. */
    while ( s.size() > 2 && s[ 0 ] == '.' && s[ 1 ] == '/' )
        s.erase( 0, 2 );
    return s;
}

bool PathExists( const std::string &s )
{
    struct stat st;
    return stat( s.c_str(), &st ) == 0;
}

/* Case-insensitive lookup of one component inside a directory. */
bool FindComponentIgnoringCase( const std::string &szDir, const std::string &szName,
                                std::string *pszResult )
{
    DIR *pDir = opendir( szDir.empty() ? "." : szDir.c_str() );
    if ( !pDir )
        return false;
    bool bFound = false;
    struct dirent *pEntry;
    while ( ( pEntry = readdir( pDir ) ) != 0 )
    {
        if ( strcasecmp( pEntry->d_name, szName.c_str() ) == 0 )
        {
            *pszResult = pEntry->d_name;
            bFound = true;
            break;
        }
    }
    closedir( pDir );
    return bFound;
}

std::string ResolveIgnoringCase( const std::string &szPath )
{
    std::vector< std::string > components;
    std::string                szCurrent;
    bool                       bAbsolute = !szPath.empty() && szPath[ 0 ] == '/';

    for ( size_t i = 0; i <= szPath.size(); ++i )
    {
        if ( i == szPath.size() || szPath[ i ] == '/' )
        {
            if ( !szCurrent.empty() )
                components.push_back( szCurrent );
            szCurrent.clear();
        }
        else
            szCurrent += szPath[ i ];
    }

    std::string szBuilt = bAbsolute ? "/" : "";
    for ( size_t i = 0; i < components.size(); ++i )
    {
        const std::string &szComponent = components[ i ];
        if ( szComponent == "." )
            continue;
        std::string szCandidate = szBuilt + szComponent;
        if ( PathExists( szCandidate ) )
        {
            szBuilt = szCandidate + "/";
            continue;
        }
        std::string szDir = szBuilt.empty() ? std::string( "." )
                                            : szBuilt.substr( 0, szBuilt.size() - 1 );
        if ( szDir.empty() )
            szDir = "/";
        std::string szMatched;
        if ( !FindComponentIgnoringCase( szDir, szComponent, &szMatched ) )
            return std::string();  /* genuinely missing */
        szBuilt += szMatched;
        szBuilt += "/";
    }
    if ( !szBuilt.empty() && szBuilt[ szBuilt.size() - 1 ] == '/' && szBuilt != "/" )
        szBuilt.erase( szBuilt.size() - 1 );
    return szBuilt;
}

std::string ResolvePath( const char *pszWinPath )
{
    std::string szNative = NormaliseSeparators( pszWinPath );
    if ( szNative.empty() )
        return szNative;

    /* Relative paths are interpreted against the mounted data root. */
    if ( szNative[ 0 ] != '/' && !g_szDataRoot.empty() )
        szNative = g_szDataRoot + "/" + szNative;

    if ( PathExists( szNative ) )
        return szNative;

    pthread_mutex_lock( &g_pathMutex );
    std::map< std::string, std::string >::const_iterator it = g_pathCache.find( szNative );
    if ( it != g_pathCache.end() )
    {
        std::string szCached = it->second;
        pthread_mutex_unlock( &g_pathMutex );
        return szCached;
    }
    pthread_mutex_unlock( &g_pathMutex );

    std::string szResolved = ResolveIgnoringCase( szNative );
    if ( szResolved.empty() )
        return szNative;  /* let the caller fail with a sensible errno */

    pthread_mutex_lock( &g_pathMutex );
    g_pathCache[ szNative ] = szResolved;
    pthread_mutex_unlock( &g_pathMutex );
    return szResolved;
}

}  // namespace

extern "C" void a5_set_data_root( const char *pszRoot )
{
    pthread_mutex_lock( &g_pathMutex );
    g_szDataRoot = pszRoot ? pszRoot : "";
    while ( !g_szDataRoot.empty() && g_szDataRoot[ g_szDataRoot.size() - 1 ] == '/' )
        g_szDataRoot.erase( g_szDataRoot.size() - 1 );
    g_pathCache.clear();
    pthread_mutex_unlock( &g_pathMutex );
}

extern "C" const char *a5_get_data_root( void )
{
    return g_szDataRoot.c_str();
}

extern "C" char *a5_resolve_path( const char *pszWinPath, char *pszOut, size_t nOutSize )
{
    std::string szResolved = ResolvePath( pszWinPath );
    if ( pszOut && nOutSize )
    {
        strncpy( pszOut, szResolved.c_str(), nOutSize - 1 );
        pszOut[ nOutSize - 1 ] = 0;
    }
    return pszOut;
}

extern "C" FILE *a5_fopen( const char *pszFileName, const char *pszMode )
{
    std::string szResolved = ResolvePath( pszFileName );
    FILE *pFile = fopen( szResolved.c_str(), pszMode );
    if ( !pFile )
        SetLastError( ErrnoToWin32( errno ) );
    return pFile;
}

extern "C" int a5_stat_exists( const char *pszWinPath )
{
    return PathExists( ResolvePath( pszWinPath ) ) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/*  Time                                                                       */
/* ------------------------------------------------------------------------- */
static long long MonotonicNanoseconds( void )
{
    struct timespec ts;
    clock_gettime( CLOCK_MONOTONIC, &ts );
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

extern "C" DWORD GetTickCount( void )
{
    return (DWORD)( MonotonicNanoseconds() / 1000000LL );
}

extern "C" DWORD timeGetTime( void ) { return GetTickCount(); }

extern "C" void Sleep( DWORD dwMilliseconds )
{
    struct timespec ts;
    ts.tv_sec  = dwMilliseconds / 1000;
    ts.tv_nsec = (long)( dwMilliseconds % 1000 ) * 1000000L;
    while ( nanosleep( &ts, &ts ) == -1 && errno == EINTR )
        ;
}

extern "C" BOOL QueryPerformanceCounter( LARGE_INTEGER *pCount )
{
    if ( !pCount )
        return FALSE;
    pCount->QuadPart = MonotonicNanoseconds();
    return TRUE;
}

extern "C" BOOL QueryPerformanceFrequency( LARGE_INTEGER *pFrequency )
{
    if ( !pFrequency )
        return FALSE;
    pFrequency->QuadPart = 1000000000LL;  /* we report in nanoseconds */
    return TRUE;
}

/* Windows FILETIME counts 100ns ticks since 1601-01-01; Unix time starts 1970. */
static const long long FILETIME_UNIX_EPOCH_DELTA = 116444736000000000LL;

static FILETIME UnixTimeToFileTime( time_t t )
{
    long long ll = (long long)t * 10000000LL + FILETIME_UNIX_EPOCH_DELTA;
    FILETIME  ft;
    ft.dwLowDateTime  = (DWORD)( ll & 0xFFFFFFFFLL );
    ft.dwHighDateTime = (DWORD)( ll >> 32 );
    return ft;
}

static long long FileTimeToLongLong( const FILETIME *pFT )
{
    return ( (long long)pFT->dwHighDateTime << 32 ) | (long long)pFT->dwLowDateTime;
}

extern "C" void GetSystemTime( SYSTEMTIME *pSystemTime )
{
    struct timeval tv;
    gettimeofday( &tv, 0 );
    struct tm gm;
    time_t    t = tv.tv_sec;
    gmtime_r( &t, &gm );
    pSystemTime->wYear         = (WORD)( gm.tm_year + 1900 );
    pSystemTime->wMonth        = (WORD)( gm.tm_mon + 1 );
    pSystemTime->wDayOfWeek    = (WORD)gm.tm_wday;
    pSystemTime->wDay          = (WORD)gm.tm_mday;
    pSystemTime->wHour         = (WORD)gm.tm_hour;
    pSystemTime->wMinute       = (WORD)gm.tm_min;
    pSystemTime->wSecond       = (WORD)gm.tm_sec;
    pSystemTime->wMilliseconds = (WORD)( tv.tv_usec / 1000 );
}

extern "C" void GetLocalTime( SYSTEMTIME *pSystemTime )
{
    struct timeval tv;
    gettimeofday( &tv, 0 );
    struct tm lt;
    time_t    t = tv.tv_sec;
    localtime_r( &t, &lt );
    pSystemTime->wYear         = (WORD)( lt.tm_year + 1900 );
    pSystemTime->wMonth        = (WORD)( lt.tm_mon + 1 );
    pSystemTime->wDayOfWeek    = (WORD)lt.tm_wday;
    pSystemTime->wDay          = (WORD)lt.tm_mday;
    pSystemTime->wHour         = (WORD)lt.tm_hour;
    pSystemTime->wMinute       = (WORD)lt.tm_min;
    pSystemTime->wSecond       = (WORD)lt.tm_sec;
    pSystemTime->wMilliseconds = (WORD)( tv.tv_usec / 1000 );
}

extern "C" BOOL SystemTimeToFileTime( const SYSTEMTIME *pSystemTime, FILETIME *pFileTime )
{
    struct tm t;
    memset( &t, 0, sizeof( t ) );
    t.tm_year = pSystemTime->wYear - 1900;
    t.tm_mon  = pSystemTime->wMonth - 1;
    t.tm_mday = pSystemTime->wDay;
    t.tm_hour = pSystemTime->wHour;
    t.tm_min  = pSystemTime->wMinute;
    t.tm_sec  = pSystemTime->wSecond;
    *pFileTime = UnixTimeToFileTime( timegm( &t ) );
    return TRUE;
}

extern "C" BOOL FileTimeToSystemTime( const FILETIME *pFileTime, SYSTEMTIME *pSystemTime )
{
    long long ll = ( FileTimeToLongLong( pFileTime ) - FILETIME_UNIX_EPOCH_DELTA ) / 10000000LL;
    time_t    t  = (time_t)ll;
    struct tm gm;
    gmtime_r( &t, &gm );
    pSystemTime->wYear         = (WORD)( gm.tm_year + 1900 );
    pSystemTime->wMonth        = (WORD)( gm.tm_mon + 1 );
    pSystemTime->wDayOfWeek    = (WORD)gm.tm_wday;
    pSystemTime->wDay          = (WORD)gm.tm_mday;
    pSystemTime->wHour         = (WORD)gm.tm_hour;
    pSystemTime->wMinute       = (WORD)gm.tm_min;
    pSystemTime->wSecond       = (WORD)gm.tm_sec;
    pSystemTime->wMilliseconds = 0;
    return TRUE;
}

extern "C" LONG CompareFileTime( const FILETIME *a, const FILETIME *b )
{
    long long x = FileTimeToLongLong( a ), y = FileTimeToLongLong( b );
    return x < y ? -1 : ( x > y ? 1 : 0 );
}

/* ------------------------------------------------------------------------- */
/*  File I/O                                                                   */
/* ------------------------------------------------------------------------- */
extern "C" HANDLE CreateFileA( LPCSTR pszFileName, DWORD dwDesiredAccess, DWORD /*dwShareMode*/,
                               LPSECURITY_ATTRIBUTES /*pSecurity*/, DWORD dwCreationDisposition,
                               DWORD /*dwFlags*/, HANDLE /*hTemplate*/ )
{
    int nFlags = 0;
    const bool bRead  = ( dwDesiredAccess & GENERIC_READ ) != 0;
    const bool bWrite = ( dwDesiredAccess & GENERIC_WRITE ) != 0;

    if ( bRead && bWrite )      nFlags = O_RDWR;
    else if ( bWrite )          nFlags = O_WRONLY;
    else                        nFlags = O_RDONLY;

    switch ( dwCreationDisposition )
    {
        case CREATE_NEW:        nFlags |= O_CREAT | O_EXCL;  break;
        case CREATE_ALWAYS:     nFlags |= O_CREAT | O_TRUNC; break;
        case OPEN_ALWAYS:       nFlags |= O_CREAT;           break;
        case TRUNCATE_EXISTING: nFlags |= O_TRUNC;           break;
        case OPEN_EXISTING:
        default:                                             break;
    }

    std::string szPath = ResolvePath( pszFileName );
    int         fd     = open( szPath.c_str(), nFlags, 0666 );
    if ( fd < 0 )
    {
        SetLastError( ErrnoToWin32( errno ) );
        return INVALID_HANDLE_VALUE;
    }
    return new SFileHandle( fd );
}

extern "C" BOOL ReadFile( HANDLE hFile, LPVOID pBuffer, DWORD nBytesToRead,
                          LPDWORD pnBytesRead, LPOVERLAPPED )
{
    SHandle *pHandle = AsHandle( hFile );
    if ( !pHandle || pHandle->type != HT_FILE )
        return FALSE;
    ssize_t nRead = read( static_cast< SFileHandle * >( pHandle )->fd, pBuffer, nBytesToRead );
    if ( nRead < 0 )
    {
        SetLastError( ErrnoToWin32( errno ) );
        return FALSE;
    }
    if ( pnBytesRead )
        *pnBytesRead = (DWORD)nRead;
    return TRUE;
}

extern "C" BOOL WriteFile( HANDLE hFile, LPCVOID pBuffer, DWORD nBytesToWrite,
                           LPDWORD pnBytesWritten, LPOVERLAPPED )
{
    SHandle *pHandle = AsHandle( hFile );
    if ( !pHandle || pHandle->type != HT_FILE )
        return FALSE;
    ssize_t nWritten = write( static_cast< SFileHandle * >( pHandle )->fd, pBuffer, nBytesToWrite );
    if ( nWritten < 0 )
    {
        SetLastError( ErrnoToWin32( errno ) );
        return FALSE;
    }
    if ( pnBytesWritten )
        *pnBytesWritten = (DWORD)nWritten;
    return TRUE;
}

extern "C" DWORD SetFilePointer( HANDLE hFile, LONG lDistanceToMove,
                                 LONG *plDistanceToMoveHigh, DWORD dwMoveMethod )
{
    SHandle *pHandle = AsHandle( hFile );
    if ( !pHandle || pHandle->type != HT_FILE )
        return INVALID_SET_FILE_POINTER;

    long long nDistance = lDistanceToMove;
    if ( plDistanceToMoveHigh )
        nDistance |= ( (long long)*plDistanceToMoveHigh ) << 32;

    int nWhence = SEEK_SET;
    if ( dwMoveMethod == FILE_CURRENT ) nWhence = SEEK_CUR;
    else if ( dwMoveMethod == FILE_END ) nWhence = SEEK_END;

#ifdef __ANDROID__
    off_t nResult = lseek64( static_cast< SFileHandle * >( pHandle )->fd, nDistance, nWhence );
#else
    off_t nResult = lseek( static_cast< SFileHandle * >( pHandle )->fd, nDistance, nWhence );
#endif
    if ( nResult == (off_t)-1 )
    {
        SetLastError( ErrnoToWin32( errno ) );
        return INVALID_SET_FILE_POINTER;
    }
    if ( plDistanceToMoveHigh )
        *plDistanceToMoveHigh = (LONG)( (long long)nResult >> 32 );
    return (DWORD)nResult;
}

extern "C" DWORD GetFileSize( HANDLE hFile, LPDWORD pnFileSizeHigh )
{
    SHandle *pHandle = AsHandle( hFile );
    if ( !pHandle || pHandle->type != HT_FILE )
        return INVALID_FILE_SIZE;
    struct stat st;
    if ( fstat( static_cast< SFileHandle * >( pHandle )->fd, &st ) != 0 )
        return INVALID_FILE_SIZE;
    if ( pnFileSizeHigh )
        *pnFileSizeHigh = (DWORD)( (long long)st.st_size >> 32 );
    return (DWORD)( st.st_size & 0xFFFFFFFF );
}

extern "C" BOOL SetEndOfFile( HANDLE hFile )
{
    SHandle *pHandle = AsHandle( hFile );
    if ( !pHandle || pHandle->type != HT_FILE )
        return FALSE;
    int fd  = static_cast< SFileHandle * >( pHandle )->fd;
    off_t p = lseek( fd, 0, SEEK_CUR );
    return ftruncate( fd, p ) == 0 ? TRUE : FALSE;
}

extern "C" BOOL FlushFileBuffers( HANDLE hFile )
{
    SHandle *pHandle = AsHandle( hFile );
    if ( !pHandle || pHandle->type != HT_FILE )
        return FALSE;
    return fsync( static_cast< SFileHandle * >( pHandle )->fd ) == 0 ? TRUE : FALSE;
}

extern "C" BOOL GetFileTime( HANDLE hFile, FILETIME *pCreate, FILETIME *pAccess, FILETIME *pWrite )
{
    SHandle *pHandle = AsHandle( hFile );
    if ( !pHandle || pHandle->type != HT_FILE )
        return FALSE;
    struct stat st;
    if ( fstat( static_cast< SFileHandle * >( pHandle )->fd, &st ) != 0 )
        return FALSE;
    if ( pCreate ) *pCreate = UnixTimeToFileTime( st.st_ctime );
    if ( pAccess ) *pAccess = UnixTimeToFileTime( st.st_atime );
    if ( pWrite )  *pWrite  = UnixTimeToFileTime( st.st_mtime );
    return TRUE;
}

extern "C" BOOL DeleteFileA( LPCSTR pszFileName )
{
    return unlink( ResolvePath( pszFileName ).c_str() ) == 0 ? TRUE : FALSE;
}

extern "C" BOOL MoveFileA( LPCSTR pszExisting, LPCSTR pszNew )
{
    std::string szFrom = ResolvePath( pszExisting );
    std::string szTo   = NormaliseSeparators( pszNew );
    return rename( szFrom.c_str(), szTo.c_str() ) == 0 ? TRUE : FALSE;
}

extern "C" BOOL CopyFileA( LPCSTR pszExisting, LPCSTR pszNew, BOOL bFailIfExists )
{
    std::string szFrom = ResolvePath( pszExisting );
    std::string szTo   = NormaliseSeparators( pszNew );
    if ( bFailIfExists && PathExists( szTo ) )
        return FALSE;
    FILE *pIn = fopen( szFrom.c_str(), "rb" );
    if ( !pIn )
        return FALSE;
    FILE *pOut = fopen( szTo.c_str(), "wb" );
    if ( !pOut )
    {
        fclose( pIn );
        return FALSE;
    }
    char   buffer[ 64 * 1024 ];
    size_t nRead;
    while ( ( nRead = fread( buffer, 1, sizeof( buffer ), pIn ) ) > 0 )
        fwrite( buffer, 1, nRead, pOut );
    fclose( pIn );
    fclose( pOut );
    return TRUE;
}

extern "C" BOOL CreateDirectoryA( LPCSTR pszPathName, LPSECURITY_ATTRIBUTES )
{
    std::string szPath = NormaliseSeparators( pszPathName );
    if ( !szPath.empty() && szPath[ 0 ] != '/' && !g_szDataRoot.empty() )
        szPath = g_szDataRoot + "/" + szPath;
    if ( mkdir( szPath.c_str(), 0777 ) == 0 )
        return TRUE;
    SetLastError( ErrnoToWin32( errno ) );
    return FALSE;
}

extern "C" BOOL RemoveDirectoryA( LPCSTR pszPathName )
{
    return rmdir( ResolvePath( pszPathName ).c_str() ) == 0 ? TRUE : FALSE;
}

extern "C" DWORD GetFileAttributesA( LPCSTR pszFileName )
{
    struct stat st;
    if ( stat( ResolvePath( pszFileName ).c_str(), &st ) != 0 )
        return INVALID_FILE_ATTRIBUTES;
    DWORD dwAttributes = 0;
    if ( S_ISDIR( st.st_mode ) )
        dwAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    if ( !( st.st_mode & S_IWUSR ) )
        dwAttributes |= FILE_ATTRIBUTE_READONLY;
    if ( dwAttributes == 0 )
        dwAttributes = FILE_ATTRIBUTE_NORMAL;
    return dwAttributes;
}

extern "C" BOOL SetFileAttributesA( LPCSTR, DWORD ) { return TRUE; }

extern "C" DWORD GetCurrentDirectoryA( DWORD nBufferLength, LPSTR pszBuffer )
{
    if ( !getcwd( pszBuffer, nBufferLength ) )
        return 0;
    return (DWORD)strlen( pszBuffer );
}

extern "C" BOOL SetCurrentDirectoryA( LPCSTR pszPathName )
{
    return chdir( ResolvePath( pszPathName ).c_str() ) == 0 ? TRUE : FALSE;
}

extern "C" DWORD GetModuleFileNameA( HMODULE, LPSTR pszFilename, DWORD nSize )
{
    ssize_t n = readlink( "/proc/self/exe", pszFilename, nSize - 1 );
    if ( n < 0 )
    {
        pszFilename[ 0 ] = 0;
        return 0;
    }
    pszFilename[ n ] = 0;
    return (DWORD)n;
}

extern "C" DWORD GetFullPathNameA( LPCSTR pszFileName, DWORD nBufferLength, LPSTR pszBuffer,
                                   LPSTR *ppszFilePart )
{
    std::string szResolved = ResolvePath( pszFileName );
    char        szAbsolute[ PATH_MAX ];
    if ( !realpath( szResolved.c_str(), szAbsolute ) )
        strncpy( szAbsolute, szResolved.c_str(), sizeof( szAbsolute ) - 1 );
    strncpy( pszBuffer, szAbsolute, nBufferLength - 1 );
    pszBuffer[ nBufferLength - 1 ] = 0;
    if ( ppszFilePart )
    {
        char *pSlash = strrchr( pszBuffer, '/' );
        *ppszFilePart = pSlash ? pSlash + 1 : pszBuffer;
    }
    return (DWORD)strlen( pszBuffer );
}

extern "C" DWORD GetTempPathA( DWORD nBufferLength, LPSTR pszBuffer )
{
    const char *pszTemp = getenv( "TMPDIR" );
    if ( !pszTemp )
        pszTemp = "/data/local/tmp";
    strncpy( pszBuffer, pszTemp, nBufferLength - 1 );
    pszBuffer[ nBufferLength - 1 ] = 0;
    return (DWORD)strlen( pszBuffer );
}

extern "C" UINT GetTempFileNameA( LPCSTR pszPathName, LPCSTR pszPrefix, UINT uUnique,
                                  LPSTR pszTempFileName )
{
    static int nCounter = 0;
    if ( uUnique == 0 )
        uUnique = (UINT)( ( GetTickCount() << 8 ) + ( ++nCounter ) );
    sprintf( pszTempFileName, "%s/%.3s%x.tmp",
             pszPathName ? pszPathName : "/data/local/tmp",
             pszPrefix ? pszPrefix : "a5", uUnique );
    return uUnique;
}

/* ----- Directory enumeration --------------------------------------------- */
namespace {

/* Windows treats "*.*" as "everything"; fnmatch does not. */
std::string TranslateWildcard( const std::string &szPattern )
{
    if ( szPattern == "*.*" )
        return "*";
    return szPattern;
}

bool FillFindData( SFindHandle *pFind, WIN32_FIND_DATAA *pData )
{
    struct dirent *pEntry;
    while ( ( pEntry = readdir( pFind->pDir ) ) != 0 )
    {
        if ( strcmp( pEntry->d_name, "." ) == 0 || strcmp( pEntry->d_name, ".." ) == 0 )
            continue;
        if ( fnmatch( pFind->szPattern.c_str(), pEntry->d_name, FNM_CASEFOLD ) != 0 )
            continue;

        memset( pData, 0, sizeof( *pData ) );
        strncpy( pData->cFileName, pEntry->d_name, MAX_PATH - 1 );

        struct stat st;
        std::string szFull = pFind->szDirectory + "/" + pEntry->d_name;
        if ( stat( szFull.c_str(), &st ) == 0 )
        {
            pData->nFileSizeLow      = (DWORD)( st.st_size & 0xFFFFFFFF );
            pData->nFileSizeHigh     = (DWORD)( (long long)st.st_size >> 32 );
            pData->ftLastWriteTime   = UnixTimeToFileTime( st.st_mtime );
            pData->ftCreationTime    = UnixTimeToFileTime( st.st_ctime );
            pData->ftLastAccessTime  = UnixTimeToFileTime( st.st_atime );
            pData->dwFileAttributes  = S_ISDIR( st.st_mode ) ? FILE_ATTRIBUTE_DIRECTORY
                                                             : FILE_ATTRIBUTE_NORMAL;
        }
        return true;
    }
    return false;
}

}  // namespace

extern "C" HANDLE FindFirstFileA( LPCSTR pszFileName, WIN32_FIND_DATAA *pFindFileData )
{
    std::string szPath = NormaliseSeparators( pszFileName );

    std::string szDirectory = ".", szPattern = szPath;
    size_t      nSlash = szPath.rfind( '/' );
    if ( nSlash != std::string::npos )
    {
        szDirectory = szPath.substr( 0, nSlash );
        szPattern   = szPath.substr( nSlash + 1 );
    }

    SFindHandle *pFind    = new SFindHandle;
    pFind->szDirectory    = ResolvePath( szDirectory.c_str() );
    pFind->szPattern      = TranslateWildcard( szPattern );
    pFind->pDir           = opendir( pFind->szDirectory.c_str() );
    if ( !pFind->pDir )
    {
        delete pFind;
        SetLastError( ERROR_PATH_NOT_FOUND );
        return INVALID_HANDLE_VALUE;
    }
    if ( !FillFindData( pFind, pFindFileData ) )
    {
        delete pFind;
        SetLastError( ERROR_NO_MORE_FILES );
        return INVALID_HANDLE_VALUE;
    }
    return pFind;
}

extern "C" BOOL FindNextFileA( HANDLE hFindFile, WIN32_FIND_DATAA *pFindFileData )
{
    SHandle *pHandle = AsHandle( hFindFile );
    if ( !pHandle || pHandle->type != HT_FIND )
        return FALSE;
    if ( !FillFindData( static_cast< SFindHandle * >( pHandle ), pFindFileData ) )
    {
        SetLastError( ERROR_NO_MORE_FILES );
        return FALSE;
    }
    return TRUE;
}

extern "C" BOOL FindClose( HANDLE hFindFile )
{
    SHandle *pHandle = AsHandle( hFindFile );
    if ( !pHandle || pHandle->type != HT_FIND )
        return FALSE;
    delete pHandle;
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/*  Synchronisation                                                            */
/* ------------------------------------------------------------------------- */
extern "C" void InitializeCriticalSection( CRITICAL_SECTION *pSection )
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init( &attr );
    /* Win32 critical sections are recursive. */
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
    pthread_mutex_init( &pSection->mutex, &attr );
    pthread_mutexattr_destroy( &attr );
    pSection->initialised = 1;
}

extern "C" void EnterCriticalSection( CRITICAL_SECTION *pSection )
{
    if ( !pSection->initialised )
        InitializeCriticalSection( pSection );
    pthread_mutex_lock( &pSection->mutex );
}

extern "C" void LeaveCriticalSection( CRITICAL_SECTION *pSection )
{
    pthread_mutex_unlock( &pSection->mutex );
}

extern "C" BOOL TryEnterCriticalSection( CRITICAL_SECTION *pSection )
{
    if ( !pSection->initialised )
        InitializeCriticalSection( pSection );
    return pthread_mutex_trylock( &pSection->mutex ) == 0 ? TRUE : FALSE;
}

extern "C" void DeleteCriticalSection( CRITICAL_SECTION *pSection )
{
    if ( pSection->initialised )
    {
        pthread_mutex_destroy( &pSection->mutex );
        pSection->initialised = 0;
    }
}

extern "C" HANDLE CreateEventA( LPSECURITY_ATTRIBUTES, BOOL bManualReset, BOOL bInitialState, LPCSTR )
{
    return new SEventHandle( bManualReset != FALSE, bInitialState != FALSE );
}

extern "C" BOOL SetEvent( HANDLE hEvent )
{
    SHandle *pHandle = AsHandle( hEvent );
    if ( !pHandle || pHandle->type != HT_EVENT )
        return FALSE;
    SEventHandle *pEvent = static_cast< SEventHandle * >( pHandle );
    pthread_mutex_lock( &pEvent->mutex );
    pEvent->bSignalled = true;
    if ( pEvent->bManualReset )
        pthread_cond_broadcast( &pEvent->cond );
    else
        pthread_cond_signal( &pEvent->cond );
    pthread_mutex_unlock( &pEvent->mutex );
    return TRUE;
}

extern "C" BOOL ResetEvent( HANDLE hEvent )
{
    SHandle *pHandle = AsHandle( hEvent );
    if ( !pHandle || pHandle->type != HT_EVENT )
        return FALSE;
    SEventHandle *pEvent = static_cast< SEventHandle * >( pHandle );
    pthread_mutex_lock( &pEvent->mutex );
    pEvent->bSignalled = false;
    pthread_mutex_unlock( &pEvent->mutex );
    return TRUE;
}

extern "C" BOOL PulseEvent( HANDLE hEvent ) { return SetEvent( hEvent ); }

extern "C" DWORD WaitForSingleObject( HANDLE hHandle, DWORD dwMilliseconds )
{
    SHandle *pHandle = AsHandle( hHandle );
    if ( !pHandle )
        return WAIT_FAILED;

    if ( pHandle->type == HT_THREAD )
    {
        pthread_join( static_cast< SThreadHandle * >( pHandle )->thread, 0 );
        return WAIT_OBJECT_0;
    }
    if ( pHandle->type != HT_EVENT )
        return WAIT_FAILED;

    SEventHandle *pEvent = static_cast< SEventHandle * >( pHandle );
    DWORD         dwResult = WAIT_OBJECT_0;
    pthread_mutex_lock( &pEvent->mutex );
    if ( dwMilliseconds == INFINITE )
    {
        while ( !pEvent->bSignalled )
            pthread_cond_wait( &pEvent->cond, &pEvent->mutex );
    }
    else
    {
        struct timespec deadline;
        clock_gettime( CLOCK_REALTIME, &deadline );
        deadline.tv_sec  += dwMilliseconds / 1000;
        deadline.tv_nsec += (long)( dwMilliseconds % 1000 ) * 1000000L;
        if ( deadline.tv_nsec >= 1000000000L )
        {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        while ( !pEvent->bSignalled )
        {
            if ( pthread_cond_timedwait( &pEvent->cond, &pEvent->mutex, &deadline ) != 0 )
            {
                dwResult = pEvent->bSignalled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
                break;
            }
        }
    }
    if ( dwResult == WAIT_OBJECT_0 && !pEvent->bManualReset )
        pEvent->bSignalled = false;
    pthread_mutex_unlock( &pEvent->mutex );
    return dwResult;
}

extern "C" BOOL CloseHandle( HANDLE hObject )
{
    SHandle *pHandle = AsHandle( hObject );
    if ( !pHandle )
        return FALSE;
    delete pHandle;
    return TRUE;
}

namespace {
struct SThreadStart
{
    LPTHREAD_START_ROUTINE pfnStart;
    LPVOID                 pParameter;
};

void *ThreadTrampoline( void *pArg )
{
    SThreadStart *pStart = static_cast< SThreadStart * >( pArg );
    LPTHREAD_START_ROUTINE pfn = pStart->pfnStart;
    LPVOID                 p   = pStart->pParameter;
    delete pStart;
    return (void *)(intptr_t)pfn( p );
}
}  // namespace

extern "C" HANDLE CreateThread( LPSECURITY_ATTRIBUTES, SIZE_T dwStackSize,
                                LPTHREAD_START_ROUTINE pfnStartAddress, LPVOID pParameter,
                                DWORD, LPDWORD pnThreadId )
{
    SThreadHandle *pThread = new SThreadHandle;
    SThreadStart  *pStart  = new SThreadStart;
    pStart->pfnStart   = pfnStartAddress;
    pStart->pParameter = pParameter;

    pthread_attr_t attr;
    pthread_attr_init( &attr );
    if ( dwStackSize )
        pthread_attr_setstacksize( &attr, dwStackSize );
    int nResult = pthread_create( &pThread->thread, &attr, ThreadTrampoline, pStart );
    pthread_attr_destroy( &attr );

    if ( nResult != 0 )
    {
        delete pStart;
        delete pThread;
        return 0;
    }
    if ( pnThreadId )
        *pnThreadId = (DWORD)(uintptr_t)pThread->thread;
    return pThread;
}

extern "C" DWORD GetCurrentThreadId( void )
{
#ifdef __ANDROID__
    return (DWORD)gettid();
#else
    /* The host build only needs a stable per-thread value. */
    return (DWORD)(uintptr_t)pthread_self();
#endif
}
extern "C" DWORD GetCurrentProcessId( void ) { return (DWORD)getpid(); }
extern "C" BOOL  SetThreadPriority( HANDLE, int ) { return TRUE; }

extern "C" LONG InterlockedIncrement( LONG volatile *pAddend )
{
    return __sync_add_and_fetch( pAddend, 1 );
}
extern "C" LONG InterlockedDecrement( LONG volatile *pAddend )
{
    return __sync_sub_and_fetch( pAddend, 1 );
}
extern "C" LONG InterlockedExchange( LONG volatile *pTarget, LONG nValue )
{
    return __sync_lock_test_and_set( pTarget, nValue );
}
extern "C" LONG InterlockedExchangeAdd( LONG volatile *pAddend, LONG nValue )
{
    return __sync_fetch_and_add( pAddend, nValue );
}

/* ------------------------------------------------------------------------- */
/*  Modules                                                                    */
/* ------------------------------------------------------------------------- */
extern "C" HMODULE LoadLibraryA( LPCSTR pszLibFileName )
{
    /* The engine's DLLs are linked statically into libsilentstorm.so on Android,
     * so a load request for one of our own modules resolves to the main image. */
    void *pHandle = dlopen( pszLibFileName, RTLD_NOW | RTLD_LOCAL );
    if ( !pHandle )
    {
        a5_log( A5_PRIORITY_WARN, "LoadLibrary(\"%s\") failed: %s", pszLibFileName, dlerror() );
        SetLastError( ERROR_FILE_NOT_FOUND );
    }
    return pHandle;
}

extern "C" void *GetProcAddress( HMODULE hModule, LPCSTR pszProcName )
{
    return dlsym( hModule ? hModule : RTLD_DEFAULT, pszProcName );
}

extern "C" BOOL FreeLibrary( HMODULE hModule )
{
    return hModule && dlclose( hModule ) == 0 ? TRUE : FALSE;
}

extern "C" HMODULE GetModuleHandleA( LPCSTR pszModuleName )
{
    return pszModuleName ? dlopen( pszModuleName, RTLD_NOW | RTLD_NOLOAD ) : RTLD_DEFAULT;
}

/* ------------------------------------------------------------------------- */
/*  Memory                                                                     */
/* ------------------------------------------------------------------------- */
#include <sys/mman.h>

/* Not every platform defines MAP_NORESERVE; where it is absent the flag is
 * simply the default behaviour. */
#ifndef MAP_NORESERVE
#  define MAP_NORESERVE 0
#endif

extern "C" LPVOID VirtualAlloc( LPVOID pAddress, SIZE_T nSize, DWORD dwAllocationType, DWORD flProtect )
{
    if ( dwAllocationType & MEM_RESERVE && !( dwAllocationType & MEM_COMMIT ) )
    {
        void *p = mmap( pAddress, nSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0 );
        return p == MAP_FAILED ? 0 : p;
    }
    int nProtection = ( flProtect == PAGE_READONLY ) ? PROT_READ : ( PROT_READ | PROT_WRITE );
    void *p = mmap( pAddress, nSize, nProtection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0 );
    return p == MAP_FAILED ? 0 : p;
}

extern "C" BOOL VirtualFree( LPVOID pAddress, SIZE_T nSize, DWORD dwFreeType )
{
    if ( dwFreeType & MEM_RELEASE )
        return munmap( pAddress, nSize ? nSize : 1 ) == 0 ? TRUE : FALSE;
    return TRUE;
}

extern "C" BOOL IsBadReadPtr( const void *lp, SIZE_T ) { return lp == 0 ? TRUE : FALSE; }
extern "C" BOOL IsBadWritePtr( void *lp, SIZE_T )      { return lp == 0 ? TRUE : FALSE; }

extern "C" void GlobalMemoryStatus( LPMEMORYSTATUS pBuffer )
{
    memset( pBuffer, 0, sizeof( *pBuffer ) );
    pBuffer->dwLength = sizeof( *pBuffer );
    long nPageSize = sysconf( _SC_PAGESIZE );
    long nTotal    = sysconf( _SC_PHYS_PAGES );
#ifdef _SC_AVPHYS_PAGES
    long nAvail    = sysconf( _SC_AVPHYS_PAGES );
#else
    long nAvail    = nTotal;   /* macOS host build: not reported */
#endif
    pBuffer->dwTotalPhys    = (SIZE_T)( nTotal * nPageSize );
    pBuffer->dwAvailPhys    = (SIZE_T)( nAvail * nPageSize );
    pBuffer->dwTotalVirtual = pBuffer->dwTotalPhys;
    pBuffer->dwAvailVirtual = pBuffer->dwAvailPhys;
    if ( nTotal > 0 )
        pBuffer->dwMemoryLoad = (DWORD)( 100 - ( nAvail * 100 / nTotal ) );
}

/* ------------------------------------------------------------------------- */
/*  Diagnostics                                                                */
/* ------------------------------------------------------------------------- */
extern "C" void OutputDebugStringA( LPCSTR pszOutputString )
{
    if ( pszOutputString )
        a5_log_write( A5_PRIORITY_DEBUG, pszOutputString );
}

extern "C" int MessageBoxA( HWND, LPCSTR pszText, LPCSTR pszCaption, UINT )
{
    a5_log( A5_PRIORITY_ERROR, "[%s] %s", pszCaption ? pszCaption : "Message",
            pszText ? pszText : "" );
    return IDOK;
}

extern "C" void a5_debugbreak( const char *pszFile, int nLine )
{
    a5_log( A5_PRIORITY_ERROR, "assertion failed at %s(%d)", pszFile, nLine );
#ifdef __ANDROID__
    raise( SIGTRAP );
#endif
}

/*  Character-set conversion lives in wide_char.cpp, together with the codepage
 *  tables and the char16_t string helpers. */

/* ------------------------------------------------------------------------- */
/*  .ini files                                                                 */
/* ------------------------------------------------------------------------- */
extern "C" DWORD GetPrivateProfileStringA( LPCSTR pszSection, LPCSTR pszKey, LPCSTR pszDefault,
                                           LPSTR pszReturn, DWORD nSize, LPCSTR pszFile )
{
    pszReturn[ 0 ] = 0;
    FILE *pFile = a5_fopen( pszFile, "rb" );
    if ( pFile )
    {
        char szLine[ 1024 ];
        bool bInSection = ( pszSection == 0 );
        while ( fgets( szLine, sizeof( szLine ), pFile ) )
        {
            char *p = szLine;
            while ( *p == ' ' || *p == '\t' ) ++p;
            size_t nLength = strlen( p );
            while ( nLength && ( p[ nLength - 1 ] == '\n' || p[ nLength - 1 ] == '\r' ) )
                p[ --nLength ] = 0;
            if ( *p == '[' )
            {
                char *pEnd = strchr( p, ']' );
                if ( pEnd )
                {
                    *pEnd = 0;
                    bInSection = pszSection && strcasecmp( p + 1, pszSection ) == 0;
                }
                continue;
            }
            if ( !bInSection )
                continue;
            char *pEquals = strchr( p, '=' );
            if ( !pEquals )
                continue;
            *pEquals = 0;
            char *pValue = pEquals + 1;
            /* trim key */
            size_t nKeyLength = strlen( p );
            while ( nKeyLength && ( p[ nKeyLength - 1 ] == ' ' || p[ nKeyLength - 1 ] == '\t' ) )
                p[ --nKeyLength ] = 0;
            if ( pszKey && strcasecmp( p, pszKey ) == 0 )
            {
                while ( *pValue == ' ' || *pValue == '\t' ) ++pValue;
                strncpy( pszReturn, pValue, nSize - 1 );
                pszReturn[ nSize - 1 ] = 0;
                fclose( pFile );
                return (DWORD)strlen( pszReturn );
            }
        }
        fclose( pFile );
    }
    if ( pszDefault )
    {
        strncpy( pszReturn, pszDefault, nSize - 1 );
        pszReturn[ nSize - 1 ] = 0;
    }
    return (DWORD)strlen( pszReturn );
}

extern "C" UINT GetPrivateProfileIntA( LPCSTR pszSection, LPCSTR pszKey, INT nDefault, LPCSTR pszFile )
{
    char szBuffer[ 64 ];
    GetPrivateProfileStringA( pszSection, pszKey, "", szBuffer, sizeof( szBuffer ), pszFile );
    if ( szBuffer[ 0 ] == 0 )
        return (UINT)nDefault;
    return (UINT)atoi( szBuffer );
}

/* ------------------------------------------------------------------------- */
/*  MSVC _findfirst / _findnext / _findclose  (declared in compat io.h)         */
/* ------------------------------------------------------------------------- */
#include "io.h"

namespace {
void FillFindData( struct _finddata_t *pOut, const WIN32_FIND_DATAA &in )
{
    pOut->attrib = 0;
    if ( in.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ) pOut->attrib |= _A_SUBDIR;
    if ( in.dwFileAttributes & FILE_ATTRIBUTE_READONLY )  pOut->attrib |= _A_RDONLY;
    pOut->size = in.nFileSizeLow;
    pOut->time_write = (time_t)( ( FileTimeToLongLong( &in.ftLastWriteTime ) - FILETIME_UNIX_EPOCH_DELTA ) / 10000000LL );
    pOut->time_create = pOut->time_access = pOut->time_write;
    strncpy( pOut->name, in.cFileName, sizeof( pOut->name ) - 1 );
    pOut->name[ sizeof( pOut->name ) - 1 ] = 0;
}
}  // namespace

extern "C" intptr_t _findfirst( const char *pszFileSpec, struct _finddata_t *pFileInfo )
{
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA( pszFileSpec, &data );
    if ( h == INVALID_HANDLE_VALUE )
        return -1;
    FillFindData( pFileInfo, data );
    return (intptr_t)h;
}

extern "C" int _findnext( intptr_t hFile, struct _finddata_t *pFileInfo )
{
    WIN32_FIND_DATAA data;
    if ( !FindNextFileA( (HANDLE)hFile, &data ) )
        return -1;
    FillFindData( pFileInfo, data );
    return 0;
}

extern "C" int _findclose( intptr_t hFile )
{
    return FindClose( (HANDLE)hFile ) ? 0 : -1;
}

extern "C" UINT GetDoubleClickTime( void ) { return A5_DOUBLE_CLICK_MS; }

namespace { long g_nPointerX = 0, g_nPointerY = 0; bool g_bPointerSeen = false; }
extern "C" void a5_set_pointer_position( long x, long y ) { g_nPointerX = x; g_nPointerY = y; g_bPointerSeen = true; }
extern "C" int  a5_get_pointer_absolute( LONG *px, LONG *py )
{
    if ( !g_bPointerSeen ) return 0;
    if ( px ) *px = g_nPointerX; if ( py ) *py = g_nPointerY;
    return 1;
}
extern "C" void a5_get_pointer_position( LONG *px, LONG *py ) { if ( px ) *px = g_nPointerX; if ( py ) *py = g_nPointerY; }

/* ------------------------------------------------------------------------- */
/*  Window geometry (see the note in windows.h)                                */
/* ------------------------------------------------------------------------- */
namespace { int g_nClientW = 800, g_nClientH = 600; }
extern "C" void a5_set_client_size( int nWidth, int nHeight ) { g_nClientW = nWidth; g_nClientH = nHeight; }
extern "C" BOOL GetClientRect( HWND, RECT *pRect )
{
    if ( !pRect ) return FALSE;
    pRect->left = 0; pRect->top = 0; pRect->right = g_nClientW; pRect->bottom = g_nClientH;
    return TRUE;
}
extern "C" BOOL IsWindowVisible( HWND ) { return TRUE; }
extern "C" BOOL SetWindowPos( HWND, HWND, int, int, int, int, UINT ) { return TRUE; }
