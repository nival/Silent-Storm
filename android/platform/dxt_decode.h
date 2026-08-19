/*
 *  dxt_decode.h -- software decoding of DXT1/DXT3/DXT5 (BC1/BC2/BC3) blocks.
 *
 *  Every texture the game ships is a DXT-compressed MMP (see Image/mmpFormat.h
 *  and NGfx::EPixelFormat).  Desktop GPUs consume those directly, and so do most
 *  Android GPUs through GL_EXT_texture_compression_s3tc -- but not all, and the
 *  boot harness wants to look at real pixels.  This is the fallback path: it
 *  turns a compressed mip level into RGBA8.
 *
 *  Reference: the S3TC block layouts as documented for D3DFMT_DXT1/3/5.
 */
#ifndef A5_DXT_DECODE_H
#define A5_DXT_DECODE_H

#include <stdint.h>
#include <stddef.h>

/*  Decodes nWidth x nHeight texels of DXT data into pOut (RGBA8, row-major,
 *  nWidth*4 bytes per row).  nDxtVersion is 1, 3 or 5.  Widths and heights
 *  that are not multiples of 4 are handled: the trailing partial block is
 *  decoded and clipped.  Returns false if the input is too short. */
bool DxtDecode( int nDxtVersion, const uint8_t *pIn, size_t nInSize,
                int nWidth, int nHeight, uint8_t *pOut );

/*  Bytes a nWidth x nHeight level of the given DXT version occupies. */
size_t DxtLevelSize( int nDxtVersion, int nWidth, int nHeight );

#endif
