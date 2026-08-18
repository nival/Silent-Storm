#include "dxt_decode.h"

#include <string.h>

namespace {

inline void Unpack565( uint16_t c, uint8_t *pRgb )
{
    /* Expand 5/6/5 to 8 bits by replicating the top bits into the low ones,
     * which is what hardware does and keeps pure white at 255. */
    const int r = ( c >> 11 ) & 0x1F, g = ( c >> 5 ) & 0x3F, b = c & 0x1F;
    pRgb[ 0 ] = (uint8_t)( ( r << 3 ) | ( r >> 2 ) );
    pRgb[ 1 ] = (uint8_t)( ( g << 2 ) | ( g >> 4 ) );
    pRgb[ 2 ] = (uint8_t)( ( b << 3 ) | ( b >> 2 ) );
}

/*  The 8-byte colour block shared by all three formats.  bDxt1 selects the
 *  DXT1 rule that c0 <= c1 means "3 colours + transparent black". */
void DecodeColourBlock( const uint8_t *pBlock, bool bDxt1, uint8_t rgba[ 16 ][ 4 ] )
{
    const uint16_t c0 = (uint16_t)( pBlock[ 0 ] | ( pBlock[ 1 ] << 8 ) );
    const uint16_t c1 = (uint16_t)( pBlock[ 2 ] | ( pBlock[ 3 ] << 8 ) );

    uint8_t palette[ 4 ][ 4 ];
    Unpack565( c0, palette[ 0 ] );
    Unpack565( c1, palette[ 1 ] );
    palette[ 0 ][ 3 ] = palette[ 1 ][ 3 ] = 255;

    if ( !bDxt1 || c0 > c1 )
    {
        for ( int i = 0; i < 3; ++i )
        {
            palette[ 2 ][ i ] = (uint8_t)( ( 2 * palette[ 0 ][ i ] + palette[ 1 ][ i ] + 1 ) / 3 );
            palette[ 3 ][ i ] = (uint8_t)( ( palette[ 0 ][ i ] + 2 * palette[ 1 ][ i ] + 1 ) / 3 );
        }
        palette[ 2 ][ 3 ] = palette[ 3 ][ 3 ] = 255;
    }
    else
    {
        for ( int i = 0; i < 3; ++i )
            palette[ 2 ][ i ] = (uint8_t)( ( palette[ 0 ][ i ] + palette[ 1 ][ i ] ) / 2 );
        palette[ 2 ][ 3 ] = 255;
        palette[ 3 ][ 0 ] = palette[ 3 ][ 1 ] = palette[ 3 ][ 2 ] = palette[ 3 ][ 3 ] = 0;
    }

    for ( int y = 0; y < 4; ++y )
    {
        const uint8_t row = pBlock[ 4 + y ];
        for ( int x = 0; x < 4; ++x )
        {
            const int nIndex = ( row >> ( x * 2 ) ) & 3;
            memcpy( rgba[ y * 4 + x ], palette[ nIndex ], 4 );
        }
    }
}

/*  DXT3: 16 explicit 4-bit alphas. */
void DecodeExplicitAlpha( const uint8_t *pBlock, uint8_t rgba[ 16 ][ 4 ] )
{
    for ( int i = 0; i < 16; ++i )
    {
        const int nibble = ( pBlock[ i / 2 ] >> ( ( i & 1 ) * 4 ) ) & 0xF;
        rgba[ i ][ 3 ] = (uint8_t)( nibble * 17 );
    }
}

/*  DXT5: two alpha endpoints and 16 3-bit indices into an interpolated ramp. */
void DecodeInterpolatedAlpha( const uint8_t *pBlock, uint8_t rgba[ 16 ][ 4 ] )
{
    const int a0 = pBlock[ 0 ], a1 = pBlock[ 1 ];
    uint8_t   ramp[ 8 ];
    ramp[ 0 ] = (uint8_t)a0;
    ramp[ 1 ] = (uint8_t)a1;
    if ( a0 > a1 )
    {
        for ( int i = 1; i <= 6; ++i )
            ramp[ i + 1 ] = (uint8_t)( ( ( 7 - i ) * a0 + i * a1 + 3 ) / 7 );
    }
    else
    {
        for ( int i = 1; i <= 4; ++i )
            ramp[ i + 1 ] = (uint8_t)( ( ( 5 - i ) * a0 + i * a1 + 2 ) / 5 );
        ramp[ 6 ] = 0;
        ramp[ 7 ] = 255;
    }

    /* 48 bits of indices, little-endian, 3 bits per texel. */
    uint64_t bits = 0;
    for ( int i = 0; i < 6; ++i )
        bits |= (uint64_t)pBlock[ 2 + i ] << ( 8 * i );
    for ( int i = 0; i < 16; ++i )
        rgba[ i ][ 3 ] = ramp[ ( bits >> ( 3 * i ) ) & 7 ];
}

}  // namespace

size_t DxtLevelSize( int nDxtVersion, int nWidth, int nHeight )
{
    const int nBlocksX = ( nWidth + 3 ) / 4, nBlocksY = ( nHeight + 3 ) / 4;
    const size_t nBlockBytes = nDxtVersion == 1 ? 8 : 16;
    return (size_t)nBlocksX * nBlocksY * nBlockBytes;
}

bool DxtDecode( int nDxtVersion, const uint8_t *pIn, size_t nInSize,
                int nWidth, int nHeight, uint8_t *pOut )
{
    if ( nDxtVersion != 1 && nDxtVersion != 3 && nDxtVersion != 5 )
        return false;
    if ( nInSize < DxtLevelSize( nDxtVersion, nWidth, nHeight ) )
        return false;

    const int    nBlocksX    = ( nWidth + 3 ) / 4;
    const int    nBlocksY    = ( nHeight + 3 ) / 4;
    const size_t nBlockBytes = nDxtVersion == 1 ? 8 : 16;

    for ( int by = 0; by < nBlocksY; ++by )
    {
        for ( int bx = 0; bx < nBlocksX; ++bx )
        {
            const uint8_t *pBlock = pIn + ( (size_t)by * nBlocksX + bx ) * nBlockBytes;
            uint8_t rgba[ 16 ][ 4 ];

            if ( nDxtVersion == 1 )
                DecodeColourBlock( pBlock, true, rgba );
            else
            {
                DecodeColourBlock( pBlock + 8, false, rgba );
                if ( nDxtVersion == 3 )
                    DecodeExplicitAlpha( pBlock, rgba );
                else
                    DecodeInterpolatedAlpha( pBlock, rgba );
            }

            for ( int y = 0; y < 4; ++y )
            {
                const int py = by * 4 + y;
                if ( py >= nHeight )
                    break;
                for ( int x = 0; x < 4; ++x )
                {
                    const int px = bx * 4 + x;
                    if ( px >= nWidth )
                        break;
                    memcpy( pOut + ( (size_t)py * nWidth + px ) * 4, rgba[ y * 4 + x ], 4 );
                }
            }
        }
    }
    return true;
}
