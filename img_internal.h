/* Internal helpers shared by the image decoders. */
#ifndef IMG_INTERNAL_H
#define IMG_INTERNAL_H

#include "img.h"

static inline unsigned int img_be16(const unsigned char *p)
{
	return ((unsigned int)p[0] << 8) | p[1];
}

static inline unsigned int img_be32(const unsigned char *p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
	       ((unsigned int)p[2] << 8) | p[3];
}

static inline unsigned int img_le16(const unsigned char *p)
{
	return ((unsigned int)p[1] << 8) | p[0];
}

static inline unsigned int img_le32(const unsigned char *p)
{
	return ((unsigned int)p[3] << 24) | ((unsigned int)p[2] << 16) |
	       ((unsigned int)p[1] << 8) | p[0];
}

static inline unsigned short img_rgb565(unsigned int r, unsigned int g,
					unsigned int b)
{
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Allocate the output image (and alpha plane when requested) from the arena.
 * Pixels are not cleared. */
int img_alloc(struct img_mem *m, unsigned int w, unsigned int h,
	      unsigned int format, int alpha, struct img_image *out);

/* Decoder entry points (each re-validates its own header). */
int img_decode_png(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out);
int img_decode_jpeg(const unsigned char *d, unsigned int n,
		    const struct img_options *opt, struct img_mem *m,
		    struct img_image *out);
int img_decode_gif(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out);
int img_decode_bmp(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out);
int img_decode_tga(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out);
int img_decode_pnm(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out);
int img_decode_ico(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out);
int img_decode_pam(const unsigned char *d, unsigned int n,
		   const struct img_options *opt, struct img_mem *m,
		   struct img_image *out);

/* Header-only probes used by img_sniff().  Return IMG_FMT_* or UNKNOWN. */
int img_sniff_png(const unsigned char *d, unsigned int n, struct img_info *out);
int img_sniff_jpeg(const unsigned char *d, unsigned int n, struct img_info *out);
int img_sniff_gif(const unsigned char *d, unsigned int n, struct img_info *out);
int img_sniff_bmp(const unsigned char *d, unsigned int n, struct img_info *out);
int img_sniff_tga(const unsigned char *d, unsigned int n, struct img_info *out);
int img_sniff_pnm(const unsigned char *d, unsigned int n, struct img_info *out);
int img_sniff_ico(const unsigned char *d, unsigned int n, struct img_info *out);
int img_sniff_pam(const unsigned char *d, unsigned int n, struct img_info *out);

/* True when an image of w*h pixels is acceptable for the given options. */
int img_size_ok(unsigned int w, unsigned int h,
		const struct img_options *opt);

#endif
