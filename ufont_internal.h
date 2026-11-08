/* Internal interfaces between the ufont cache/metrics layer and the
 * fixed-point TrueType parser/rasterizer.  Not part of the app ABI. */
#ifndef UFONT_INTERNAL_H
#define UFONT_INTERNAL_H

#define UFONT_MAX_FACES   8
#define UFONT_MAX_POINTS  256
#define UFONT_MAX_CONTOURS 16

struct ufont_face {
	unsigned int id;
	const unsigned char *data;
	unsigned int len;

	int upem;
	int num_glyphs;
	int loca_short;
	int num_hmetrics;

	int ascent;        /* font units, hhea */
	int descent;       /* font units, negative */
	int line_gap;      /* font units */

	const unsigned char *loca;
	const unsigned char *glyf;
	unsigned int glyf_len;
	const unsigned char *hmtx;
	const unsigned char *cmap4;
	unsigned int cmap4_len;
};

/* Parse the sfnt directory and fill the face; 0 on success. */
int ufont_ttf_load(struct ufont_face *f, const unsigned char *data,
		   unsigned int len);

int ufont_ttf_glyph_index(const struct ufont_face *f, unsigned int cp);

/* Advance in 26.6 pixels. */
int ufont_ttf_advance(const struct ufont_face *f, int glyph, int px);

/* Bitmap box relative to the pen origin (x right, y down from baseline).
 * Returns 0 on success; w/h are 0 for an empty glyph. */
int ufont_ttf_bbox(const struct ufont_face *f, int glyph, int px,
		   int *xoff, int *yoff, int *w, int *h);

/* Rasterize into mask[w*h] (zeroed by the caller); 0 on success. */
int ufont_ttf_raster(const struct ufont_face *f, int glyph, int px,
		     int xoff, int yoff, int w, int h, unsigned char *mask);

/* Arena helpers shared with ufont.c. */
void *ufont_arena_alloc(unsigned int bytes, unsigned int align);
void ufont_arena_mark(unsigned int mark);
unsigned int ufont_arena_used(void);

#endif
