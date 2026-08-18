#!/usr/bin/env bash
#
#  run.sh -- install, launch, and show what the engine reported.
#
#  Usage: scripts/run.sh [--build]
#
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
ANDROID_DIR="$( dirname "$HERE" )"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
ADB="$SDK/platform-tools/adb"
PACKAGE="org.silentstorm.port"

if [ "${1:-}" = "--build" ]; then
    "$HERE/build_apk.sh"
fi

APK="$ANDROID_DIR/build/apk/silentstorm.apk"
if [ ! -f "$APK" ]; then
    echo "no APK yet -- run scripts/build_apk.sh" >&2
    exit 1
fi

"$ADB" wait-for-device
echo "==> installing"
"$ADB" install -r "$APK" >/dev/null

echo "==> launching"
"$ADB" logcat -c
"$ADB" shell am start -n "$PACKAGE/android.app.NativeActivity" >/dev/null
sleep 4

echo "==> engine report (adb logcat -s SilentStorm):"
"$ADB" logcat -d -s SilentStorm
