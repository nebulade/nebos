/* Image library core: format sniffing, decode dispatch, arena image
 * allocation and error/format names.  See img.h. */

#include "img_internal.h"

int img_size_ok(unsigned int w, unsigned int h,
		const struct img_options *opt)
{
	unsigned long long n;

	if (w == 0 || h == 0 || w > 32768 || h > 32768)
		return 0;
	n = (unsigned long long)w * h;
	if (n > 0x3FFFFFFFull)
		return 0;
	if (opt && opt->max_pixels && n > opt->max_pixels)
		return 0;
	return 1;
}

int img_alloc(struct img_mem *m, unsigned int w, unsigned int h,
	      unsigned int format, int alpha, struct img_image *out)
{
	unsigned long long n = (unsigned long long)w * h;
	unsigned short *px;
	unsigned char *a = 0;

	if (!m || !out || n == 0 || n > 0x3FFFFFFFull)
		return IMG_ERR_TOOBIG;
	px = (unsigned short *)img_mem_alloc(m, (unsigned int)(n * 2), 16);
	if (!px)
		return IMG_ERR_NOMEM;
	if (alpha) {
		a = (unsigned char *)img_mem_alloc(m, (unsigned int)n, 16);
		if (!a)
			return IMG_ERR_NOMEM;
	}
	out->format = format;
	out->w = w;
	out->h = h;
	out->px = px;
	out->alpha = a;
	out->flags = alpha ? IMG_FLAG_ALPHA : 0;
	return 0;
}

static int png_sig(const unsigned char *d, unsigned int n)
{
	static const unsigned char sig[8] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A
	};
	unsigned int i;

	if (n < 8)
		return 0;
	for (i = 0; i < 8; i++)
		if (d[i] != sig[i])
			return 0;
	return 1;
}

int img_sniff(const void *data, unsigned int len, struct img_info *out)
{
	const unsigned char *d = (const unsigned char *)data;
	int fmt;

	if (!out)
		return IMG_ERR_FORMAT;
	out->format = IMG_FMT_UNKNOWN;
	out->width = out->height = 0;
	out->has_alpha = 0;
	out->progressive = 0;
	out->components = 1;
	out->max_h = out->max_v = 1;
	if (!d || len == 0)
		return IMG_ERR_EMPTY;

	if (png_sig(d, len))
		fmt = img_sniff_png(d, len, out);
	else if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
		fmt = img_sniff_jpeg(d, len, out);
	else if (len >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' &&
		 d[3] == '8' && (d[4] == '7' || d[4] == '9') && d[5] == 'a')
		fmt = img_sniff_gif(d, len, out);
	else if (len >= 2 && d[0] == 'B' && d[1] == 'M')
		fmt = img_sniff_bmp(d, len, out);
	else if (len >= 2 && d[0] == 'P' && d[1] == '7')
		fmt = img_sniff_pam(d, len, out);
	else if (len >= 2 && d[0] == 'P' && d[1] >= '1' && d[1] <= '6')
		fmt = img_sniff_pnm(d, len, out);
	else if (len >= 4 && d[0] == 0 && d[1] == 0 && d[3] == 0 &&
		 (d[2] == 1 || d[2] == 2)) {
		/* TGA type 2 also starts with 00 00 02 00.  Only accept the
		 * ICO interpretation when its directory is valid. */
		fmt = img_sniff_ico(d, len, out);
		if (fmt == IMG_FMT_UNKNOWN)
			fmt = img_sniff_tga(d, len, out);
	} else
		fmt = img_sniff_tga(d, len, out);   /* no magic: strict parse */

	if (fmt == IMG_FMT_UNKNOWN)
		return IMG_ERR_FORMAT;
	return 0;
}

int img_decode(const void *data, unsigned int len,
	       const struct img_options *opt, struct img_mem *m,
	       struct img_image *out)
{
	const unsigned char *d = (const unsigned char *)data;
	struct img_info info;
	int r;

	if (!d || len == 0)
		return IMG_ERR_EMPTY;
	if (!m || !out)
		return IMG_ERR_NOMEM;

	r = img_sniff(d, len, &info);
	if (r)
		return r;
	if (!img_size_ok(info.width, info.height, opt))
		return IMG_ERR_TOOBIG;

	switch (info.format) {
	case IMG_FMT_PNG:
		r = img_decode_png(d, len, opt, m, out);
		break;
	case IMG_FMT_JPEG:
		r = img_decode_jpeg(d, len, opt, m, out);
		break;
	case IMG_FMT_GIF:
		r = img_decode_gif(d, len, opt, m, out);
		break;
	case IMG_FMT_BMP:
		r = img_decode_bmp(d, len, opt, m, out);
		break;
	case IMG_FMT_TGA:
		r = img_decode_tga(d, len, opt, m, out);
		break;
	case IMG_FMT_PNM:
		r = img_decode_pnm(d, len, opt, m, out);
		break;
	case IMG_FMT_ICO:
		r = img_decode_ico(d, len, opt, m, out);
		break;
	case IMG_FMT_PAM:
		r = img_decode_pam(d, len, opt, m, out);
		break;
	default:
		r = IMG_ERR_UNSUPPORTED;
		break;
	}
	if (r == 0 && (!out->px || out->w != info.width ||
		       out->h != info.height))
		return IMG_ERR_CORRUPT;
	return r;
}

unsigned int img_estimate(const struct img_info *info)
{
	unsigned long long px, bytes;

	if (!info || !info->width || !info->height)
		return 2u * 1024u * 1024u;
	px = (unsigned long long)info->width * info->height;
	bytes = px * 2;                       /* RGB565 output */
	if (info->has_alpha)
		bytes += px;                  /* alpha plane */
	if (info->format == IMG_FMT_JPEG) {
		/* Component pixel count relative to luma.  Keep this in 32-bit
		 * for the target (no 64-bit division in ring 3). */
		unsigned int samp = info->max_h * info->max_v;
		unsigned int comp_px;
		unsigned int p;

		if (px > 0xFFFFFFFFull)
			return 0xFFFFFFFFu;
		p = (unsigned int)px;

		if (samp == 0)
			samp = 1;
		if (info->components == 3) {
			/* p + 2*p/samp, using only 32-bit division. */
			comp_px = p + (p / samp) * 2 + 2;
			if (comp_px < p)
				return 0xFFFFFFFFu;
		} else {
			comp_px = p;
		}
		if (info->progressive)
			bytes += (unsigned long long)comp_px * 3; /* coef+plane */
		else
			bytes += comp_px;        /* planes */
	}

	bytes += 2u * 1024u * 1024u;          /* inflate/row/Huffman scratch */
	if (bytes > 0xFFFFFFFFull)
		return 0xFFFFFFFFu;
	return (unsigned int)bytes;
}

const char *img_format_name(unsigned int format)
{
	switch (format) {
	case IMG_FMT_PNG:  return "PNG";
	case IMG_FMT_JPEG: return "JPEG";
	case IMG_FMT_GIF:  return "GIF";
	case IMG_FMT_BMP:  return "BMP";
	case IMG_FMT_TGA:  return "TGA";
	case IMG_FMT_PNM:  return "PNM";
	case IMG_FMT_ICO:  return "ICO";
	case IMG_FMT_PAM:  return "PAM";
	default:           return "image";
	}
}

const char *img_error_string(int err)
{
	switch (err) {
	case IMG_ERR_IO:          return "Cannot read the file";
	case IMG_ERR_FORMAT:      return "Unknown image format";
	case IMG_ERR_CORRUPT:     return "Image data is corrupt or truncated";
	case IMG_ERR_NOMEM:       return "Not enough memory for the image";
	case IMG_ERR_TOOBIG:      return "Image is too large";
	case IMG_ERR_UNSUPPORTED: return "Image variant is not supported yet";
	case IMG_ERR_EMPTY:       return "The file is empty";
	default:                  return "Cannot decode the image";
	}
}
