/* img: freestanding image decoding + RGB565 compositing.
 *
 * The library is deliberately independent of the OS and of the GUI toolkit:
 * it takes a memory buffer and an arena, and returns RGB565 pixels plus an
 * optional 8-bit alpha plane.  That makes it usable by the image viewer now
 * and by a browser (or any other app) later, and lets it be tested on the
 * host.
 *
 * There is no malloc and no global mutable state.  All scratch and output
 * memory comes from struct img_mem.  The caller should take an img_mem_mark()
 * before a decode and rewind to it when img_decode() fails; a decoder cannot
 * always reclaim scratch allocated before its output image.
 */
#ifndef IMG_H
#define IMG_H

#define IMG_FMT_UNKNOWN 0
#define IMG_FMT_PNG     1
#define IMG_FMT_JPEG    2
#define IMG_FMT_GIF     3
#define IMG_FMT_BMP     4
#define IMG_FMT_TGA     5
#define IMG_FMT_PNM     6
#define IMG_FMT_ICO     7

#define IMG_FLAG_ALPHA  1

/* Errors are negative. */
#define IMG_ERR_IO        (-1)
#define IMG_ERR_FORMAT    (-2)
#define IMG_ERR_CORRUPT   (-3)
#define IMG_ERR_NOMEM     (-4)
#define IMG_ERR_TOOBIG    (-5)
#define IMG_ERR_UNSUPPORTED (-6)
#define IMG_ERR_EMPTY     (-7)

/* Whole-file limit used by the viewer's file buffer. */
#define IMG_FILE_MAX  (4u * 1024u * 1024u)

struct img_info {
	unsigned int format;
	unsigned int width, height;
	unsigned int has_alpha;
};

struct img_options {
	/* 0 = no additional limit beyond the arena / sanity checks. */
	unsigned int max_pixels;
};

struct img_mem {
	unsigned char *base;
	unsigned int size;
	unsigned int used;
};

struct img_image {
	unsigned int format;
	unsigned int w, h;
	unsigned short *px;      /* RGB565, w*h */
	unsigned char *alpha;    /* w*h, or NULL when fully opaque */
	unsigned int flags;
};

struct img_blit {
	unsigned short *dst;
	int dst_w, dst_h;
	int clip_x, clip_y, clip_w, clip_h;
	int x, y;                /* destination top-left of the image */
	unsigned int scale;      /* 16.16, > 0 */
	unsigned int bg;         /* RGB565 background for alpha */
	unsigned int bg2;        /* second checker colour */
	unsigned int flags;
};

#define IMG_BLIT_CHECKER  1  /* alpha over a checkered background */
#define IMG_BLIT_OVER_DST 2  /* alpha over the existing destination pixels */
#define IMG_BLIT_FAST     4  /* nearest sampling while dragging */

void img_mem_init(struct img_mem *m, void *base, unsigned int size);
unsigned int img_mem_mark(const struct img_mem *m);
int  img_mem_rewind(struct img_mem *m, unsigned int mark);
void *img_mem_alloc(struct img_mem *m, unsigned int bytes, unsigned int align);

int  img_sniff(const void *data, unsigned int len, struct img_info *out);
int  img_decode(const void *data, unsigned int len,
		const struct img_options *opt, struct img_mem *m,
		struct img_image *out);
unsigned int img_estimate(const struct img_info *info);

const char *img_format_name(unsigned int format);
const char *img_error_string(int err);

unsigned int img_fit_scale(unsigned int iw, unsigned int ih,
			   unsigned int bw, unsigned int bh);
void img_draw(const struct img_image *im, const struct img_blit *b);

#endif
