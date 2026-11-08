/* Userland text engine: fixed-point TrueType faces, Latin-1/UTF-8 text
 * measurement and antialiased glyph bitmaps.
 *
 * Faces and glyph masks live in one heap arena owned by this module.  The
 * arena is deliberately reset by ngui_font_detach()/ufont_reset() because the
 * browser calls heap_reset() on navigation; no pointer from this module may
 * survive that call. */
#ifndef UFONT_H
#define UFONT_H

#include <stddef.h>

#define UFONT_ARENA_BYTES   (256u * 1024u)
#define UFONT_MAX_FILE      (256u * 1024u)
#define UFONT_CACHE_SLOTS   512

struct ufont_face;

struct ufont_glyph {
	const unsigned char *mask;  /* alpha, w*h bytes, row-major */
	unsigned short w, h;
	short xoff, yoff;           /* bitmap origin relative to pen/baseline */
	int advance;                /* 26.6 pixels */
};

struct ufont_metrics {
	int ascent;                 /* pixels above the baseline */
	int descent;                /* pixels below the baseline */
	int line_gap;
	int line_height;
};

/* Allocate the shared arena (256 KB).  Returns 0 on success. */
int ufont_arena_init(void);
void ufont_reset(void);

/* Open a face from memory or from a VFS path.  The path form lives in
 * ufont_file.c and is not linked into the host tests. */
struct ufont_face *ufont_face_open_mem(const void *data, unsigned int len);
struct ufont_face *ufont_face_open_path(const char *path);

void ufont_metrics(const struct ufont_face *face, int px,
		   struct ufont_metrics *out);

/* Width in whole pixels.  n < 0 means NUL-terminated; utf8 selects the
 * decoder (0 = Latin-1 / one byte per codepoint). */
int ufont_text_width(const struct ufont_face *face, int px,
		     const char *s, int n, int utf8);

/* Cached alpha bitmap for one codepoint. */
const struct ufont_glyph *ufont_glyph(const struct ufont_face *face, int px,
				      unsigned int cp);

/* Alpha blend one glyph.  The mask is positioned at
 * (x + g->xoff, baseline + g->yoff).  Returns the number of touched pixels. */
int ufont_blit_rgb565(unsigned short *px, int w, int h,
		      int x, int baseline, const struct ufont_glyph *g,
		      unsigned short color,
		      int cx, int cy, int cw, int ch);

int ufont_blit_rgb(void *px, int w, int h, int pitch, int bpp,
		   int x, int baseline, const struct ufont_glyph *g,
		   unsigned int rgb,
		   int cx, int cy, int cw, int ch);

#endif
