[English](README.md)        [Русский](README_Russian.md)        [中文](README_Chinese.md)        [हिन्दी](README_Hindi.md)        [Español](README_Spanish.md)        [Français](README_French.md)        [Deutsch](README_German.md)        [Português](README_Portuguese.md)        [日本語](README_Japanese.md)        [Bahasa Indonesia](README_Indonesian.md)

[![Silent Storm Trailer](Silent_Storm.png)](https://www.youtube.com/watch?v=ugRAaC7K_1I)

The computer game [Silent Storm](https://en.wikipedia.org/wiki/Silent_Storm) is a turn-based tactical RPG developed by [Nival Interactive](http://nival.com/) and released in 2003.

The game is still available on [Steam](https://store.steampowered.com/app/254960/Silent_Storm_Gold_Edition/) and [GOG.com](https://www.gog.com/game/silent_storm_gold).

In 2026, the game's source code was released under a [special license](LICENSE.md) that prohibits commercial use but is completely open to the game's community, education, and research. Please review the terms of the [license agreement](LICENSE.md) carefully before using it.

## Tech stack

- **Engine**: Proprietary 3D engine, primarily written in C++
- **Scripting Language**: Lua 4.0
- **Animation**: Custom animation system
- **Video**: Bink Video Technology ⚠️ *Commercial license — not included*
- **Sound**: FMOD sound system ⚠️ *Commercial license — not included*
- **Graphics**: DirectX 8

## What is in this repository

- `Soft/` — source code and development tools
- `Complete/` — game data and resources
- `Data/` — game configuration data
- `Tools/` — development and build tools
- `bin/` — compiled executables
- `cfg/` — configuration files
- `Versions/Current` — developer version
- `android/` — Android (NDK) port of the engine — see [android/README.md](android/README.md)

---

# Running the Game

## Basic Launch

1. Navigate to the `bin/` directory.
2. Run the game executable, `Game.exe`.

By default, the game runs in windowed mode at 800x600 resolution. For windowed mode, the color depth in Display Properties/Settings/Colors must be set to 32-bit (true color). To run in fullscreen mode, use the `-fullscreen` parameter. To change the resolution, use the `-640`, `-1024`, or `-1280` parameters for 640x480, 1024x768, or 1280x1024, respectively.

⚠️ There are issues running the game on modern operating systems. If you find a solution, please let us know through GitHub Issues.

---

# Android

The `android/` directory builds the engine for Android with the NDK. The engine
core — file I/O, the `.res` package reader, the chunk serialiser and the Lua 4
virtual machine — runs on device against real game data; the Direct3D renderer,
audio and the game layer are not ported yet.

```bash
cd android
./scripts/build_apk.sh     # build (SDK + NDK only, no Gradle required)
./scripts/push_data.sh     # copy game data to the device
./scripts/run.sh           # install and launch
```

The original sources under `Soft/` are not modified: a staging step copies the
modules being built and applies a documented set of rewrites (include paths, x86
assembly, MSVC-only C++, 64-bit fixes). Run
`python3 android/tools/prepare_sources.py --report` to list every change.

See [android/README.md](android/README.md) for the current state and
[android/docs/PORTING.md](android/docs/PORTING.md) for what porting the rest
involves.

---

# Map Editor and Development Tools

## Map Editor

- **Location**: `bin/MapEdit.exe`

---

# Building the Project

## Build Requirements

- Microsoft Visual Studio .NET 2003
- DirectX 8.1 SDK

---

### ⚠️ Additional tools not included in the source code:

- **FMOD Audio System**
- **Bink Video Technology**

### 📋 Third-party component licenses:

- **STLport** — BSD-like license
- **zlib** — zlib License
- **Lua 4.0** — MIT-like license
- **Ogg Vorbis** — BSD-like license
