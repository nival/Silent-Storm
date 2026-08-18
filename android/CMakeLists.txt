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
    )
    target_include_directories(silentstorm_hosttest PRIVATE
        ${COMPAT_DIR}/include
        ${ENGINE_GEN}
        ${PLATFORM_DIR}
    )
    target_compile_options(silentstorm_hosttest PRIVATE ${ENGINE_COMPILE_OPTIONS})
    target_link_libraries(silentstorm_hosttest a5script a5miscdll a5fileio a5misc a5compat)
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
    a5script a5miscdll a5fileio a5misc a5compat
    android log EGL GLESv2
)

#  ANativeActivity_onCreate lives in native_app_glue and nothing in our own code
#  references it, so the linker would garbage-collect it without this.
set_target_properties(silentstorm PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")
