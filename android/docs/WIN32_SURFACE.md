# The Win32 surface the port implements

`compat/include/windows.h` and `compat/src/win32compat.cpp` are not a general
Win32 emulator — that is what Wine is for. They cover exactly the API families
the ported engine modules call, so that the 2003 sources compile and run
unmodified. This file is the inventory, and the place to record additions.

## Implemented

| Family | Functions | Notes |
|---|---|---|
| Time | `GetTickCount`, `QueryPerformanceCounter`/`Frequency`, `Sleep`, `timeGetTime`, `GetSystemTime`, `GetLocalTime` | monotonic clock; QPC reports nanoseconds, so its "frequency" is 1e9 |
| File I/O | `CreateFile`, `ReadFile`, `WriteFile`, `SetFilePointer`, `GetFileSize`, `SetEndOfFile`, `FlushFileBuffers`, `GetFileTime`, `CloseHandle` | handles are tagged heap objects, so `CloseHandle` can tell kinds apart |
| Files & paths | `DeleteFile`, `MoveFile`, `CopyFile`, `CreateDirectory`, `RemoveDirectory`, `GetFileAttributes`, `GetCurrentDirectory`, `SetCurrentDirectory`, `GetModuleFileName`, `GetFullPathName`, `GetTempPath`, `GetTempFileName` | every path goes through the resolver below |
| Directory search | `FindFirstFile`, `FindNextFile`, `FindClose` | `dirent` + `fnmatch`; `"*.*"` is translated to `"*"` |
| Synchronisation | `InitializeCriticalSection` and friends, `CreateEvent`, `SetEvent`, `ResetEvent`, `PulseEvent`, `WaitForSingleObject`, `CreateThread`, `Interlocked*` | critical sections are recursive pthread mutexes, matching Win32 |
| Modules | `LoadLibrary`, `GetProcAddress`, `FreeLibrary`, `GetModuleHandle` | `dlopen` family; the engine's own "DLLs" are linked into the one `.so` |
| Memory | `VirtualAlloc`, `VirtualFree`, `GlobalMemoryStatus`, `IsBadReadPtr` | `mmap`-backed |
| Diagnostics | `OutputDebugString`, `MessageBox` | both go to logcat; `MessageBox` returns `IDOK` rather than blocking a device with no input focus |
| Character sets | `MultiByteToWideChar`, `WideCharToMultiByte` | byte-extension only — see the `wchar_t` note in PORTING.md |
| Profiles | `GetPrivateProfileString`, `GetPrivateProfileInt` | small `.ini` parser |
| CRT gaps | `stricmp`, `strnicmp`, `memicmp`, `itoa`/`ltoa`/`ultoa`/`_i64toa`, `strupr`, `strlwr`, `_splitpath`, `_makepath`, `_fullpath`, `_itow`, `_wtof`, `_wtoi`, MSVC-signature `swprintf`/`vswprintf` | in `a5_msvc_compat.h`, force-included into every engine translation unit |

Also provided as headers, because the engine includes them: `crtdbg.h` (no-ops),
`hash_map` / `hash_set` (STLport's containers expressed as the C++11 unordered
ones), `stl/_config.h`, `io.h`, `direct.h`, `process.h`, `tchar.h`.

## Path resolution

The engine builds paths like `Textures\1234` and `.\cfg\autoexec.cfg`, against a
current directory, on a case-insensitive filesystem. Android has none of those
things, so `a5_resolve_path()`:

1. converts `\` to `/` and strips a leading `./`
2. makes relative paths absolute against the mounted data root
   (`a5_set_data_root`, called once at start-up by the data mount)
3. if the literal path does not exist, walks it component by component matching
   case-insensitively, and caches the result

Engine call sites that use `fopen` directly are routed to `a5_fopen`, which does
the same resolution — currently one site, `CFileStream::Open`.

## Deliberately absent

The engine never calls these, so they are not implemented; if a newly ported
module needs one, add it here and to this table.

* Registry (`RegOpenKey` …) — configuration is plain text under `cfg/`
* `CreateProcess`, `ShellExecute`
* Memory-mapped files (`CreateFileMapping`, `MapViewOfFile`)
* Structured exception handling (`__try`/`__except`)
* COM (`CoInitialize` …) — only `ADOImport` used it, and the game links `ADOFake`
* GDI, USER32, window management — replaced by `NativeActivity` + EGL
* DirectX of any kind — see PORTING.md
