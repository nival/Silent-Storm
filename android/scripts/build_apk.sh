#!/usr/bin/env bash
#
#  build_apk.sh -- package libsilentstorm.so into an installable APK.
#
#  Uses the SDK build-tools directly (aapt2 / zipalign / apksigner) rather than
#  Gradle, so the whole build needs nothing but the SDK + NDK.  Android Studio
#  users can open android/ as a Gradle project instead; both paths produce the
#  same native library from the same CMakeLists.txt.
#
#  Usage: scripts/build_apk.sh [abi ...]
#
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ANDROID_DIR="$( dirname "$HERE" )"
BUILD_ROOT="${A5_BUILD_ROOT:-$ANDROID_DIR/build}"
case "$BUILD_ROOT" in /*) ;; *) BUILD_ROOT="$ANDROID_DIR/$BUILD_ROOT" ;; esac
OUT_DIR="$BUILD_ROOT/apk"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
BUILD_TOOLS="$( ls -d "$SDK"/build-tools/* | sort -V | tail -1 )"
PLATFORM_JAR="$( ls "$SDK"/platforms/android-*/android.jar | sort -V | tail -1 )"

ABIS=( "$@" )
if [ ${#ABIS[@]} -eq 0 ]; then
    ABIS=( arm64-v8a armeabi-v7a x86_64 )
fi

# Build any ABI whose library is missing.
for ABI in "${ABIS[@]}"; do
    if [ ! -f "$BUILD_ROOT/$ABI/libsilentstorm.so" ]; then
        "$HERE/build.sh" "$ABI"
    fi
done

echo "==> packaging with $( basename "$BUILD_TOOLS" ), $( basename "$( dirname "$PLATFORM_JAR" )" )"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/staging"

# 1. Compile the manifest into a base APK (no resources, no dex: hasCode=false).
"$BUILD_TOOLS/aapt2" link \
    -I "$PLATFORM_JAR" \
    --manifest "$ANDROID_DIR/app/AndroidManifest.xml" \
    --min-sdk-version 24 \
    --target-sdk-version 35 \
    -o "$OUT_DIR/base.apk"

# 2. Add the native libraries.  They go in lib/<abi>/ and, because the manifest
#    sets extractNativeLibs="true", may be compressed.
for ABI in "${ABIS[@]}"; do
    mkdir -p "$OUT_DIR/staging/lib/$ABI"
    cp "$BUILD_ROOT/$ABI/libsilentstorm.so" "$OUT_DIR/staging/lib/$ABI/"
done
( cd "$OUT_DIR/staging" && zip -q -r "$OUT_DIR/base.apk" lib )

# 3. Align, then sign with a debug key (generated on first run).
KEYSTORE="$ANDROID_DIR/build/debug.keystore"
if [ ! -f "$KEYSTORE" ]; then
    echo "==> generating a debug keystore"
    keytool -genkeypair -v -keystore "$KEYSTORE" \
        -alias androiddebugkey -storepass android -keypass android \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Silent Storm Android port (debug), OU=port, O=none, C=US" >/dev/null 2>&1
fi

"$BUILD_TOOLS/zipalign" -f -p 4 "$OUT_DIR/base.apk" "$OUT_DIR/silentstorm-aligned.apk"
"$BUILD_TOOLS/apksigner" sign \
    --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT_DIR/silentstorm.apk" \
    "$OUT_DIR/silentstorm-aligned.apk"

rm -f "$OUT_DIR/base.apk" "$OUT_DIR/silentstorm-aligned.apk" "$OUT_DIR/silentstorm.apk.idsig"
rm -rf "$OUT_DIR/staging"

echo "==> $OUT_DIR/silentstorm.apk  ($( ls -lh "$OUT_DIR/silentstorm.apk" | awk '{print $5}' ))"
