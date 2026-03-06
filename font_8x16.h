/* 8x16 bitmap font: 256 chars, 16 bytes each (one byte per row, MSB left).
   Minimal VGA-style glyphs for ASCII 32-126. */
#ifndef FONT_8X16_H
#define FONT_8X16_H

#define FONT_8X16_CHARS  256
#define FONT_8X16_ROWS   16

extern const unsigned char font_8x16[FONT_8X16_CHARS * FONT_8X16_ROWS];

#endif
