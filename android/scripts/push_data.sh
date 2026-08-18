#!/usr/bin/env bash
#
#  push_data.sh -- copy game data onto a connected device or emulator.
#
#  The port reads from the app's own external files directory:
#      /sdcard/Android/data/org.silentstorm.port/files/SilentStorm
#  which needs no runtime permission and is removed when the app is uninstalled.
#
#  Usage:
#      scripts/push_data.sh              # a small subset, enough to boot (~5 MB)
#      scripts/push_data.sh --full       # everything under Complete/ (~3.7 GB)
#      scripts/push_data.sh --from DIR   # copy from your own game install
#
set -euo pipefail

HERE="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
REPO_ROOT="$( dirname "$( dirname "$HERE" )" )"

SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/Library/Android/sdk}}"
ADB="$SDK/platform-tools/adb"
PACKAGE="org.silentstorm.port"
TARGET="/sdcard/Android/data/$PACKAGE/files/SilentStorm"

SOURCE="$REPO_ROOT/Complete"
MODE="subset"

while [ $# -gt 0 ]; do
    case "$1" in
        --full)  MODE="full"; shift ;;
        --from)  SOURCE="$2"; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ ! -d "$SOURCE" ]; then
    echo "game data not found at $SOURCE" >&2
    exit 1
fi

"$ADB" wait-for-device
"$ADB" shell mkdir -p "$TARGET"

if [ "$MODE" = "full" ]; then
    echo "==> pushing all of $SOURCE (this takes a while)"
    "$ADB" push "$SOURCE/." "$TARGET" >/dev/null
else
    # Enough for the boot harness: every package, the script sources, and the
    # small numbered asset directories.  Textures/ and Geometries/ are tens of
    # thousands of files and are left for --full.
    echo "==> pushing boot subset from $SOURCE"
    for f in "$SOURCE"/*.res; do
        [ -f "$f" ] && "$ADB" push "$f" "$TARGET/" >/dev/null
    done
    for d in Scripts Globals Chapters Waypoints Fonts; do
        if [ -d "$SOURCE/$d" ]; then
            "$ADB" shell mkdir -p "$TARGET/$d"
            "$ADB" push "$SOURCE/$d/." "$TARGET/$d" >/dev/null
        fi
    done
    # A few textures, so the MMP/DXT check has something to decode.
    if [ -d "$SOURCE/Textures" ]; then
        "$ADB" shell mkdir -p "$TARGET/Textures"
        for id in 1 2 4 6 10 13; do
            [ -f "$SOURCE/Textures/$id" ] && "$ADB" push "$SOURCE/Textures/$id" "$TARGET/Textures/" >/dev/null
        done
    fi
    # game.db: Complete/game.db is the full retail database (34 MB, 155
    # tables); Data/game.db is a 3 MB development cut of it (RPGWeaponTypes
    # has one row there, and weapons that reference the others crash the
    # import).  Both are the same generic-table format platform/db_retail.cpp
    # reads.
    "$ADB" push "$SOURCE/game.db" "$TARGET/game.db" >/dev/null
fi

# adb creates directories as the `shell` user with mode 0770, which the app's
# own uid cannot enter -- the data would be invisible to the game.  Widening the
# pushed tree is the fix for adb transfers; copying the files with a file
# manager or over MTP produces correctly-owned files and needs none of this.
echo "==> relaxing permissions on the pushed tree"
"$ADB" shell chmod -R a+rwX "$TARGET"

echo "==> data root contents:"
"$ADB" shell ls -la "$TARGET" | head -20
