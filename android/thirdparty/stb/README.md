# stb_vorbis

`stb_vorbis.c` v1.22 by Sean Barrett, unmodified, from
https://github.com/nothings/stb (public domain / MIT, see the file's tail).

The port uses it for the game's Ogg Vorbis assets: most of `Sounds/<id>` and
every music stream under `Res/Music/` (named `.wav` but Vorbis inside).
`platform/audio_android.cpp` includes it in header mode; this file is compiled
once as C by `CMakeLists.txt`.
