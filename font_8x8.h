/* 8x8 bitmap font: 256 chars, 8 bytes each (one byte per row, MSB left).
   Minimal VGA-style glyphs for ASCII 32-126. */
#ifndef FONT_8X8_H
#define FONT_8X8_H

#define FONT_8X8_CHARS  256
#define FONT_8X8_ROWS   8

extern const unsigned char font_8x8[FONT_8X8_CHARS * FONT_8X8_ROWS];

#endif
