/*
 *  text_font.h -- the 5x7 bitmap font used by the boot console.
 *
 *  The engine has its own font format (Complete/Fonts/*, read by Main/FontFormat),
 *  but that lives behind the parts of the renderer that are not ported yet.  The
 *  boot console needs to say something on screen before any of that exists, so it
 *  carries this small font of its own.
 */
#ifndef A5_TEXT_FONT_H
#define A5_TEXT_FONT_H

#include <stdint.h>

/* Cell size in the generated atlas (glyph is 5x7 with one pixel of padding). */
enum { FONT_CELL_W = 6, FONT_CELL_H = 8, FONT_COLUMNS = 16, FONT_ROWS = 6 };
enum { FONT_ATLAS_W = FONT_CELL_W * FONT_COLUMNS, FONT_ATLAS_H = FONT_CELL_H * FONT_ROWS };
enum { FONT_FIRST_CHAR = 32, FONT_LAST_CHAR = 127 };

/*  Rasterises the font into a single-channel (8bpp) atlas of
 *  FONT_ATLAS_W * FONT_ATLAS_H bytes, which the caller owns.  */
void FontBuildAtlas( uint8_t *pOut );

/*  Cell coordinates for a character, for UV computation. */
void FontGetCell( char c, int *pnColumn, int *pnRow );

#endif
