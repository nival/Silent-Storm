#  Silent Storm -- Android (NDK) build
#
#  Builds libsilentstorm.so from:
#    * compat/   -- Win32 -> POSIX shim (see docs/WIN32_SURFACE.md)
#    * gen/      -- engine modules staged by tools/prepare_sources.py
#    * platform/ -- the Android entry point, GLES presenter and boot harness
#
#  The engine's original build produced one DLL per module with .def-file
#  exports.  On Android everything links into a single .so, so those .def files
#  and the __declspec(dllimport) markers simply fall away (externA5 -> extern).

cmake_minimum_required(VERSION 3.22)
project(silentstorm CXX C)

#  C++14, not later: the engine uses std::ptr_fun and other binders that C++17
#  removed outright.  Nothing in the port needs a newer standard.
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ENGINE_GEN  ${CMAKE_CURRENT_SOURCE_DIR}/gen)
set(COMPAT_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/compat)

#  Headers only the host build needs.  <malloc.h> already exists on Android, and
#  shadowing it there produces an include cycle with bionic's <stdlib.h>.
if(NOT ANDROID)
    include_directories(${COMPAT_DIR}/include-host)
endif()
set(PLATFORM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/platform)

if(NOT EXISTS ${ENGINE_GEN}/Misc)
    message(FATAL_ERROR
        "Engine sources have not been staged yet.\n"
        "Run: python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_sources.py")
endif()

# ---------------------------------------------------------------------------
#  Flags shared by the historical engine code
# ---------------------------------------------------------------------------
#  These sources predate ISO C++11 by a decade; the warning suppressions below
#  are about *style* the language later tightened, not about correctness.  The
#  two that matter are documented individually.
set(ENGINE_COMPILE_OPTIONS
    -include ${COMPAT_DIR}/include/a5_msvc_compat.h  # MSVC CRT spellings
    -fms-extensions            # MSVC-isms clang can accept outright
    -fdelayed-template-parsing # MSVC parses template bodies at instantiation
    -fno-strict-aliasing       # FP_BITS() type-puns floats through DWORD*
    -fno-operator-names        # MSVC never treated `or`/`and`/`not` as keywords;
                               # the engine has variables named `or`
    -Wno-unknown-pragmas       # #pragma warning(disable:...)
    -Wno-invalid-token-paste
    -Wno-writable-strings
    -Wno-deprecated-register
    -Wno-int-to-void-pointer-cast
    -Wno-null-conversion
    -Wno-tautological-compare
    -Wno-parentheses
    -Wno-dangling-else
    -Wno-logical-op-parentheses
    -Wno-switch
    -Wno-format
    -Wno-unused-value
    -Wno-c++11-narrowing
    -Wno-inconsistent-missing-override
    -Wno-microsoft-goto
    -Wno-microsoft-template
)

set(ENGINE_DEFINITIONS
    A5_ANDROID=1
    NDEBUG                    # the engine's ASSERT() compiles out
    _STLP_NO_THREADS
)

# ---------------------------------------------------------------------------
#  compat -- the Win32 surface
# ---------------------------------------------------------------------------
add_library(a5compat STATIC
    ${COMPAT_DIR}/src/win32compat.cpp
    ${COMPAT_DIR}/src/msvc_crt.cpp
    ${COMPAT_DIR}/src/wide_char.cpp
)
target_include_directories(a5compat PUBLIC ${COMPAT_DIR}/include)
target_compile_definitions(a5compat PUBLIC ${ENGINE_DEFINITIONS})
if(ANDROID)
    target_link_libraries(a5compat PUBLIC log)
endif()

# ---------------------------------------------------------------------------
#  Engine modules
# ---------------------------------------------------------------------------
#  Misc     -- refcounting, math, strings, timing            (foundation)
#  FileIO   -- streams, chunked serialiser, .res packages
#  Script   -- Lua 4.0 fork + the engine's C++ wrapper
#  MiscDll  -- console variables/commands, log streams
function(a5_engine_module name)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} PUBLIC
        ${COMPAT_DIR}/include
        ${ENGINE_GEN}
        ${ENGINE_GEN}/${name}
    )
    target_compile_definitions(${name} PUBLIC ${ENGINE_DEFINITIONS})
    target_compile_options(${name} PRIVATE ${ENGINE_COMPILE_OPTIONS})
    target_link_libraries(${name} PUBLIC a5compat)
endfunction()

a5_engine_module(a5misc
    ${ENGINE_GEN}/Misc/Basic2.cpp
    ${ENGINE_GEN}/Misc/BasicShare.cpp
    ${ENGINE_GEN}/Misc/Events.cpp
    ${ENGINE_GEN}/Misc/EventsBase.cpp
    ${ENGINE_GEN}/Misc/HPTimer.cpp
    ${ENGINE_GEN}/Misc/RandomGen.cpp
    ${ENGINE_GEN}/Misc/StrProc.cpp
    ${ENGINE_GEN}/Misc/Tools.cpp
)

a5_engine_module(a5fileio
    ${ENGINE_GEN}/FileIO/BasicChunk1.cpp
    ${ENGINE_GEN}/FileIO/FilesPackage.cpp
    ${ENGINE_GEN}/FileIO/Streams.cpp
)
target_link_libraries(a5fileio PUBLIC a5misc)

file(GLOB SCRIPT_LUA_SOURCES ${ENGINE_GEN}/Script/l*.cpp)
#  MiscDll: console variables/commands and the log streams Script.cpp uses.
a5_engine_module(a5miscdll
    ${ENGINE_GEN}/MiscDll/Commands.cpp
    ${ENGINE_GEN}/MiscDll/LogStream.cpp
)
target_link_libraries(a5miscdll PUBLIC a5fileio)

a5_engine_module(a5script ${SCRIPT_LUA_SOURCES} ${ENGINE_GEN}/Script/Script.cpp)
target_link_libraries(a5script PUBLIC a5fileio a5miscdll)

#  libpng 1.0.9, as vendored in the tree.  Its two x86 assembler back ends
#  (pngvcrd.c: MSVC MMX, pnggccrd.c: GCC MMX) are left out; the C paths are
#  complete on their own.  zlib comes from the NDK / host system.
find_package(ZLIB REQUIRED)
add_library(a5png STATIC
    ${ENGINE_GEN}/libpng/png.c      ${ENGINE_GEN}/libpng/pngerror.c ${ENGINE_GEN}/libpng/pngget.c
    ${ENGINE_GEN}/libpng/pngmem.c   ${ENGINE_GEN}/libpng/pngpread.c ${ENGINE_GEN}/libpng/pngread.c
    ${ENGINE_GEN}/libpng/pngrio.c   ${ENGINE_GEN}/libpng/pngrtran.c ${ENGINE_GEN}/libpng/pngrutil.c
    ${ENGINE_GEN}/libpng/pngset.c   ${ENGINE_GEN}/libpng/pngtrans.c ${ENGINE_GEN}/libpng/pngwio.c
    ${ENGINE_GEN}/libpng/pngwrite.c ${ENGINE_GEN}/libpng/pngwtran.c ${ENGINE_GEN}/libpng/pngwutil.c
)
target_include_directories(a5png PUBLIC ${ENGINE_GEN}/libpng)
target_compile_definitions(a5png PUBLIC PNG_NO_ASSEMBLER_CODE)
#  Nival annotated libpng's exports with the engine's externA5 (dllimport marker),
#  so the C build needs the same compat prologue as the C++ modules.
target_compile_options(a5png PRIVATE
    -include ${COMPAT_DIR}/include/a5_msvc_compat.h
    -Wno-everything)   # 2001-era C; warnings are noise
target_link_libraries(a5png PUBLIC ZLIB::ZLIB)

#  Image -- BMP/TGA/PNG loading and the MMP mipmapped-texture container.
#  ImagePack.cpp (the DXT *encoder*, on a proprietary s3tc.h) is a tools-side
#  dependency of TexConv and is not part of the runtime.
a5_engine_module(a5image
    ${ENGINE_GEN}/Image/Image.cpp
    ${ENGINE_GEN}/Image/ImageBMP.cpp
    ${ENGINE_GEN}/Image/ImageMMP.cpp
    ${ENGINE_GEN}/Image/ImageOperation.cpp
    ${ENGINE_GEN}/Image/ImagePNG.cpp
    ${ENGINE_GEN}/Image/ImageTGA.cpp
)
target_link_libraries(a5image PUBLIC a5fileio a5png)

#  DBFormat -- the schema of every record in game.db.  ADOFake supplies the
#  database-source stub the shipping game links in place of ADOImport's COM/ADO
#  content-import path.
file(GLOB DBFORMAT_SOURCES ${ENGINE_GEN}/DBFormat/Data*.cpp)
a5_engine_module(a5dbformat
    ${DBFORMAT_SOURCES}
    ${ENGINE_GEN}/ADOFake/BasicDBfake.cpp
)
target_link_libraries(a5dbformat PUBLIC a5fileio a5miscdll)

# ---------------------------------------------------------------------------
#  Main -- the engine + game (154k lines).  Survey target: compiles every file
#  as an object library so errors can be counted; nothing links against it yet.
#  Enable with -DA5_BUILD_MAIN=ON.
# ---------------------------------------------------------------------------
option(A5_BUILD_MAIN "Compile the Main module (work in progress)" OFF)
if(A5_BUILD_MAIN)
    file(GLOB MAIN_SOURCES ${ENGINE_GEN}/Main/*.cpp ${ENGINE_GEN}/Main/*.CPP)
    add_library(a5main OBJECT ${MAIN_SOURCES})
    #  ${ENGINE_GEN}/Main is deliberately NOT an include directory: it holds a
    #  Time.h that would shadow the system <time.h> (a case-insensitive host, or
    #  the -I search order, both find it first).  Engine sources include their
    #  siblings as "Foo.h", which resolves relative to the including file and
    #  needs no -I.
    target_include_directories(a5main PUBLIC ${COMPAT_DIR}/include ${ENGINE_GEN})
    target_compile_definitions(a5main PUBLIC ${ENGINE_DEFINITIONS})
    target_compile_options(a5main PRIVATE ${ENGINE_COMPILE_OPTIONS} -ferror-limit=0)
    target_link_libraries(a5main PUBLIC a5compat)
endif()

# ---------------------------------------------------------------------------
#  The application
# ---------------------------------------------------------------------------
if(NOT ANDROID)
    #  Host build: the same boot harness without Android, so engine behaviour can
    #  be debugged on the development machine.  See docs/PORTING.md.
    add_executable(silentstorm_hosttest
        ${PLATFORM_DIR}/host_main.cpp
        ${PLATFORM_DIR}/boot_harness.cpp
        ${PLATFORM_DIR}/data_mount.cpp
        ${PLATFORM_DIR}/dxt_decode.cpp
    )
    target_include_directories(silentstorm_hosttest PRIVATE
        ${COMPAT_DIR}/include
        ${ENGINE_GEN}
        ${PLATFORM_DIR}
    )
    target_compile_options(silentstorm_hosttest PRIVATE ${ENGINE_COMPILE_OPTIONS})
    target_link_libraries(silentstorm_hosttest a5script a5dbformat a5image a5miscdll a5fileio a5misc a5compat)
    return()
endif()

#  native_app_glue ships with the NDK itself, so the APK can be assembled with
#  nothing but the SDK build-tools (no Gradle/prefab dependency).
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

add_library(silentstorm SHARED
    ${PLATFORM_DIR}/android_main.cpp
    ${PLATFORM_DIR}/gles_present.cpp
    ${PLATFORM_DIR}/boot_harness.cpp
    ${PLATFORM_DIR}/data_mount.cpp
    ${PLATFORM_DIR}/dxt_decode.cpp
    ${PLATFORM_DIR}/text_font.cpp
)
target_include_directories(silentstorm PRIVATE
    ${COMPAT_DIR}/include
    ${ENGINE_GEN}
    ${PLATFORM_DIR}
)
target_compile_options(silentstorm PRIVATE ${ENGINE_COMPILE_OPTIONS})
target_link_libraries(silentstorm
    native_app_glue
    a5script a5dbformat a5image a5miscdll a5fileio a5misc a5compat
    android log EGL GLESv2
)

#  ANativeActivity_onCreate lives in native_app_glue and nothing in our own code
#  references it, so the linker would garbage-collect it without this.
set_target_properties(silentstorm PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")
