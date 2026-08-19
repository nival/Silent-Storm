/*
 *  audio_decode.cpp -- see audio_decode.h.
 */
#include "audio_decode.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace NAudioDecode
{
namespace {

inline uint16_t RdU16( const uint8_t *p ) { return (uint16_t)( p[ 0 ] | ( p[ 1 ] << 8 ) ); }
inline int16_t  RdS16( const uint8_t *p ) { return (int16_t)RdU16( p ); }
inline uint32_t RdU32( const uint8_t *p ) { return (uint32_t)p[ 0 ] | ( (uint32_t)p[ 1 ] << 8 ) | ( (uint32_t)p[ 2 ] << 16 ) | ( (uint32_t)p[ 3 ] << 24 ); }
inline int16_t  ClampS16( int v ) { return (int16_t)( v < -32768 ? -32768 : ( v > 32767 ? 32767 : v ) ); }

/* ---- Microsoft ADPCM (WAVE_FORMAT_ADPCM, 2) --------------------------------- */
const int MS_ADAPT[ 16 ] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };
const int MS_COEF_DEFAULT[ 7 ][ 2 ] = { { 256, 0 }, { 512, -256 }, { 0, 0 }, { 192, 64 }, { 240, 0 }, { 460, -208 }, { 392, -232 } };

void DecodeMsAdpcm( const uint8_t *p, size_t nBytes, int nChannels, int nBlockAlign, int nSamplesPerBlock,
                    const int ( *coef )[ 2 ], int nCoef, std::vector< int16_t > *pOut )
{
    if ( nChannels < 1 || nChannels > 2 || nBlockAlign < 7 * nChannels )
        return;
    const size_t nBlocks = nBytes / nBlockAlign;
    pOut->reserve( nBlocks * nSamplesPerBlock * nChannels );
    for ( size_t b = 0; b < nBlocks; ++b )
    {
        const uint8_t *blk = p + b * nBlockAlign;
        const uint8_t *end = blk + nBlockAlign;
        int pred[ 2 ], delta[ 2 ], s1[ 2 ], s2[ 2 ], c1[ 2 ], c2[ 2 ];
        const uint8_t *q = blk;
        for ( int c = 0; c < nChannels; ++c ) { pred[ c ] = *q++; if ( pred[ c ] >= nCoef ) pred[ c ] = 0; c1[ c ] = coef[ pred[ c ] ][ 0 ]; c2[ c ] = coef[ pred[ c ] ][ 1 ]; }
        for ( int c = 0; c < nChannels; ++c ) { delta[ c ] = RdS16( q ); q += 2; }
        for ( int c = 0; c < nChannels; ++c ) { s1[ c ] = RdS16( q ); q += 2; }
        for ( int c = 0; c < nChannels; ++c ) { s2[ c ] = RdS16( q ); q += 2; }
        /* the header carries the first two samples of each channel */
        for ( int c = 0; c < nChannels; ++c ) pOut->push_back( (int16_t)s2[ c ] );
        for ( int c = 0; c < nChannels; ++c ) pOut->push_back( (int16_t)s1[ c ] );
        int nWritten = 2;
        int nChan = 0;
        while ( q < end && nWritten < nSamplesPerBlock )
        {
            const uint8_t byte = *q++;
            for ( int half = 0; half < 2 && nWritten < nSamplesPerBlock; ++half )
            {
                const int nib = half == 0 ? ( byte >> 4 ) : ( byte & 15 );
                const int sn = nib >= 8 ? nib - 16 : nib;
                const int c = nChan;
                /* C division, truncating toward zero: what Windows' codec and
                 * ffmpeg do; an arithmetic shift (floor) drifts by 1 LSB per
                 * step and the block ends hundreds of units off */
                int v = ( ( s1[ c ] * c1[ c ] ) + ( s2[ c ] * c2[ c ] ) ) / 256;
                v += sn * delta[ c ];
                v = v < -32768 ? -32768 : ( v > 32767 ? 32767 : v );
                s2[ c ] = s1[ c ];
                s1[ c ] = v;
                delta[ c ] = ( MS_ADAPT[ nib ] * delta[ c ] ) >> 8;
                if ( delta[ c ] < 16 ) delta[ c ] = 16;
                pOut->push_back( (int16_t)v );
                if ( ++nChan == nChannels ) { nChan = 0; ++nWritten; }
            }
        }
    }
}

/* ---- IMA / DVI ADPCM (WAVE_FORMAT_DVI_ADPCM, 0x11) ------------------------- */
const int IMA_INDEX[ 16 ] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
const int IMA_STEP[ 89 ] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166,
    1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
    8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

inline int16_t ImaStep( int nib, int *pPred, int *pIndex )
{
    const int step = IMA_STEP[ *pIndex ];
    int diff = step >> 3;
    if ( nib & 4 ) diff += step;
    if ( nib & 2 ) diff += step >> 1;
    if ( nib & 1 ) diff += step >> 2;
    int v = *pPred + ( ( nib & 8 ) ? -diff : diff );
    v = v < -32768 ? -32768 : ( v > 32767 ? 32767 : v );
    *pPred = v;
    *pIndex += IMA_INDEX[ nib ];
    if ( *pIndex < 0 ) *pIndex = 0; else if ( *pIndex > 88 ) *pIndex = 88;
    return (int16_t)v;
}

void DecodeImaAdpcm( const uint8_t *p, size_t nBytes, int nChannels, int nBlockAlign, int nSamplesPerBlock, std::vector< int16_t > *pOut )
{
    if ( nChannels < 1 || nChannels > 2 || nBlockAlign < 4 * nChannels )
        return;
    const size_t nBlocks = nBytes / nBlockAlign;
    pOut->reserve( nBlocks * nSamplesPerBlock * nChannels );
    std::vector< int16_t > frame( nChannels * 8 );
    for ( size_t b = 0; b < nBlocks; ++b )
    {
        const uint8_t *blk = p + b * nBlockAlign;
        const uint8_t *end = blk + nBlockAlign;
        int pred[ 2 ], index[ 2 ];
        const uint8_t *q = blk;
        for ( int c = 0; c < nChannels; ++c )
        {
            pred[ c ] = RdS16( q );
            index[ c ] = q[ 2 ];
            if ( index[ c ] > 88 ) index[ c ] = 88;
            q += 4;
        }
        for ( int c = 0; c < nChannels; ++c ) pOut->push_back( (int16_t)pred[ c ] );
        int nWritten = 1;
        /* data: 4 bytes (8 samples) per channel, channels interleaved by word */
        while ( q + 4 * nChannels <= end && nWritten < nSamplesPerBlock )
        {
            for ( int c = 0; c < nChannels; ++c )
                for ( int i = 0; i < 4; ++i )
                {
                    const uint8_t byte = q[ c * 4 + i ];
                    frame[ ( i * 2 ) * nChannels + c ]     = ImaStep( byte & 15, &pred[ c ], &index[ c ] );
                    frame[ ( i * 2 + 1 ) * nChannels + c ] = ImaStep( byte >> 4, &pred[ c ], &index[ c ] );
                }
            q += 4 * nChannels;
            for ( int s = 0; s < 8 && nWritten < nSamplesPerBlock; ++s, ++nWritten )
                for ( int c = 0; c < nChannels; ++c )
                    pOut->push_back( frame[ s * nChannels + c ] );
        }
    }
}

}  // namespace

/* ---- RIFF WAVE ---------------------------------------------------------- */
PPcm DecodeWav( const uint8_t *p, size_t n, const char **ppszError )
{
    *ppszError = 0;
    if ( n < 12 || memcmp( p, "RIFF", 4 ) != 0 || memcmp( p + 8, "WAVE", 4 ) != 0 )
    {
        *ppszError = "not a RIFF WAVE";
        return PPcm();
    }
    int nFormat = 0, nChannels = 0, nRate = 0, nBits = 0, nBlockAlign = 0, nSamplesPerBlock = 0, nCoef = 0;
    int coef[ 32 ][ 2 ];
    const uint8_t *pData = 0;
    size_t nData = 0;
    size_t pos = 12;
    while ( pos + 8 <= n )
    {
        const uint32_t nSize = RdU32( p + pos + 4 );
        const uint8_t *body = p + pos + 8;
        const size_t nAvail = n - ( pos + 8 );
        const size_t nBody = nSize < nAvail ? nSize : nAvail;
        if ( memcmp( p + pos, "fmt ", 4 ) == 0 && nBody >= 16 )
        {
            nFormat     = RdU16( body );
            nChannels   = RdU16( body + 2 );
            nRate       = (int)RdU32( body + 4 );
            nBlockAlign = RdU16( body + 12 );
            nBits       = RdU16( body + 14 );
            if ( nFormat == 0xFFFE && nBody >= 26 )      /* WAVE_FORMAT_EXTENSIBLE: sub-format GUID */
                nFormat = RdU16( body + 24 );
            if ( ( nFormat == 2 || nFormat == 0x11 ) && nBody >= 20 )
                nSamplesPerBlock = RdU16( body + 18 );
            if ( nFormat == 2 )
            {
                nCoef = nBody >= 22 ? RdU16( body + 20 ) : 0;
                if ( nCoef > 32 ) nCoef = 32;
                if ( nCoef == 0 || nBody < (size_t)( 22 + nCoef * 4 ) )
                {
                    nCoef = 7;
                    memcpy( coef, MS_COEF_DEFAULT, sizeof( MS_COEF_DEFAULT ) );
                }
                else
                    for ( int i = 0; i < nCoef; ++i )
                    {
                        coef[ i ][ 0 ] = RdS16( body + 22 + i * 4 );
                        coef[ i ][ 1 ] = RdS16( body + 24 + i * 4 );
                    }
            }
        }
        else if ( memcmp( p + pos, "data", 4 ) == 0 )
        {
            pData = body;
            nData = nBody;
        }
        pos += 8 + nBody + ( nBody & 1 );
    }
    if ( !nChannels || !nRate || !pData )
    {
        *ppszError = "WAVE without fmt/data";
        return PPcm();
    }
    PPcm pcm( new SPcm );
    pcm->nChannels = nChannels;
    pcm->nRate = nRate;
    switch ( nFormat )
    {
        case 1:     /* PCM */
        case 3:     /* IEEE float */
        {
            const int nBytes = nBits / 8;
            if ( nBytes < 1 || nBytes > 4 || ( nFormat == 3 && nBytes != 4 ) )
            {
                *ppszError = "unsupported PCM width";
                return PPcm();
            }
            const size_t nSamples = nData / nBytes;
            pcm->data.resize( nSamples );
            for ( size_t i = 0; i < nSamples; ++i )
            {
                const uint8_t *s = pData + i * nBytes;
                int16_t v;
                if ( nFormat == 3 )
                {
                    float f;
                    memcpy( &f, s, 4 );
                    v = ClampS16( (int)lrintf( f * 32767.0f ) );
                }
                else if ( nBytes == 1 ) v = (int16_t)( ( (int)s[ 0 ] - 128 ) << 8 );
                else if ( nBytes == 2 ) v = RdS16( s );
                else                    v = RdS16( s + nBytes - 2 );   /* top 16 bits of 24/32 */
                pcm->data[ i ] = v;
            }
            break;
        }
        case 2:
            if ( !nSamplesPerBlock ) nSamplesPerBlock = ( nBlockAlign - 7 * nChannels ) * 2 / nChannels + 2;
            DecodeMsAdpcm( pData, nData, nChannels, nBlockAlign, nSamplesPerBlock, coef, nCoef, &pcm->data );
            break;
        case 0x11:
            if ( !nSamplesPerBlock ) nSamplesPerBlock = ( nBlockAlign - 4 * nChannels ) * 2 / nChannels + 1;
            DecodeImaAdpcm( pData, nData, nChannels, nBlockAlign, nSamplesPerBlock, &pcm->data );
            break;
        default:
            *ppszError = "unsupported WAVE format tag";
            return PPcm();
    }
    return pcm;
}

/* ---- Ogg Vorbis: vorbisfile over a memory buffer ------------------------ */
namespace {
size_t MemRead( void *pDst, size_t nSize, size_t nCount, void *pUser )
{
    SMemFile *m = (SMemFile *)pUser;
    size_t nWant = nSize * nCount;
    const size_t nLeft = m->n - m->pos;
    if ( nWant > nLeft ) nWant = nLeft;
    if ( nWant ) memcpy( pDst, m->p + m->pos, nWant );
    m->pos += nWant;
    return nSize ? nWant / nSize : 0;
}
int MemSeek( void *pUser, ogg_int64_t nOffset, int nWhence )
{
    SMemFile *m = (SMemFile *)pUser;
    ogg_int64_t nNew;
    switch ( nWhence )
    {
        case SEEK_SET: nNew = nOffset; break;
        case SEEK_CUR: nNew = (ogg_int64_t)m->pos + nOffset; break;
        case SEEK_END: nNew = (ogg_int64_t)m->n + nOffset; break;
        default: return -1;
    }
    if ( nNew < 0 || nNew > (ogg_int64_t)m->n )
        return -1;
    m->pos = (size_t)nNew;
    return 0;
}
long MemTell( void *pUser ) { return (long)( (SMemFile *)pUser )->pos; }
}  // namespace
const ov_callbacks MEM_CALLBACKS = { MemRead, MemSeek, 0, MemTell };

/*  Read up to nFrames interleaved 16-bit frames; 0 at the end. */
int OvReadFrames( OggVorbis_File *pVF, int nChannels, int16_t *pOut, int nFrames )
{
    int nDone = 0;
    while ( nDone < nFrames )
    {
        int nBitstream = 0;
        const long nBytes = ov_read( pVF, (char *)( pOut + (size_t)nDone * nChannels ), ( nFrames - nDone ) * nChannels * 2, 0, 2, 1, &nBitstream );
        if ( nBytes == OV_HOLE )        /* a damaged page: skip it, keep going */
            continue;
        if ( nBytes <= 0 )
            break;
        nDone += (int)( nBytes / ( nChannels * 2 ) );
    }
    return nDone;
}

PPcm DecodeOgg( const uint8_t *p, size_t n, const char **ppszError )
{
    *ppszError = 0;
    SMemFile mem = { p, n, 0 };
    OggVorbis_File vf;
    if ( ov_open_callbacks( &mem, &vf, 0, 0, MEM_CALLBACKS ) < 0 )
    {
        *ppszError = "not a Vorbis stream";
        return PPcm();
    }
    const vorbis_info *pInfo = ov_info( &vf, -1 );
    if ( !pInfo || pInfo->channels < 1 || pInfo->channels > 2 )
    {
        ov_clear( &vf );
        *ppszError = "Vorbis with an unsupported channel count";
        return PPcm();
    }
    PPcm pcm( new SPcm );
    pcm->nChannels = pInfo->channels;
    pcm->nRate = (int)pInfo->rate;
    const ogg_int64_t nTotal = ov_pcm_total( &vf, -1 );
    size_t nFrames = 0;
    pcm->data.resize( ( nTotal > 0 ? (size_t)nTotal : 4096 ) * pcm->nChannels );
    for ( ;; )
    {
        if ( ( nFrames + 4096 ) * pcm->nChannels > pcm->data.size() )
            pcm->data.resize( ( nFrames + 4096 ) * pcm->nChannels * 2 );
        const int nGot = OvReadFrames( &vf, pcm->nChannels, &pcm->data[ nFrames * pcm->nChannels ], 4096 );
        if ( nGot <= 0 )
            break;
        nFrames += nGot;
    }
    ov_clear( &vf );
    pcm->data.resize( nFrames * pcm->nChannels );
    if ( !nFrames )
    {
        *ppszError = "Vorbis stream decoded to nothing";
        return PPcm();
    }
    return pcm;
}

PPcm DecodeAny( const void *pData, int nLength, const char **ppszError )
{
    const uint8_t *p = (const uint8_t *)pData;
    if ( !p || nLength < 12 )
    {
        *ppszError = "empty";
        return PPcm();
    }
    if ( memcmp( p, "OggS", 4 ) == 0 )
        return DecodeOgg( p, nLength, ppszError );
    if ( memcmp( p, "RIFF", 4 ) == 0 )
        return DecodeWav( p, nLength, ppszError );
    *ppszError = "unknown container";
    return PPcm();
}

/* ---- CStreamSource ------------------------------------------------------ */
CStreamSource::CStreamSource() : bVorbis( false ), nChannels( 0 ), nRate( 0 ), nPcmPos( 0 )
{
    memset( &mem, 0, sizeof( mem ) );
    memset( &vf, 0, sizeof( vf ) );
}
CStreamSource::~CStreamSource()
{
    if ( bVorbis )
        ov_clear( &vf );
}

bool CStreamSource::Open( const char *pszNativePath, const char **ppszError )
{
    *ppszError = 0;
    FILE *f = fopen( pszNativePath, "rb" );
    if ( !f )
    {
        *ppszError = "cannot open";
        return false;
    }
    fseek( f, 0, SEEK_END );
    const long nSize = ftell( f );
    fseek( f, 0, SEEK_SET );
    if ( nSize <= 12 )
    {
        fclose( f );
        *ppszError = "empty file";
        return false;
    }
    file.resize( (size_t)nSize );
    const size_t nRead = fread( &file[ 0 ], 1, (size_t)nSize, f );
    fclose( f );
    if ( nRead != (size_t)nSize )
    {
        *ppszError = "short read";
        return false;
    }
    if ( memcmp( &file[ 0 ], "OggS", 4 ) == 0 )
    {
        mem.p = &file[ 0 ];
        mem.n = file.size();
        mem.pos = 0;
        if ( ov_open_callbacks( &mem, &vf, 0, 0, MEM_CALLBACKS ) < 0 )
        {
            *ppszError = "Vorbis open failed";
            return false;
        }
        bVorbis = true;
        const vorbis_info *pInfo = ov_info( &vf, -1 );
        nChannels = pInfo ? pInfo->channels : 0;
        nRate = pInfo ? (int)pInfo->rate : 0;
    }
    else
    {
        pcm = DecodeWav( &file[ 0 ], file.size(), ppszError );
        std::vector< uint8_t >().swap( file );
        if ( !pcm )
            return false;
        nChannels = pcm->nChannels;
        nRate = pcm->nRate;
    }
    if ( nChannels < 1 || nChannels > 2 || nRate <= 0 )
    {
        *ppszError = "unsupported channel count";
        return false;
    }
    return true;
}

int CStreamSource::Read( int16_t *pOut, int nFrames )
{
    if ( bVorbis )
        return OvReadFrames( &vf, nChannels, pOut, nFrames );
    if ( pcm )
    {
        const size_t nLeft = pcm->Frames() - nPcmPos;
        const size_t nTake = (size_t)nFrames < nLeft ? (size_t)nFrames : nLeft;
        if ( nTake )
            memcpy( pOut, &pcm->data[ nPcmPos * nChannels ], nTake * nChannels * sizeof( int16_t ) );
        nPcmPos += nTake;
        return (int)nTake;
    }
    return 0;
}

void CStreamSource::Rewind()
{
    if ( bVorbis )
        ov_pcm_seek( &vf, 0 );
    nPcmPos = 0;
}

}  // namespace NAudioDecode
