/*
 *  audio_decode.h -- the sound-asset decoders behind platform/audio_android.cpp,
 *  kept free of engine and Android dependencies so they can be tested on the
 *  host (tools/audio_decode_test.cpp checks every WAV in Sounds/ against
 *  ffmpeg and every Ogg against libvorbis's own count).
 *
 *  Formats, from a survey of the retail Sounds/ directory (8494 files):
 *    6910  Ogg Vorbis    mono 44.1 kHz (one stereo); libVorbis 1.0 beta3/RC1,
 *                        floor type 0 -- hence the reference decoder
 *    1300  RIFF WAVE     Microsoft ADPCM (tag 2), 4-bit mono 44.1 kHz
 *      80  RIFF WAVE     IMA/DVI ADPCM (tag 0x11), 4-bit mono 44.1 kHz
 *     202  RIFF WAVE     PCM 16-bit mono 44.1 kHz
 *       2  RIFF WAVE     PCM 8-bit mono 22.05 kHz
 *  and Res/Music/*.wav: Ogg Vorbis stereo 44.1 kHz despite the extension.
 */
#ifndef A5_AUDIO_DECODE_H
#define A5_AUDIO_DECODE_H

#include <stdint.h>
#include <stdio.h>

#include <memory>
#include <vector>

#include <vorbis/vorbisfile.h>

namespace NAudioDecode
{

/*  Decoded audio: interleaved 16-bit PCM at its own rate. */
struct SPcm
{
    int                    nChannels;
    int                    nRate;
    std::vector< int16_t > data;
    SPcm() : nChannels( 0 ), nRate( 0 ) {}
    int Frames() const { return nChannels ? (int)( data.size() / nChannels ) : 0; }
};
typedef std::shared_ptr< SPcm > PPcm;

/*  Whole-buffer decoders.  On failure the result is empty and *ppszError
 *  names the reason (a static string). */
PPcm DecodeWav( const uint8_t *p, size_t n, const char **ppszError );
PPcm DecodeOgg( const uint8_t *p, size_t n, const char **ppszError );
PPcm DecodeAny( const void *pData, int nLength, const char **ppszError );   /* sniffs the header */

/*  vorbisfile over a memory buffer */
struct SMemFile
{
    const uint8_t *p;
    size_t n, pos;
};
extern const ov_callbacks MEM_CALLBACKS;
/*  up to nFrames interleaved 16-bit frames; 0 at the end */
int OvReadFrames( OggVorbis_File *pVF, int nChannels, int16_t *pOut, int nFrames );

/*  A stream source: the whole file in memory, decoded on demand (Ogg), or a
 *  WAV decoded up front. */
class CStreamSource
{
public:
    CStreamSource();
    ~CStreamSource();
    /* pszNativePath: a path fopen() can open */
    bool Open( const char *pszNativePath, const char **ppszError );
    /* interleaved frames; 0 at the end */
    int  Read( int16_t *pOut, int nFrames );
    void Rewind();
    int  Channels() const { return nChannels; }
    int  Rate() const { return nRate; }

private:
    CStreamSource( const CStreamSource & );
    CStreamSource &operator=( const CStreamSource & );
    std::vector< uint8_t > file;
    SMemFile       mem;
    OggVorbis_File vf;
    bool           bVorbis;
    PPcm           pcm;
    int            nChannels, nRate;
    size_t         nPcmPos;
};

}  // namespace NAudioDecode

#endif
