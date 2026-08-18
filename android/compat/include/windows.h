/*
 *  windows.h -- the slice of the Win32 API that the Silent Storm engine actually
 *               uses, reimplemented on top of POSIX / Android NDK primitives.
 *
 *  The engine's StdAfx.h does `#include <windows.h>` unconditionally.  Rather than
 *  edit ~600 historical sources, this directory is placed ahead of the NDK sysroot
 *  on the include path and supplies the same vocabulary.  Everything declared here
 *  is implemented in compat/src/win32compat.cpp.
 *
 *  Scope note: this is deliberately NOT a general Win32 emulator (that is what Wine
 *  is for).  It covers the handful of API families the ported modules call, listed
 *  in docs/WIN32_SURFACE.md.  Adding a new module usually means adding a few more
 *  functions here.
 */
#ifndef A5_COMPAT_WINDOWS_H
#define A5_COMPAT_WINDOWS_H

#include "a5_msvc_compat.h"

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Fundamental types -------------------------------------------------- */
typedef int                 BOOL;
typedef unsigned char       BYTE;
typedef unsigned char       byte;
typedef unsigned short      WORD;
typedef unsigned int        DWORD;      /* 32 bits on Win32 *and* here (not "long") */
typedef unsigned int        UINT;
typedef int                 INT;
typedef long                LONG;
typedef unsigned long       ULONG;
typedef unsigned short      USHORT;
typedef unsigned char       UCHAR;
typedef char                CHAR;
typedef short               SHORT;
typedef float               FLOAT;
typedef long long           LONGLONG;
typedef unsigned long long  ULONGLONG;
typedef wchar_t             WCHAR;

typedef char               *LPSTR;
typedef const char         *LPCSTR;
typedef WCHAR              *LPWSTR;
typedef const WCHAR        *LPCWSTR;
typedef char                TCHAR;
typedef TCHAR              *LPTSTR;
typedef const TCHAR        *LPCTSTR;
typedef void               *LPVOID;
typedef const void         *LPCVOID;
typedef void               *PVOID;
typedef BYTE               *LPBYTE;
typedef DWORD              *LPDWORD;
typedef int                *LPINT;
typedef BOOL               *LPBOOL;
typedef long                LRESULT;
typedef unsigned int        WPARAM;
typedef long                LPARAM;
typedef size_t              SIZE_T;

/* Handles.  Opaque pointers, exactly as on Win32. */
typedef void               *HANDLE;
typedef void               *HMODULE;
typedef void               *HINSTANCE;
typedef void               *HWND;
typedef void               *HDC;
typedef void               *HICON;
typedef void               *HCURSOR;
typedef void               *HMENU;
typedef void               *HBITMAP;
typedef void               *HFONT;
typedef void               *HPALETTE;
typedef void               *HGLRC;
typedef long                HRESULT;

#ifndef TRUE
#  define TRUE  1
#  define FALSE 0
#endif
#ifndef NULL
#  define NULL 0
#endif
#define VOID void
#define CONST const
#define WINAPI
#define APIENTRY
#define CALLBACK
#define WINAPIV
#define IN
#define OUT
#define FAR
#define NEAR

#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE  0xFFFFFFFFu

#define S_OK      ((HRESULT)0)
#define S_FALSE   ((HRESULT)1)
#define E_FAIL    ((HRESULT)0x80004005L)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr)    (((HRESULT)(hr)) <  0)

#define MAKEWORD(a,b)  ((WORD)(((BYTE)(a)) | (((WORD)((BYTE)(b))) << 8)))
#define MAKELONG(a,b)  ((LONG)(((WORD)(a)) | (((DWORD)((WORD)(b))) << 16)))
#define LOWORD(l)      ((WORD)((DWORD)(l) & 0xffff))
#define HIWORD(l)      ((WORD)(((DWORD)(l) >> 16) & 0xffff))
#define LOBYTE(w)      ((BYTE)((DWORD)(w) & 0xff))
#define HIBYTE(w)      ((BYTE)(((DWORD)(w) >> 8) & 0xff))

typedef struct _POINT { LONG x, y; } POINT, *LPPOINT;
typedef struct _SIZE  { LONG cx, cy; } SIZE;
typedef struct _RECT  { LONG left, top, right, bottom; } RECT, *LPRECT;

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; } u;
    struct { DWORD LowPart; LONG HighPart; };
    long long QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;
typedef LARGE_INTEGER _LARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct { DWORD LowPart; DWORD HighPart; };
    unsigned long long QuadPart;
} ULARGE_INTEGER;

/* ----- Time --------------------------------------------------------------- */
typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME, *LPFILETIME;

typedef struct _SYSTEMTIME {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME, *LPSYSTEMTIME;

DWORD GetTickCount( void );
void  Sleep( DWORD dwMilliseconds );
BOOL  QueryPerformanceCounter( LARGE_INTEGER *lpPerformanceCount );
BOOL  QueryPerformanceFrequency( LARGE_INTEGER *lpFrequency );
void  GetSystemTime( SYSTEMTIME *lpSystemTime );
void  GetLocalTime( SYSTEMTIME *lpSystemTime );
BOOL  SystemTimeToFileTime( const SYSTEMTIME *lpSystemTime, FILETIME *lpFileTime );
BOOL  FileTimeToSystemTime( const FILETIME *lpFileTime, SYSTEMTIME *lpSystemTime );
LONG  CompareFileTime( const FILETIME *a, const FILETIME *b );
DWORD timeGetTime( void );

/* ----- Errors ------------------------------------------------------------- */
#define ERROR_SUCCESS           0
#define ERROR_FILE_NOT_FOUND    2
#define ERROR_PATH_NOT_FOUND    3
#define ERROR_ACCESS_DENIED     5
#define ERROR_NO_MORE_FILES     18
#define ERROR_ALREADY_EXISTS    183

DWORD GetLastError( void );
void  SetLastError( DWORD dwErrCode );

/* ----- File I/O ----------------------------------------------------------- */
#define GENERIC_READ            0x80000000u
#define GENERIC_WRITE           0x40000000u
#define FILE_SHARE_READ         0x00000001u
#define FILE_SHARE_WRITE        0x00000002u
#define CREATE_NEW              1
#define CREATE_ALWAYS           2
#define OPEN_EXISTING           3
#define OPEN_ALWAYS             4
#define TRUNCATE_EXISTING       5
#define FILE_ATTRIBUTE_READONLY   0x00000001u
#define FILE_ATTRIBUTE_HIDDEN     0x00000002u
#define FILE_ATTRIBUTE_DIRECTORY  0x00000010u
#define FILE_ATTRIBUTE_ARCHIVE    0x00000020u
#define FILE_ATTRIBUTE_NORMAL     0x00000080u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_RANDOM_ACCESS   0x10000000u
#define FILE_BEGIN              0
#define FILE_CURRENT            1
#define FILE_END                2
#define INVALID_FILE_SIZE       0xFFFFFFFFu
#define INVALID_SET_FILE_POINTER 0xFFFFFFFFu
#define INVALID_FILE_ATTRIBUTES  0xFFFFFFFFu

typedef struct _SECURITY_ATTRIBUTES {
    DWORD  nLength;
    LPVOID lpSecurityDescriptor;
    BOOL   bInheritHandle;
} SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _OVERLAPPED {
    unsigned long Internal, InternalHigh;
    DWORD  Offset, OffsetHigh;
    HANDLE hEvent;
} OVERLAPPED, *LPOVERLAPPED;

HANDLE CreateFileA( LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                    LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                    DWORD dwFlagsAndAttributes, HANDLE hTemplateFile );
BOOL   ReadFile( HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead,
                 LPDWORD lpNumberOfBytesRead, LPOVERLAPPED lpOverlapped );
BOOL   WriteFile( HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                  LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped );
DWORD  SetFilePointer( HANDLE hFile, LONG lDistanceToMove, LONG *lpDistanceToMoveHigh,
                       DWORD dwMoveMethod );
DWORD  GetFileSize( HANDLE hFile, LPDWORD lpFileSizeHigh );
BOOL   SetEndOfFile( HANDLE hFile );
BOOL   FlushFileBuffers( HANDLE hFile );
BOOL   GetFileTime( HANDLE hFile, FILETIME *pCreate, FILETIME *pAccess, FILETIME *pWrite );
BOOL   DeleteFileA( LPCSTR lpFileName );
BOOL   MoveFileA( LPCSTR lpExistingFileName, LPCSTR lpNewFileName );
BOOL   CopyFileA( LPCSTR lpExistingFileName, LPCSTR lpNewFileName, BOOL bFailIfExists );
BOOL   CreateDirectoryA( LPCSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes );
BOOL   RemoveDirectoryA( LPCSTR lpPathName );
DWORD  GetFileAttributesA( LPCSTR lpFileName );
BOOL   SetFileAttributesA( LPCSTR lpFileName, DWORD dwFileAttributes );
DWORD  GetCurrentDirectoryA( DWORD nBufferLength, LPSTR lpBuffer );
BOOL   SetCurrentDirectoryA( LPCSTR lpPathName );
DWORD  GetModuleFileNameA( HMODULE hModule, LPSTR lpFilename, DWORD nSize );
DWORD  GetFullPathNameA( LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR *lpFilePart );
UINT   GetTempFileNameA( LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName );
DWORD  GetTempPathA( DWORD nBufferLength, LPSTR lpBuffer );

/* Directory enumeration.  Backslashes and "*.*" patterns are accepted. */
typedef struct _WIN32_FIND_DATAA {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD    nFileSizeHigh;
    DWORD    nFileSizeLow;
    DWORD    dwReserved0;
    DWORD    dwReserved1;
    CHAR     cFileName[ MAX_PATH ];
    CHAR     cAlternateFileName[ 14 ];
} WIN32_FIND_DATAA, *LPWIN32_FIND_DATAA;

HANDLE FindFirstFileA( LPCSTR lpFileName, WIN32_FIND_DATAA *lpFindFileData );
BOOL   FindNextFileA( HANDLE hFindFile, WIN32_FIND_DATAA *lpFindFileData );
BOOL   FindClose( HANDLE hFindFile );

/* ----- Synchronisation ---------------------------------------------------- */
typedef struct _RTL_CRITICAL_SECTION {
    pthread_mutex_t mutex;
    int             initialised;
} CRITICAL_SECTION, *LPCRITICAL_SECTION;

void InitializeCriticalSection( CRITICAL_SECTION *lpCriticalSection );
void EnterCriticalSection( CRITICAL_SECTION *lpCriticalSection );
void LeaveCriticalSection( CRITICAL_SECTION *lpCriticalSection );
BOOL TryEnterCriticalSection( CRITICAL_SECTION *lpCriticalSection );
void DeleteCriticalSection( CRITICAL_SECTION *lpCriticalSection );

#define WAIT_OBJECT_0   0x00000000u
#define WAIT_TIMEOUT    0x00000102u
#define WAIT_FAILED     0xFFFFFFFFu

HANDLE CreateEventA( LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset,
                     BOOL bInitialState, LPCSTR lpName );
BOOL   SetEvent( HANDLE hEvent );
BOOL   ResetEvent( HANDLE hEvent );
BOOL   PulseEvent( HANDLE hEvent );
DWORD  WaitForSingleObject( HANDLE hHandle, DWORD dwMilliseconds );
BOOL   CloseHandle( HANDLE hObject );

typedef DWORD (WINAPI *LPTHREAD_START_ROUTINE)( LPVOID lpThreadParameter );
HANDLE CreateThread( LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                     LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter,
                     DWORD dwCreationFlags, LPDWORD lpThreadId );
DWORD  GetCurrentThreadId( void );
DWORD  GetCurrentProcessId( void );
BOOL   SetThreadPriority( HANDLE hThread, int nPriority );
#define THREAD_PRIORITY_NORMAL        0
#define THREAD_PRIORITY_ABOVE_NORMAL  1
#define THREAD_PRIORITY_BELOW_NORMAL (-1)

LONG InterlockedIncrement( LONG volatile *lpAddend );
LONG InterlockedDecrement( LONG volatile *lpAddend );
LONG InterlockedExchange( LONG volatile *Target, LONG Value );
LONG InterlockedExchangeAdd( LONG volatile *Addend, LONG Value );

/* ----- Modules ------------------------------------------------------------ */
HMODULE LoadLibraryA( LPCSTR lpLibFileName );
void   *GetProcAddress( HMODULE hModule, LPCSTR lpProcName );
BOOL    FreeLibrary( HMODULE hLibModule );
HMODULE GetModuleHandleA( LPCSTR lpModuleName );

/* ----- Memory ------------------------------------------------------------- */
#define MEM_COMMIT      0x00001000u
#define MEM_RESERVE     0x00002000u
#define MEM_RELEASE     0x00008000u
#define PAGE_READWRITE  0x04u
#define PAGE_READONLY   0x02u

LPVOID VirtualAlloc( LPVOID lpAddress, SIZE_T dwSize, DWORD flAllocationType, DWORD flProtect );
BOOL   VirtualFree( LPVOID lpAddress, SIZE_T dwSize, DWORD dwFreeType );
BOOL   IsBadReadPtr( const void *lp, SIZE_T ucb );
BOOL   IsBadWritePtr( void *lp, SIZE_T ucb );

typedef struct _MEMORYSTATUS {
    DWORD dwLength, dwMemoryLoad;
    SIZE_T dwTotalPhys, dwAvailPhys, dwTotalPageFile, dwAvailPageFile,
           dwTotalVirtual, dwAvailVirtual;
} MEMORYSTATUS, *LPMEMORYSTATUS;
void GlobalMemoryStatus( LPMEMORYSTATUS lpBuffer );

/* Win32 spells these as macros over memset/memcpy, and so do we. */
#define ZeroMemory(d,n)     memset((d), 0, (n))
#define CopyMemory(d,s,n)   memcpy((d), (s), (n))
#define MoveMemory(d,s,n)   memmove((d), (s), (n))
#define FillMemory(d,n,f)   memset((d), (f), (n))

/* ----- Path handling ------------------------------------------------------ */
/*  Game data was authored on a case-insensitive filesystem and every engine path
 *  uses backslashes.  Android's filesystem is case-sensitive and uses '/', so
 *  every path entering the compat layer goes through this resolver: it converts
 *  separators and, if the literal path does not exist, retries component by
 *  component ignoring case.  Result is cached, so repeated lookups are cheap.
 *
 *  Engine call sites that use fopen() directly should call a5_fopen() instead.
 */
char *a5_resolve_path( const char *pszWinPath, char *pszOut, size_t nOutSize );
FILE *a5_fopen( const char *pszFileName, const char *pszMode );
int   a5_stat_exists( const char *pszWinPath );
/* Directory the game data was mounted from; set once at start-up. */
void        a5_set_data_root( const char *pszRoot );
const char *a5_get_data_root( void );

/* ----- Diagnostics -------------------------------------------------------- */
void OutputDebugStringA( LPCSTR lpOutputString );

/* MessageBox has no Android equivalent; it logs at error level and returns IDOK. */
#define MB_OK               0x00000000u
#define MB_OKCANCEL         0x00000001u
#define MB_YESNO            0x00000004u
#define MB_ICONERROR        0x00000010u
#define MB_ICONEXCLAMATION  0x00000030u
#define MB_ICONINFORMATION  0x00000040u
#define IDOK      1
#define IDCANCEL  2
#define IDYES     6
#define IDNO      7
int MessageBoxA( HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType );

/* ----- Character sets ----------------------------------------------------- */
#define CP_ACP   0
#define CP_UTF8  65001
int MultiByteToWideChar( UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte,
                         LPWSTR lpWideCharStr, int cchWideChar );
int WideCharToMultiByte( UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar,
                         LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR lpDefaultChar,
                         LPBOOL lpUsedDefaultChar );

/* ----- Profile (.ini) files ----------------------------------------------- */
DWORD GetPrivateProfileStringA( LPCSTR sect, LPCSTR key, LPCSTR def,
                                LPSTR ret, DWORD size, LPCSTR file );
UINT  GetPrivateProfileIntA( LPCSTR sect, LPCSTR key, INT def, LPCSTR file );

#ifdef __cplusplus
}  /* extern "C" */
#endif

/* ----- ANSI/Unicode name mapping ------------------------------------------ */
/* The engine is an ANSI (single byte) build, so the plain names map to *A.    */
#define CreateFile              CreateFileA
#define DeleteFile              DeleteFileA
#define MoveFile                MoveFileA
#define CopyFile                CopyFileA
#define CreateDirectory         CreateDirectoryA
#define RemoveDirectory         RemoveDirectoryA
#define GetFileAttributes       GetFileAttributesA
#define SetFileAttributes       SetFileAttributesA
#define GetCurrentDirectory     GetCurrentDirectoryA
#define SetCurrentDirectory     SetCurrentDirectoryA
#define GetModuleFileName       GetModuleFileNameA
#define GetFullPathName         GetFullPathNameA
#define GetTempFileName         GetTempFileNameA
#define GetTempPath             GetTempPathA
#define FindFirstFile           FindFirstFileA
#define FindNextFile            FindNextFileA
#define WIN32_FIND_DATA         WIN32_FIND_DATAA
#define LPWIN32_FIND_DATA       LPWIN32_FIND_DATAA
#define CreateEvent             CreateEventA
#define LoadLibrary             LoadLibraryA
#define GetModuleHandle         GetModuleHandleA
#define OutputDebugString       OutputDebugStringA
#define MessageBox              MessageBoxA
#define GetPrivateProfileString GetPrivateProfileStringA
#define GetPrivateProfileInt    GetPrivateProfileIntA

#endif /* A5_COMPAT_WINDOWS_H */
