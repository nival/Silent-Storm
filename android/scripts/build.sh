#!/usr/bin/env bash
#
#  build.sh -- stage the engine sources and build libsilentstorm.so for each ABI.
#
#  Usage: scripts/build.sh [abi ...]        (default: arm64-v8a armeabi-v7a x86_64)
#
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ANDROID_DIR="$( dirname "$HERE" )"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
if [ ! -d "$SDK" ]; then
    echo "Android SDK not found. Set ANDROID_SDK_ROOT." >&2
    exit 1
fi

NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ]; then
    NDK="$( ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 )"
fi
if [ ! -d "$NDK" ]; then
    echo "Android NDK not found. Install one via the SDK manager, or set ANDROID_NDK_HOME." >&2
    exit 1
fi

#  A5_BUILD_ROOT overrides the build directory (default android/build), and
#  A5_CMAKE_EXTRA passes extra -D options -- e.g. a second working copy of the
#  build for a session that must not disturb another's:
#    A5_BUILD_ROOT=build-x A5_CMAKE_EXTRA="-DA5_AUDIO_NULL=ON" scripts/build.sh arm64-v8a
BUILD_ROOT="${A5_BUILD_ROOT:-$ANDROID_DIR/build}"
case "$BUILD_ROOT" in /*) ;; *) BUILD_ROOT="$ANDROID_DIR/$BUILD_ROOT" ;; esac

ABIS=( "$@" )
if [ ${#ABIS[@]} -eq 0 ]; then
    ABIS=( arm64-v8a armeabi-v7a x86_64 )
fi

echo "==> staging engine sources"
python3 "$ANDROID_DIR/tools/prepare_sources.py"

for ABI in "${ABIS[@]}"; do
    echo "==> building $ABI"
    cmake -S "$ANDROID_DIR" -B "$BUILD_ROOT/$ABI" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM=android-24 \
        -DANDROID_NDK="$NDK" \
        -DA5_BUILD_MAIN="${A5_BUILD_MAIN:-ON}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo ${A5_CMAKE_EXTRA:-} >/dev/null
    cmake --build "$BUILD_ROOT/$ABI"
done

echo "==> done"
for ABI in "${ABIS[@]}"; do
    printf '    %-14s %s\n' "$ABI" "$( ls -lh "$BUILD_ROOT/$ABI/libsilentstorm.so" | awk '{print $5}' )"
done
