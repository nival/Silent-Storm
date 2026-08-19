/*
 *  audio_android.cpp -- the engine's audio interface (NFMSound,
 *  FModSound/FMsound.h) on Android, without FMOD.
 *
 *  Silent Storm played sound through FMOD 3.x; FModSound/FMSound.cpp is a thin
 *  wrapper over it and FMsound.h is the seam -- everything in Main talks to
 *  NFMSound::*, never to FMOD.  This file is that seam implemented on a small
 *  software mixer of its own:
 *
 *    decoders   Sounds/<id> assets are Ogg Vorbis (mono 44.1 kHz, most of them)
 *               or RIFF WAVE -- PCM, Microsoft ADPCM or IMA ADPCM.  Music
 *               streams ("Res\Music\Combat01.wav") are Ogg Vorbis despite the
 *               extension.  Vorbis is the Xiph reference decoder (libogg +
 *               libvorbis under thirdparty/, needed because the game's
 *               2002-vintage encoder used floor type 0, which the small
 *               single-file decoders do not implement); WAV and the two ADPCM
 *               codecs are decoded here.  Every asset is sniffed by its
 *               header, never by name.
 *    mixer      64 sample voices + any number of streams, mixed in float at the
 *               output rate with linear-interpolation resampling, on a thread of
 *               its own.  Streams are decoded on that thread from a copy of
 *               the file held in memory, so a stall in the game loop (a level
 *               load) never starves the music.
 *    output     AAudio (API 26+), loaded with dlopen so the .so still loads on
 *               the minSdk 24 devices -- there the mixer runs against a clock
 *               instead and the game is silent, exactly as with the old null
 *               back end.  Blocking writes on the mixer thread, no real-time
 *               callback: latency of ~50 ms is nothing for a turn-based game and
 *               it keeps decoding off any real-time-scheduled thread.
 *
 *  Semantics follow the FMOD wrapper closely, because Main was written against
 *  its quirks:
 *    * dropping the last CObj<> to a CSound2D/CSound3D stops the voice (the
 *      wrapper's CChannel destructor did FSOUND_StopSound);
 *    * 3D positions go through the listener's projection matrix -- the sound
 *      lives in clip space, x/y doubled, distance = |(2x, 2y, w)| -- with
 *      FMOD's logarithmic rolloff between the sample's min and max distance;
 *    * a stream's IsPlaying() goes false when it ends (non-looping ambient
 *      music), which is what makes CSoundScene restart it after 120 s of
 *      silence; SwitchStream() starts the new stream at once and the old one
 *      is dropped on the next Update();
 *    * volumes are FMOD's 0..255; SFX master scales the voices, the music
 *      volume is absolute per stream; FadeOut() on a stream is in seconds and
 *      stops it at zero; a 3D voice's fade in/out is measured in source samples.
 *
 *  Handles are CObjectBase-derived because Main holds them in CObj<>/CPtr<>.
 */
#include "a5_engine_prologue.h"
#include "FModSound/FMsound.h"
#include "a5_log.h"
#include "audio_android.h"

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <memory>
#include <vector>


#include "audio_decode.h"
using namespace NAudioDecode;

/* ========================================================================== */
/*  AAudio, loaded at run time                                                 */
/* ========================================================================== */
namespace {

typedef struct AAudioStreamBuilderStruct AAudioStreamBuilder;
typedef struct AAudioStreamStruct        AAudioStream;
typedef int32_t aaudio_result_t;
enum
{
    AA_OK = 0,
    AA_ERROR_DISCONNECTED = -899,
    AA_FORMAT_PCM_I16 = 1,
    AA_SHARING_MODE_SHARED = 1,
    AA_PERFORMANCE_MODE_NONE = 10,
    AA_USAGE_GAME = 14,
    AA_CONTENT_TYPE_MUSIC = 2,
    AA_DIRECTION_OUTPUT = 0,
};

struct SAAudio
{
    void *hLib;
    aaudio_result_t ( *createStreamBuilder )( AAudioStreamBuilder ** );
    void ( *builderSetFormat )( AAudioStreamBuilder *, int32_t );
    void ( *builderSetChannelCount )( AAudioStreamBuilder *, int32_t );
    void ( *builderSetSharingMode )( AAudioStreamBuilder *, int32_t );
    void ( *builderSetPerformanceMode )( AAudioStreamBuilder *, int32_t );
    void ( *builderSetDirection )( AAudioStreamBuilder *, int32_t );
    void ( *builderSetUsage )( AAudioStreamBuilder *, int32_t );          /* API 28, may be null */
    void ( *builderSetContentType )( AAudioStreamBuilder *, int32_t );    /* API 28, may be null */
    aaudio_result_t ( *builderOpenStream )( AAudioStreamBuilder *, AAudioStream ** );
    aaudio_result_t ( *builderDelete )( AAudioStreamBuilder * );
    int32_t ( *streamGetSampleRate )( AAudioStream * );
    int32_t ( *streamGetChannelCount )( AAudioStream * );
    int32_t ( *streamGetFormat )( AAudioStream * );
    int32_t ( *streamGetFramesPerBurst )( AAudioStream * );
    int32_t ( *streamGetBufferCapacityInFrames )( AAudioStream * );
    aaudio_result_t ( *streamSetBufferSizeInFrames )( AAudioStream *, int32_t );
    int32_t ( *streamGetXRunCount )( AAudioStream * );
    aaudio_result_t ( *streamRequestStart )( AAudioStream * );
    aaudio_result_t ( *streamRequestStop )( AAudioStream * );
    aaudio_result_t ( *streamClose )( AAudioStream * );
    aaudio_result_t ( *streamWrite )( AAudioStream *, const void *, int32_t, int64_t );
    const char *( *convertResultToText )( aaudio_result_t );

    SAAudio() { memset( this, 0, sizeof( *this ) ); }
    bool Load()
    {
        if ( hLib )
            return true;
        hLib = dlopen( "libaaudio.so", RTLD_NOW );
        if ( !hLib )
            return false;
#define AA_SYM( field, name ) *(void **)&field = dlsym( hLib, name )
        AA_SYM( createStreamBuilder, "AAudio_createStreamBuilder" );
        AA_SYM( builderSetFormat, "AAudioStreamBuilder_setFormat" );
        AA_SYM( builderSetChannelCount, "AAudioStreamBuilder_setChannelCount" );
        AA_SYM( builderSetSharingMode, "AAudioStreamBuilder_setSharingMode" );
        AA_SYM( builderSetPerformanceMode, "AAudioStreamBuilder_setPerformanceMode" );
        AA_SYM( builderSetDirection, "AAudioStreamBuilder_setDirection" );
        AA_SYM( builderSetUsage, "AAudioStreamBuilder_setUsage" );
        AA_SYM( builderSetContentType, "AAudioStreamBuilder_setContentType" );
        AA_SYM( builderOpenStream, "AAudioStreamBuilder_openStream" );
        AA_SYM( builderDelete, "AAudioStreamBuilder_delete" );
        AA_SYM( streamGetSampleRate, "AAudioStream_getSampleRate" );
        AA_SYM( streamGetChannelCount, "AAudioStream_getChannelCount" );
        AA_SYM( streamGetFormat, "AAudioStream_getFormat" );
        AA_SYM( streamGetFramesPerBurst, "AAudioStream_getFramesPerBurst" );
        AA_SYM( streamGetBufferCapacityInFrames, "AAudioStream_getBufferCapacityInFrames" );
        AA_SYM( streamSetBufferSizeInFrames, "AAudioStream_setBufferSizeInFrames" );
        AA_SYM( streamGetXRunCount, "AAudioStream_getXRunCount" );
        AA_SYM( streamRequestStart, "AAudioStream_requestStart" );
        AA_SYM( streamRequestStop, "AAudioStream_requestStop" );
        AA_SYM( streamClose, "AAudioStream_close" );
        AA_SYM( streamWrite, "AAudioStream_write" );
        AA_SYM( convertResultToText, "AAudio_convertResultToText" );
#undef AA_SYM
        const bool bOK = createStreamBuilder && builderSetFormat && builderSetChannelCount && builderSetSharingMode &&
                         builderSetPerformanceMode && builderSetDirection && builderOpenStream && builderDelete &&
                         streamGetSampleRate && streamGetChannelCount && streamGetFormat && streamGetFramesPerBurst &&
                         streamGetBufferCapacityInFrames && streamSetBufferSizeInFrames && streamRequestStart &&
                         streamRequestStop && streamClose && streamWrite;
        if ( !bOK )
        {
            dlclose( hLib );
            hLib = 0;
        }
        return bOK;
    }
    const char *Text( aaudio_result_t r ) { return convertResultToText ? convertResultToText( r ) : "?"; }
};

}  // namespace

/* ========================================================================== */
/*  The mixer                                                                 */
/* ========================================================================== */
namespace NFMSound
{
namespace {

const int N_VOICES = 64;
const int N_BLOCK_FRAMES = 1024;         /* one mix block; also the write granularity */
const int N_STREAM_FIFO = 8192;          /* decoded frames a stream keeps ahead */
const float F_XY_SCALE = 2.0f;           /* the FMOD wrapper's fXYScale */

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
struct SLock
{
    SLock() { pthread_mutex_lock( &g_mutex ); }
    ~SLock() { pthread_mutex_unlock( &g_mutex ); }
};

/*  A sample voice.  Everything below is read and written under g_mutex. */
struct SVoice
{
    bool     bActive;
    unsigned nGen;            /* handle validity: a handle names (index, generation) */
    unsigned nSerial;         /* start order, for stealing the oldest */
    PPcm     pcm;
    double   fPos;            /* frame position in the sample */
    bool     bLoop;
    int      nLoops;          /* SPlayParams::nLoops (-1 = not given) */
    int      nCurrentLoop;
    int      nVolume;         /* 0..255, the requested volume */
    bool     b3D;
    CVec3    vPos;
    float    fMinDist, fMaxDist;
    int      nPriority;
    /* fades, in source samples, as the FMOD wrapper's CSound3D did them */
    bool     bFadeIn, bFadeOut, bFadeOutInProgress;
    float    fFadeVolume, fFadeSpeed;
    int      nFadeSamples;
    /* smoothed output gains, to keep panning/attenuation changes click-free */
    float    fGainL, fGainR;
    bool     bGainInit;

    SVoice() { Reset(); nGen = 1; }
    void Reset()
    {
        bActive = false; nSerial = 0; pcm.reset(); fPos = 0; bLoop = false; nLoops = -1; nCurrentLoop = 0; nVolume = 255;
        b3D = false; vPos = CVec3( 0, 0, 0 ); fMinDist = 1; fMaxDist = 100; nPriority = 128;
        bFadeIn = bFadeOut = bFadeOutInProgress = false; fFadeVolume = 255; fFadeSpeed = 0; nFadeSamples = 0;
        fGainL = fGainR = 0; bGainInit = false;
    }
};

/*  A stream's playback state.  Owned by its CStream (game thread), listed in
 *  g_streams for the mixer; both sides touch it under g_mutex only. */
struct SStreamState
{
    CStreamSource src;
    bool   bPlaying;          /* started and not yet ended/stopped */
    bool   bLoop;
    float  fVolume;           /* 0..255, absolute (music volume) */
    bool   bFadeOut;
    float  fFadeVolume, fFadeSpeed;   /* per second, in 0..255 units */
    std::vector< int16_t > fifo;      /* decoded frames at the source rate */
    int    nFifoFrames;
    double fReadPos;                  /* fractional frame in the fifo */
    bool   bSourceEnded;
    float  fGainL, fGainR;
    bool   bGainInit;
    SStreamState() : bPlaying( false ), bLoop( false ), fVolume( 255 ), bFadeOut( false ), fFadeVolume( 255 ), fFadeSpeed( 0 ),
                     nFifoFrames( 0 ), fReadPos( 0 ), bSourceEnded( false ), fGainL( 0 ), fGainR( 0 ), bGainInit( false ) {}
};

SVoice   g_voices[ N_VOICES ];
unsigned g_nSerial = 0;
std::vector< SStreamState * > g_streams;
float    g_fSfxMaster = 1.0f;          /* 0..1 */
int      g_nMusicVolume = 255;         /* 0..255 */
SHMatrix g_toCamera;
bool     g_bListenerSet = false;
int      g_nOutRate = 48000;           /* whatever the device gave us; the mixer reads it per block */

/* output thread */
pthread_t g_thread;
bool      g_bThreadRunning = false;
volatile bool g_bRun = false;
volatile bool g_bActive = true;        /* foreground */
bool      g_bInitialised = false;
SAAudio   g_aa;
AAudioStream *g_pStream = 0;
int       g_nOutChannels = 2;
int       g_nUnderruns = 0;
int       g_nReopens = 0;
bool      g_bLoggedNoAAudio = false;
float     g_fPeak = 0;                 /* max |sample| since the last stats line (mixer thread writes) */
long      g_nBlocksMixed = 0;
int       g_nTrace = -1;               /* A5_AUDIO_TRACE=1: log every sound start */

bool Trace()
{
    if ( g_nTrace < 0 )
    {
        const char *e = getenv( "A5_AUDIO_TRACE" );
        g_nTrace = e ? atoi( e ) : 0;
    }
    return g_nTrace > 0;
}

/* ---- 3D helpers (the FMOD wrapper's conversion, verbatim) ---------------- */
inline CVec3 ConvertPosToFMode( const CVec3 &_v )
{
    CVec4 v;
    g_toCamera.RotateHVector( &v, _v );
    CVec3 ptPos( v.x * F_XY_SCALE, v.y * F_XY_SCALE, v.w );
    return ptPos;
}

/*  Stereo gains for a voice: FMOD 3's logarithmic rolloff (halving per doubled
 *  distance between min and max, flat outside) and a constant-power pan on the
 *  clip-space x. */
inline void SpatialGains( const SVoice &v, float *pL, float *pR )
{
    float fAtten = 1.0f, fPan = 0.0f;
    if ( v.b3D && g_bListenerSet )
    {
        const CVec3 p = ConvertPosToFMode( v.vPos );
        const float fDist = sqrtf( p.x * p.x + p.y * p.y + p.z * p.z );
        const float fMin = v.fMinDist > 0.0001f ? v.fMinDist : 0.0001f;
        const float fMax = v.fMaxDist > fMin ? v.fMaxDist : fMin;
        if ( fDist > fMin )
            fAtten = fMin / ( fDist < fMax ? fDist : fMax );
        if ( fDist > 0.001f )
        {
            fPan = p.x / fDist;
            /* a source right on top of the listener spreads to both speakers */
            const float fNear = fDist < fMin ? fDist / fMin : 1.0f;
            fPan *= fNear;
        }
    }
    const float fAngle = ( fPan + 1.0f ) * 0.78539816f;   /* 0..pi/2 */
    *pL = fAtten * cosf( fAngle );
    *pR = fAtten * sinf( fAngle );
}

/* ---- mixing ------------------------------------------------------------- */
inline void MixResampled( const int16_t *pData, int nSrcChannels, int nSrcFrames, double *pfPos, double fStep,
                          float *pOut, int nFrames, float fGainL0, float fGainL1, float fGainR0, float fGainR1,
                          bool bLoop, int *pnWraps, bool *pbEnded )
{
    double fPos = *pfPos;
    const float fScale = 1.0f / 32768.0f;
    for ( int i = 0; i < nFrames; ++i )
    {
        if ( fPos >= nSrcFrames )
        {
            if ( !bLoop || nSrcFrames <= 0 )
            {
                *pbEnded = true;
                break;
            }
            fPos -= nSrcFrames;
            ++*pnWraps;
            if ( fPos >= nSrcFrames ) fPos = 0;
        }
        const int    n0 = (int)fPos;
        const float  fFrac = (float)( fPos - n0 );
        int n1 = n0 + 1;
        if ( n1 >= nSrcFrames ) n1 = bLoop ? 0 : n0;
        float l, r;
        if ( nSrcChannels == 1 )
        {
            const float s0 = pData[ n0 ], s1 = pData[ n1 ];
            l = r = ( s0 + ( s1 - s0 ) * fFrac ) * fScale;
        }
        else
        {
            const float l0 = pData[ n0 * 2 ], l1 = pData[ n1 * 2 ];
            const float r0 = pData[ n0 * 2 + 1 ], r1 = pData[ n1 * 2 + 1 ];
            l = ( l0 + ( l1 - l0 ) * fFrac ) * fScale;
            r = ( r0 + ( r1 - r0 ) * fFrac ) * fScale;
        }
        const float t = (float)i / (float)nFrames;
        pOut[ i * 2 ]     += l * ( fGainL0 + ( fGainL1 - fGainL0 ) * t );
        pOut[ i * 2 + 1 ] += r * ( fGainR0 + ( fGainR1 - fGainR0 ) * t );
        fPos += fStep;
    }
    *pfPos = fPos;
}

void MixVoice( SVoice &v, float *pOut, int nFrames, int nOutRate )
{
    const SPcm &pcm = *v.pcm;
    const int nLen = pcm.Frames();
    if ( nLen <= 0 )
    {
        v.bActive = false;
        return;
    }
    /* volume this block: the fade logic of the FMOD wrapper's CSound3D::Update,
     * in source samples */
    const double fPlayed = (double)v.nCurrentLoop * nLen + v.fPos;
    float fVolume = (float)v.nVolume;
    if ( v.bFadeIn )
    {
        v.fFadeVolume = (float)( fPlayed * v.fFadeSpeed );
        if ( v.fFadeVolume >= v.nVolume )
        {
            v.fFadeVolume = (float)v.nVolume;
            v.bFadeIn = false;
        }
        fVolume = v.fFadeVolume;
    }
    if ( v.bFadeOut )
    {
        double fRemaining;
        if ( v.nLoops > 0 )       fRemaining = (double)v.nLoops * nLen - fPlayed;
        else if ( !v.bLoop )      fRemaining = nLen - v.fPos;
        else                      fRemaining = 1e30;
        if ( !v.bFadeOutInProgress && fRemaining <= v.nFadeSamples )
            v.bFadeOutInProgress = true;
        if ( v.bFadeOutInProgress )
        {
            const float fCap = (float)( v.fFadeSpeed * fRemaining );
            if ( fCap < v.fFadeVolume ) v.fFadeVolume = fCap;
            if ( v.fFadeVolume < 0 )
            {
                v.bActive = false;
                return;
            }
            fVolume = v.fFadeVolume;
        }
    }
    float fL, fR;
    SpatialGains( v, &fL, &fR );
    const float fMaster = ( fVolume / 255.0f ) * g_fSfxMaster;
    fL *= fMaster;
    fR *= fMaster;
    if ( !v.bGainInit )
    {
        v.fGainL = fL; v.fGainR = fR; v.bGainInit = true;
    }
    int nWraps = 0;
    bool bEnded = false;
    const double fStep = (double)pcm.nRate / (double)nOutRate;
    MixResampled( &pcm.data[ 0 ], pcm.nChannels, nLen, &v.fPos, fStep, pOut, nFrames,
                  v.fGainL, fL, v.fGainR, fR, v.bLoop, &nWraps, &bEnded );
    v.fGainL = fL;
    v.fGainR = fR;
    if ( bEnded )
    {
        v.bActive = false;
        v.pcm.reset();
        return;
    }
    if ( nWraps )
    {
        v.nCurrentLoop += nWraps;
        if ( v.nLoops > 1 && v.nCurrentLoop >= v.nLoops )
        {
            v.bActive = false;
            v.pcm.reset();
        }
    }
}

/*  Keep at least nNeed frames decoded in the stream's fifo. */
void FillStreamFifo( SStreamState &s, int nNeed )
{
    const int nCh = s.src.Channels();
    /* drop what has been consumed */
    const int nConsumed = (int)s.fReadPos;
    if ( nConsumed > 0 )
    {
        const int nKeep = s.nFifoFrames - nConsumed;
        if ( nKeep > 0 )
            memmove( &s.fifo[ 0 ], &s.fifo[ (size_t)nConsumed * nCh ], (size_t)nKeep * nCh * sizeof( int16_t ) );
        s.nFifoFrames = nKeep > 0 ? nKeep : 0;
        s.fReadPos -= nConsumed;
    }
    if ( nNeed < N_STREAM_FIFO ) nNeed = N_STREAM_FIFO;
    if ( (int)s.fifo.size() < ( nNeed + 1 ) * nCh )
        s.fifo.resize( (size_t)( nNeed + 1 ) * nCh );
    bool bJustRewound = false;
    while ( s.nFifoFrames < nNeed && !s.bSourceEnded )
    {
        const int nGot = s.src.Read( &s.fifo[ (size_t)s.nFifoFrames * nCh ], nNeed - s.nFifoFrames );
        if ( nGot > 0 )
        {
            s.nFifoFrames += nGot;
            bJustRewound = false;
        }
        else if ( s.bLoop && !bJustRewound )
        {
            s.src.Rewind();
            bJustRewound = true;
        }
        else
            s.bSourceEnded = true;      /* end of file, or a loop that yields nothing */
    }
}

void MixStream( SStreamState &s, float *pOut, int nFrames, int nOutRate, double fBlockSeconds )
{
    if ( !s.bPlaying )
        return;
    float fVolume = s.fVolume;
    if ( s.bFadeOut )
    {
        s.fFadeVolume -= (float)( s.fFadeSpeed * fBlockSeconds );
        if ( s.fFadeVolume <= 0.001f )
        {
            s.bPlaying = false;
            return;
        }
        fVolume = s.fFadeVolume;
    }
    const double fStep = (double)s.src.Rate() / (double)nOutRate;
    const int nNeed = (int)( nFrames * fStep ) + 4;
    FillStreamFifo( s, nNeed );
    if ( s.nFifoFrames <= 1 )
    {
        if ( s.bSourceEnded )
            s.bPlaying = false;
        return;
    }
    const float fGain = fVolume / 255.0f;
    if ( !s.bGainInit )
    {
        s.fGainL = s.fGainR = fGain; s.bGainInit = true;
    }
    int nWraps = 0;
    bool bEnded = false;
    /* the fifo is a non-looping window: an end here just means "underfed" */
    MixResampled( &s.fifo[ 0 ], s.src.Channels(), s.nFifoFrames, &s.fReadPos, fStep, pOut, nFrames,
                  s.fGainL, fGain, s.fGainR, fGain, false, &nWraps, &bEnded );
    s.fGainL = s.fGainR = fGain;
    if ( bEnded && s.bSourceEnded )
        s.bPlaying = false;
}

void MixBlock( float *pOut, int nFrames, int nOutRate )
{
    memset( pOut, 0, sizeof( float ) * nFrames * 2 );
    SLock lock;
    for ( int i = 0; i < N_VOICES; ++i )
        if ( g_voices[ i ].bActive )
            MixVoice( g_voices[ i ], pOut, nFrames, nOutRate );
    const double fSeconds = (double)nFrames / (double)nOutRate;
    for ( size_t i = 0; i < g_streams.size(); ++i )
        MixStream( *g_streams[ i ], pOut, nFrames, nOutRate, fSeconds );
}

/* ---- output ------------------------------------------------------------- */
bool OpenOutput()
{
    if ( !g_aa.Load() )
    {
        if ( !g_bLoggedNoAAudio )
        {
            g_bLoggedNoAAudio = true;
            a5_log( A5_PRIORITY_WARN, "audio: libaaudio.so not available (API < 26?) -- mixer runs silent" );
        }
        return false;
    }
    AAudioStreamBuilder *pBuilder = 0;
    aaudio_result_t r = g_aa.createStreamBuilder( &pBuilder );
    if ( r != AA_OK || !pBuilder )
    {
        a5_log( A5_PRIORITY_ERROR, "audio: AAudio_createStreamBuilder: %s", g_aa.Text( r ) );
        return false;
    }
    g_aa.builderSetDirection( pBuilder, AA_DIRECTION_OUTPUT );
    g_aa.builderSetFormat( pBuilder, AA_FORMAT_PCM_I16 );
    g_aa.builderSetChannelCount( pBuilder, 2 );
    g_aa.builderSetSharingMode( pBuilder, AA_SHARING_MODE_SHARED );
    g_aa.builderSetPerformanceMode( pBuilder, AA_PERFORMANCE_MODE_NONE );
    if ( g_aa.builderSetUsage )       g_aa.builderSetUsage( pBuilder, AA_USAGE_GAME );
    if ( g_aa.builderSetContentType ) g_aa.builderSetContentType( pBuilder, AA_CONTENT_TYPE_MUSIC );
    AAudioStream *pStream = 0;
    r = g_aa.builderOpenStream( pBuilder, &pStream );
    g_aa.builderDelete( pBuilder );
    if ( r != AA_OK || !pStream )
    {
        a5_log( A5_PRIORITY_ERROR, "audio: AAudioStreamBuilder_openStream: %s", g_aa.Text( r ) );
        return false;
    }
    const int nRate = g_aa.streamGetSampleRate( pStream );
    const int nChannels = g_aa.streamGetChannelCount( pStream );
    const int nFormat = g_aa.streamGetFormat( pStream );
    const int nBurst = g_aa.streamGetFramesPerBurst( pStream );
    const int nCapacity = g_aa.streamGetBufferCapacityInFrames( pStream );
    if ( nChannels != 2 || nFormat != AA_FORMAT_PCM_I16 || nRate <= 0 )
    {
        a5_log( A5_PRIORITY_ERROR, "audio: stream came back as %d ch, format %d, %d Hz -- unusable", nChannels, nFormat, nRate );
        g_aa.streamClose( pStream );
        return false;
    }
    /* enough buffer for the blocking-write model: a few bursts, at least ~50 ms */
    int nWant = nBurst * 4;
    if ( nWant < nRate / 20 ) nWant = nRate / 20;
    if ( nWant > nCapacity ) nWant = nCapacity;
    g_aa.streamSetBufferSizeInFrames( pStream, nWant );
    r = g_aa.streamRequestStart( pStream );
    if ( r != AA_OK )
    {
        a5_log( A5_PRIORITY_ERROR, "audio: AAudioStream_requestStart: %s", g_aa.Text( r ) );
        g_aa.streamClose( pStream );
        return false;
    }
    g_pStream = pStream;
    g_nOutRate = nRate;
    g_nOutChannels = nChannels;
    a5_log( A5_PRIORITY_INFO, "audio: AAudio output %d Hz, %d ch, burst %d, buffer %d/%d frames%s",
            nRate, nChannels, nBurst, nWant, nCapacity, g_nReopens ? " (reopened)" : "" );
    return true;
}

void CloseOutput()
{
    if ( g_pStream )
    {
        g_aa.streamRequestStop( g_pStream );
        g_aa.streamClose( g_pStream );
        g_pStream = 0;
    }
}

void SleepMs( int nMs )
{
    struct timespec ts;
    ts.tv_sec = nMs / 1000;
    ts.tv_nsec = (long)( nMs % 1000 ) * 1000000L;
    nanosleep( &ts, 0 );
}

void *MixerThread( void * )
{
    std::vector< float >   mix( N_BLOCK_FRAMES * 2 );
    std::vector< int16_t > out( N_BLOCK_FRAMES * 2 );
    bool bWasActive = true;
    int nRetryMs = 0;
    while ( g_bRun )
    {
        if ( !g_bActive )
        {
            if ( bWasActive )
            {
                CloseOutput();
                bWasActive = false;
            }
            SleepMs( 50 );
            continue;
        }
        bWasActive = true;
        if ( !g_pStream )
        {
            if ( nRetryMs > 0 )
            {
                SleepMs( 50 );
                nRetryMs -= 50;
                /* no device: keep the sounds' clocks moving so IsPlaying() ends */
                for ( int nLeft = g_nOutRate / 20; nLeft > 0; nLeft -= N_BLOCK_FRAMES )
                    MixBlock( &mix[ 0 ], nLeft < N_BLOCK_FRAMES ? nLeft : N_BLOCK_FRAMES, g_nOutRate );
                continue;
            }
            if ( !OpenOutput() )
            {
                nRetryMs = g_aa.hLib ? 2000 : 1000000;   /* no AAudio at all: don't keep trying */
                continue;
            }
        }
        const int nRate = g_nOutRate;
        MixBlock( &mix[ 0 ], N_BLOCK_FRAMES, nRate );
        float fPeak = g_fPeak;
        for ( int i = 0; i < N_BLOCK_FRAMES * 2; ++i )
        {
            float f = mix[ i ];
            if ( f > 1.0f ) f = 1.0f; else if ( f < -1.0f ) f = -1.0f;
            const float fAbs = f < 0 ? -f : f;
            if ( fAbs > fPeak ) fPeak = fAbs;
            out[ i ] = (int16_t)lrintf( f * 32767.0f );
        }
        g_fPeak = fPeak;
        ++g_nBlocksMixed;
        int nWritten = 0;
        while ( nWritten < N_BLOCK_FRAMES && g_bRun && g_bActive )
        {
            const aaudio_result_t r = g_aa.streamWrite( g_pStream, &out[ (size_t)nWritten * 2 ], N_BLOCK_FRAMES - nWritten, 1000000000LL );
            if ( r < 0 )
            {
                a5_log( A5_PRIORITY_WARN, "audio: AAudioStream_write: %s -- reopening", g_aa.Text( r ) );
                CloseOutput();
                ++g_nReopens;
                nRetryMs = 200;
                break;
            }
            nWritten += r;
        }
        if ( g_pStream && g_aa.streamGetXRunCount )
            g_nUnderruns = g_aa.streamGetXRunCount( g_pStream );
    }
    CloseOutput();
    return 0;
}

void StartThread()
{
    if ( g_bThreadRunning )
        return;
    g_bRun = true;
    if ( pthread_create( &g_thread, 0, MixerThread, 0 ) == 0 )
        g_bThreadRunning = true;
    else
    {
        g_bRun = false;
        a5_log( A5_PRIORITY_ERROR, "audio: cannot start the mixer thread (%s)", strerror( errno ) );
    }
}

void StopThread()
{
    if ( !g_bThreadRunning )
        return;
    g_bRun = false;
    pthread_join( g_thread, 0 );
    g_bThreadRunning = false;
}

/* ---- voices ------------------------------------------------------------- */
/*  A handle to a voice.  Its destructor stops the voice, which is how the
 *  FMOD wrapper behaved (CChannel::~CChannel -> FSOUND_StopSound) and what Main
 *  relies on to silence a looping sound: it drops the CObj<>. */
struct SVoiceRef
{
    int      nIndex;
    unsigned nGen;
    SVoiceRef() : nIndex( -1 ), nGen( 0 ) {}
    SVoiceRef( const SVoiceRef & ) : nIndex( -1 ), nGen( 0 ) {}   /* a copy owns nothing */
    SVoiceRef &operator=( const SVoiceRef & ) { return *this; }
    ~SVoiceRef() { Stop(); }
    SVoice *Get() const     /* under g_mutex */
    {
        if ( nIndex < 0 )
            return 0;
        SVoice &v = g_voices[ nIndex ];
        return v.nGen == nGen ? &v : 0;
    }
    void Stop()
    {
        if ( nIndex < 0 )
            return;
        SLock lock;
        SVoice *pV = Get();
        if ( pV && pV->bActive )
        {
            pV->bActive = false;
            pV->pcm.reset();
        }
        nIndex = -1;
    }
    bool IsPlaying() const
    {
        if ( nIndex < 0 )
            return false;
        SLock lock;
        const SVoice *pV = Get();
        return pV && pV->bActive;
    }
};

/*  Find a voice for a new sound: a free one, else steal the least important
 *  (FMOD 3: higher priority number = less important), oldest first. */
int AllocVoice( int nPriority )
{
    for ( int i = 0; i < N_VOICES; ++i )
        if ( !g_voices[ i ].bActive )
            return i;
    int nBest = -1;
    for ( int i = 0; i < N_VOICES; ++i )
    {
        const SVoice &v = g_voices[ i ];
        if ( v.nPriority < nPriority )
            continue;
        if ( nBest < 0 || v.nPriority > g_voices[ nBest ].nPriority ||
             ( v.nPriority == g_voices[ nBest ].nPriority && v.nSerial < g_voices[ nBest ].nSerial ) )
            nBest = i;
    }
    return nBest;
}

bool StartVoice( SVoiceRef *pRef, const PPcm &pcm, int nPriority, SVoice **ppVoice )
{
    const int nIndex = AllocVoice( nPriority );
    if ( nIndex < 0 )
        return false;
    SVoice &v = g_voices[ nIndex ];
    const unsigned nGen = v.nGen + 1;
    v.Reset();
    v.nGen = nGen ? nGen : 1;
    v.nSerial = ++g_nSerial;
    v.pcm = pcm;
    v.nPriority = nPriority;
    pRef->nIndex = nIndex;
    pRef->nGen = v.nGen;
    *ppVoice = &v;
    return true;
}

}  // namespace

/* ========================================================================== */
/*  The NFMSound handle classes                                                */
/* ========================================================================== */
class CSample2D : public CObjectBase
{
    OBJECT_BASIC_METHODS( CSample2D );
public:
    PPcm pcm;
};
class CSample3D : public CObjectBase
{
    OBJECT_BASIC_METHODS( CSample3D );
public:
    PPcm  pcm;
    float fMinDistance, fMaxDistance;
    int   nPriority;
    CSample3D() : fMinDistance( 1 ), fMaxDistance( 100 ), nPriority( 128 ) {}
    bool IsEmpty() const { return !pcm; }
};
class CSound2D : public CObjectBase
{
    OBJECT_BASIC_METHODS( CSound2D );
public:
    SVoiceRef voice;
};
class CSound3D : public CObjectBase, public ISound3D
{
    OBJECT_BASIC_METHODS( CSound3D );
public:
    SVoiceRef voice;
    virtual void SetPosition( const CVec3 &pos )
    {
        SLock lock;
        if ( SVoice *pV = voice.Get() )
            pV->vPos = pos;
    }
};
class CStream : public CObjectBase
{
    OBJECT_NOCOPY_METHODS( CStream );
public:
    CStream() : pState( 0 ), bClose( false ) {}
    virtual ~CStream() { Close(); }

    bool Play( const char *pszName, bool bLoop )
    {
        Close();
        szFileName = pszName ? pszName : "";
        SStreamState *pS = new SStreamState;
        const char *pszError = 0;
        char szNative[ 1024 ];
        a5_resolve_path( szFileName.c_str(), szNative, sizeof( szNative ) );
        if ( !pS->src.Open( szNative, &pszError ) )
        {
            a5_log( A5_PRIORITY_WARN, "audio: stream \"%s\": %s", szFileName.c_str(), pszError ? pszError : "?" );
            delete pS;
            return false;
        }
        pS->bLoop = bLoop;
        pS->bPlaying = true;
        pS->fVolume = (float)g_nMusicVolume;
        pS->fFadeVolume = pS->fVolume;
        {
            SLock lock;
            g_streams.push_back( pS );
        }
        pState = pS;
        a5_log( A5_PRIORITY_INFO, "audio: stream \"%s\" %s, %d Hz %d ch", szFileName.c_str(), bLoop ? "looping" : "once", pS->src.Rate(), pS->src.Channels() );
        return true;
    }
    void Close()
    {
        if ( !pState )
            return;
        {
            SLock lock;
            for ( size_t i = 0; i < g_streams.size(); ++i )
                if ( g_streams[ i ] == pState )
                {
                    g_streams.erase( g_streams.begin() + i );
                    break;
                }
        }
        delete pState;
        pState = 0;
    }
    bool IsPlaying() const
    {
        if ( bClose || !pState )
            return false;
        SLock lock;
        return pState->bPlaying;
    }
    void FadeOut( float fSec )
    {
        if ( !pState )
            return;
        SLock lock;
        if ( pState->bFadeOut )
            return;
        pState->bFadeOut = true;
        pState->fFadeVolume = pState->fVolume;
        pState->fFadeSpeed = fSec > 0.01f ? pState->fVolume / fSec : pState->fVolume * 100.0f;
    }
    void CancelFadeOut()
    {
        if ( !pState )
            return;
        SLock lock;
        pState->bFadeOut = false;
        pState->fVolume = (float)g_nMusicVolume;
        pState->fFadeVolume = pState->fVolume;
    }
    void SetVolume( int n )
    {
        if ( !pState )
            return;
        SLock lock;
        pState->fVolume = (float)n;
        if ( !pState->bFadeOut )
            pState->fFadeVolume = pState->fVolume;
    }
    void MarkClosed() { bClose = true; }
    bool IsFadingOut() const
    {
        if ( !pState )
            return false;
        SLock lock;
        return pState->bFadeOut;
    }
    const string &GetFileName() const { return szFileName; }

private:
    string        szFileName;
    SStreamState *pState;
    bool          bClose;    /* superseded by SwitchStream: reports "not playing" and gets dropped */
};

/* ========================================================================== */
/*  NFMSound                                                                    */
/* ========================================================================== */
CDriversInfo drivers;

namespace
{
typedef list< CMObj< CStream > > CStreamList;
CStreamList g_streamObjects;      /* what keeps the CStream objects alive (Main holds CPtr<>) */
}

bool SearchDevices()
{
    drivers.clear();
    SDriverInfo info;
    info.sName = "AAudio";
    info.isHardware3DAccelerated = false;
    info.supportEAXReverb = false;
    info.supportA3DOcclusions = false;
    info.supportA3DReflections = false;
    info.supportReverb = false;
    drivers.push_back( info );
    return true;
}

bool Init( const SStartInfo &info )
{
    if ( g_bInitialised )
        return true;
    {
        SLock lock;
        for ( int i = 0; i < N_VOICES; ++i )
        {
            const unsigned nGen = g_voices[ i ].nGen;
            g_voices[ i ].Reset();
            g_voices[ i ].nGen = nGen;
        }
        g_bListenerSet = false;
    }
    g_bInitialised = true;
    StartThread();
    a5_log( A5_PRIORITY_INFO, "audio: mixer up (%d voices, %d channels requested by the engine, %d Hz mix rate requested)",
            N_VOICES, info.nMaxChannels, info.nMixrate );
    return true;
}

void Done()
{
    if ( !g_bInitialised )
        return;
    StopThread();
    {
        SLock lock;
        for ( int i = 0; i < N_VOICES; ++i )
        {
            g_voices[ i ].bActive = false;
            g_voices[ i ].pcm.reset();
        }
    }
    g_streamObjects.clear();      /* destroys the CStreams -> their states leave g_streams */
    g_bInitialised = false;
}

bool IsInitialized() { return g_bInitialised; }

void Update( const SListener &listener )
{
    if ( !g_bInitialised )
        return;
    {
        SLock lock;
        g_toCamera = listener.toProjective;
        g_bListenerSet = true;
    }
    /* streams that ended, were faded out or superseded: drop the objects (Main
     * holds only CPtr<>s, which go invalid with them) */
    for ( CStreamList::iterator i = g_streamObjects.begin(); i != g_streamObjects.end(); )
    {
        if ( !IsValid( *i ) || !( *i )->IsPlaying() )
            i = g_streamObjects.erase( i );
        else
            ++i;
    }
}

CSample2D *LoadSample2D( const void *pData, int nLength )
{
    if ( !g_bInitialised )
        return 0;
    const char *pszError = 0;
    PPcm pcm = DecodeAny( pData, nLength, &pszError );
    if ( !pcm )
    {
        a5_log( A5_PRIORITY_WARN, "audio: 2D sample (%d bytes): %s", nLength, pszError ? pszError : "?" );
        return 0;
    }
    CSample2D *p = new CSample2D;
    p->pcm = pcm;
    return p;
}

CSample3D *LoadSample3D( const void *pData, int nLength, float fMinDistance, float fMaxDistance, int nPriority )
{
    if ( !g_bInitialised )
        return 0;
    const char *pszError = 0;
    PPcm pcm = DecodeAny( pData, nLength, &pszError );
    if ( !pcm )
    {
        a5_log( A5_PRIORITY_WARN, "audio: 3D sample (%d bytes): %s", nLength, pszError ? pszError : "?" );
        return 0;
    }
    CSample3D *p = new CSample3D;
    p->pcm = pcm;
    p->fMinDistance = fMinDistance;
    p->fMaxDistance = fMaxDistance;
    p->nPriority = nPriority;
    return p;
}

CSample3D *GetDefault3DSound()
{
    return new CSample3D();       /* empty: Play3DSound() ignores it, as the wrapper did */
}

CSound2D *PlaySound( CSample2D *pSample )
{
    if ( !g_bInitialised )
        return 0;
    if ( !IsValid( pSample ) )
        return 0;
    CSound2D *pSound = new CSound2D;
    if ( pSample->pcm )
    {
        SLock lock;
        SVoice *pV = 0;
        if ( StartVoice( &pSound->voice, pSample->pcm, 128, &pV ) )
        {
            pV->b3D = false;
            pV->nVolume = 255;
            pV->bActive = true;
            if ( Trace() )
                a5_log( A5_PRIORITY_DEBUG, "audio: 2D sound: voice %d, %d frames %d Hz %d ch", pSound->voice.nIndex, pSample->pcm->Frames(), pSample->pcm->nRate, pSample->pcm->nChannels );
        }
        else
            a5_log( A5_PRIORITY_WARN, "audio: no free voice for a 2D sound" );
    }
    return pSound;
}

CSound3D *Play3DSound( const SPlayParams &params )
{
    if ( !g_bInitialised )
        return 0;
    if ( !IsValid( params.pSample ) )
    {
        ASSERT( 0 );
        return 0;
    }
    if ( params.pSample->IsEmpty() )
        return 0;
    bool bLoop = params.bLoop;
    if ( params.nLoops != -1 )
        bLoop = params.nLoops > 1;
    CSound3D *pSound = new CSound3D;
    SLock lock;
    SVoice *pV = 0;
    if ( !StartVoice( &pSound->voice, params.pSample->pcm, params.pSample->nPriority, &pV ) )
    {
        a5_log( A5_PRIORITY_WARN, "audio: no free voice for a 3D sound" );
        return pSound;
    }
    pV->b3D = true;
    pV->vPos = params.position;
    pV->fMinDist = params.pSample->fMinDistance;
    pV->fMaxDist = params.pSample->fMaxDistance;
    pV->bLoop = bLoop;
    pV->nLoops = params.nLoops;
    pV->nVolume = params.nVolume < 0 ? 0 : ( params.nVolume > 255 ? 255 : params.nVolume );
    pV->fFadeVolume = (float)pV->nVolume;
    if ( params.nFadeSamples > 0 && ( params.bFadeIn || params.bFadeOut ) )
    {
        pV->fFadeSpeed = (float)pV->nVolume / (float)params.nFadeSamples;
        pV->nFadeSamples = params.nFadeSamples;
        if ( params.bFadeIn )
        {
            pV->bFadeIn = true;
            pV->fFadeVolume = 0;
        }
        /* the wrapper: no fade-out for an endless loop */
        if ( params.bFadeOut && !( params.nLoops == -1 && bLoop ) )
            pV->bFadeOut = true;
    }
    pV->bActive = true;
    if ( Trace() )
        a5_log( A5_PRIORITY_DEBUG, "audio: 3D sound: voice %d, %d frames, vol %d, loop %d (n %d), fade in %d out %d (%d), pos (%.1f %.1f %.1f) min %.1f max %.1f pri %d",
                pSound->voice.nIndex, params.pSample->pcm->Frames(), pV->nVolume, (int)bLoop, params.nLoops, (int)params.bFadeIn, (int)params.bFadeOut,
                params.nFadeSamples, params.position.x, params.position.y, params.position.z, pV->fMinDist, pV->fMaxDist, pV->nPriority );
    return pSound;
}

CStream *PlayStream( const char *pszName, bool bLoop )
{
    if ( !g_bInitialised )
        return 0;
    /*  One copy of a track at a time.  Every CRenderBaseInterface (main menu,
     *  side menu, hero menu, ...) creates its own sound scene, and a scene
     *  starts its ambient music on its first Draw() -- with a fresh FMOD
     *  stream each time, so pushing the side menu over the main menu would
     *  layer a second Mainmenu.wav a few seconds behind the first.  Handing
     *  the new scene the stream that is already playing that file keeps the
     *  music continuous instead. */
    for ( CStreamList::iterator i = g_streamObjects.begin(); i != g_streamObjects.end(); ++i )
        if ( IsValid( *i ) && ( *i )->IsPlaying() && !( *i )->IsFadingOut() && ( *i )->GetFileName() == pszName )
        {
            if ( Trace() )
                a5_log( A5_PRIORITY_DEBUG, "audio: stream \"%s\" already playing -- shared", pszName );
            return ( *i ).GetPtr();
        }
    CStream *pRes = new CStream;
    pRes->Play( pszName, bLoop );
    g_streamObjects.push_back( pRes );
    return pRes;
}

CStream *SwitchStream( CStream *pOldStream, const char *pszNameNewStream, bool bLoop )
{
    if ( !IsValid( pOldStream ) )
        return PlayStream( pszNameNewStream, bLoop );
    for ( CStreamList::iterator i = g_streamObjects.begin(); i != g_streamObjects.end(); ++i )
    {
        if ( IsValid( *i ) && ( *i ).GetPtr() == pOldStream )
        {
            CStream *pNew = new CStream;
            pNew->Play( pszNameNewStream, bLoop );
            g_streamObjects.push_back( pNew );
            pOldStream->MarkClosed();     /* dropped on the next Update() */
            return pNew;
        }
    }
    return 0;
}

bool IsPlaying( CStream *pStream )
{
    for ( CStreamList::iterator i = g_streamObjects.begin(); i != g_streamObjects.end(); ++i )
        if ( IsValid( *i ) && ( *i ).GetPtr() == pStream )
            return pStream->IsPlaying();
    return false;
}
bool IsPlaying( CSound2D *pSound ) { return IsValid( pSound ) && pSound->voice.IsPlaying(); }
bool IsPlaying( CSound3D *pSound ) { return IsValid( pSound ) && pSound->voice.IsPlaying(); }

void FadeOut( CStream *pStream, float fSec )
{
    for ( CStreamList::iterator i = g_streamObjects.begin(); i != g_streamObjects.end(); ++i )
        if ( IsValid( *i ) && ( *i ).GetPtr() == pStream )
            pStream->FadeOut( fSec );
}

void CancelFadeOut( CStream *pStream )
{
    for ( CStreamList::iterator i = g_streamObjects.begin(); i != g_streamObjects.end(); ++i )
        if ( IsValid( *i ) && ( *i ).GetPtr() == pStream )
            pStream->CancelFadeOut();
}

void SetSFXMasterVolume( int nSFX )
{
    SLock lock;
    g_fSfxMaster = ( nSFX < 0 ? 0 : ( nSFX > 255 ? 255 : nSFX ) ) / 255.0f;
}

void SetMusicMasterVolume( int nMusic )
{
    g_nMusicVolume = nMusic < 0 ? 0 : ( nMusic > 255 ? 255 : nMusic );
    for ( CStreamList::iterator i = g_streamObjects.begin(); i != g_streamObjects.end(); ++i )
        if ( IsValid( *i ) )
            ( *i )->SetVolume( g_nMusicVolume );
}

void SetSpeakerType( ESpeakerType ) {}

}  // namespace NFMSound

BASIC_REGISTER_CLASS( NFMSound::CSample2D );
BASIC_REGISTER_CLASS( NFMSound::CSample3D );
BASIC_REGISTER_CLASS( NFMSound::CSound2D );
BASIC_REGISTER_CLASS( NFMSound::CSound3D );
BASIC_REGISTER_CLASS( NFMSound::CStream );

/* ========================================================================== */
/*  Platform hooks                                                             */
/* ========================================================================== */
extern "C" void a5_audio_set_active( int bActive )
{
    NFMSound::g_bActive = bActive != 0;
}

extern "C" void a5_audio_log_stats( void )
{
    using namespace NFMSound;
    int nVoices = 0, n3D = 0, nStreams = 0;
    {
        SLock lock;
        for ( int i = 0; i < N_VOICES; ++i )
            if ( g_voices[ i ].bActive )
            {
                ++nVoices;
                if ( g_voices[ i ].b3D ) ++n3D;
            }
        for ( size_t i = 0; i < g_streams.size(); ++i )
            if ( g_streams[ i ]->bPlaying ) ++nStreams;
    }
    const float fPeak = g_fPeak;
    g_fPeak = 0;
    a5_log( A5_PRIORITY_INFO, "audio: %s, %d voices (%d 3D), %d streams, out %d Hz, peak %.2f, %ld blocks, underruns %d, reopens %d, sfx %.2f music %d",
            g_bInitialised ? ( g_pStream ? "playing" : "no output" ) : "not initialised",
            nVoices, n3D, nStreams, g_nOutRate, fPeak, g_nBlocksMixed, g_nUnderruns, g_nReopens, g_fSfxMaster, g_nMusicVolume );
}
