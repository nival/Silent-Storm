/*
 *  audio_decode_test.cpp -- check platform/audio_decode.cpp against the retail
 *  sound assets, on the host.
 *
 *      build/host/silentstorm_audiotest [--ffmpeg PATH] FILE...
 *
 *  Every file is decoded with the port's decoder.  RIFF WAVE files (PCM,
 *  Microsoft ADPCM, IMA ADPCM) are also decoded by ffmpeg to signed 16-bit and
 *  compared sample by sample -- the ADPCM decoders were written from the
 *  specifications, this is what proves them.  Ogg Vorbis files are checked
 *  against libvorbis's own length report.  Exit status is the number of files
 *  that failed.
 *
 *      find ../Complete/Sounds -type f | xargs build/host/silentstorm_audiotest
 */
#include "audio_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

using namespace NAudioDecode;

static bool ReadFile( const char *pszPath, std::vector< uint8_t > *pOut )
{
    FILE *f = fopen( pszPath, "rb" );
    if ( !f )
        return false;
    fseek( f, 0, SEEK_END );
    const long n = ftell( f );
    fseek( f, 0, SEEK_SET );
    pOut->resize( n > 0 ? (size_t)n : 0 );
    const size_t nRead = n > 0 ? fread( &( *pOut )[ 0 ], 1, (size_t)n, f ) : 0;
    fclose( f );
    return nRead == (size_t)n;
}

/*  ffmpeg -> raw s16le, via a pipe */
static bool FfmpegDecode( const char *pszFfmpeg, const char *pszPath, int nChannels, std::vector< int16_t > *pOut )
{
    std::string szCmd = std::string( "\"" ) + pszFfmpeg + "\" -v error -i \"" + pszPath + "\" -f s16le -acodec pcm_s16le -ac " +
                        std::to_string( nChannels ) + " - 2>/dev/null";
    FILE *p = popen( szCmd.c_str(), "r" );
    if ( !p )
        return false;
    pOut->clear();
    int16_t buf[ 4096 ];
    size_t n;
    while ( ( n = fread( buf, sizeof( int16_t ), 4096, p ) ) > 0 )
        pOut->insert( pOut->end(), buf, buf + n );
    return pclose( p ) == 0;
}

int main( int argc, char **argv )
{
    const char *pszFfmpeg = "ffmpeg";
    int nFirst = 1;
    if ( argc > 2 && strcmp( argv[ 1 ], "--ffmpeg" ) == 0 )
    {
        pszFfmpeg = argv[ 2 ];
        nFirst = 3;
    }
    if ( nFirst >= argc )
    {
        fprintf( stderr, "usage: %s [--ffmpeg PATH] FILE...\n", argv[ 0 ] );
        return 2;
    }
    int nFailed = 0, nWav = 0, nOgg = 0, nCompared = 0;
    long nMaxDiffAll = 0;
    for ( int a = nFirst; a < argc; ++a )
    {
        const char *pszPath = argv[ a ];
        std::vector< uint8_t > file;
        if ( !ReadFile( pszPath, &file ) )
        {
            printf( "%s: cannot read\n", pszPath );
            ++nFailed;
            continue;
        }
        const char *pszError = 0;
        PPcm pcm = DecodeAny( file.empty() ? 0 : &file[ 0 ], (int)file.size(), &pszError );
        if ( !pcm )
        {
            printf( "%s: FAIL decode: %s\n", pszPath, pszError ? pszError : "?" );
            ++nFailed;
            continue;
        }
        const bool bWav = file.size() >= 4 && memcmp( &file[ 0 ], "RIFF", 4 ) == 0;
        if ( bWav )
        {
            ++nWav;
            std::vector< int16_t > ref;
            if ( !FfmpegDecode( pszFfmpeg, pszPath, pcm->nChannels, &ref ) )
            {
                printf( "%s: ffmpeg failed (%d frames ours)\n", pszPath, pcm->Frames() );
                continue;
            }
            ++nCompared;
            const size_t nOurs = pcm->data.size(), nRef = ref.size();
            const size_t nCommon = nOurs < nRef ? nOurs : nRef;
            long nMaxDiff = 0, nBig = 0;
            for ( size_t i = 0; i < nCommon; ++i )
            {
                const long d = labs( (long)pcm->data[ i ] - (long)ref[ i ] );
                if ( d > nMaxDiff ) nMaxDiff = d;
                if ( d > 1 ) ++nBig;
            }
            if ( nMaxDiff > nMaxDiffAll ) nMaxDiffAll = nMaxDiff;
            /* ADPCM: our block-exact length may differ from ffmpeg's by a few
             * frames at the tail; anything else is a real difference */
            const long nLenDiff = labs( (long)nOurs - (long)nRef );
            const bool bOK = nBig == 0 && nLenDiff <= 64 * pcm->nChannels;
            if ( !bOK )
            {
                printf( "%s: FAIL vs ffmpeg: %zu vs %zu samples, %ld differ by >1, max diff %ld\n", pszPath, nOurs, nRef, nBig, nMaxDiff );
                ++nFailed;
            }
        }
        else
        {
            ++nOgg;
            /* cross-check the length against vorbisfile's own report */
            SMemFile mem = { &file[ 0 ], file.size(), 0 };
            OggVorbis_File vf;
            if ( ov_open_callbacks( &mem, &vf, 0, 0, MEM_CALLBACKS ) == 0 )
            {
                const long nTotal = (long)ov_pcm_total( &vf, -1 );
                ov_clear( &vf );
                if ( nTotal > 0 && nTotal != pcm->Frames() )
                {
                    printf( "%s: FAIL length: decoded %d frames, stream says %ld\n", pszPath, pcm->Frames(), nTotal );
                    ++nFailed;
                }
            }
        }
    }
    printf( "%d files: %d WAV (%d compared with ffmpeg, max sample diff %ld), %d Ogg; %d failed\n",
            argc - nFirst, nWav, nCompared, nMaxDiffAll, nOgg, nFailed );
    return nFailed > 255 ? 255 : nFailed;
}
