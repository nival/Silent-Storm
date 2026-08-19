# Third-party code used by the Android port

| directory | what | version | licence |
|-----------|------|---------|---------|
| `ogg/`    | libogg, the Ogg container (bitwise.c, framing.c + headers; `config_types.h` hand-written) | 1.3.5 | BSD (`ogg/COPYING`) |
| `vorbis/` | libvorbis, the Xiph reference Vorbis codec: `lib/` minus `vorbisenc.c` and the tools, plus `include/vorbis/` | 1.3.7 | BSD (`vorbis/COPYING`) |

Both are unmodified copies of the release tarballs from downloads.xiph.org.
They exist for `platform/audio_android.cpp`: the game's sound assets were
encoded with libVorbis 1.0 beta 3 / RC1 (2001-02) and use **floor type 0**,
which the single-file decoders (stb_vorbis, minivorbis) do not implement --
6901 of the 6910 Ogg samples and all 30 music files fail there.  The
reference decoder plays all of them.
