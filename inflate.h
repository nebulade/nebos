/* inflate: freestanding DEFLATE / zlib / gzip decoder.
 *
 * No libc, no allocation: the caller provides the workspace (struct inflate,
 * about 33 KB) and an output sink.  The compressed input is a sequence of
 * memory segments, so a PNG decoder can feed consecutive IDAT chunks without
 * copying them.
 *
 * Used by the image library (PNG) and by the kernel's compressed command pack
 * (gzip).  See img_png.c and ramfs.c. */
#ifndef INFLATE_H
#define INFLATE_H

#define INFLATE_MAXBITS 15

struct inflate_src {
	const unsigned char *p;
	unsigned int len;
	/* Called when p/len are exhausted: return 1 and set a new segment,
	 * or 0 at the end of the input. */
	int (*more)(void *ctx, const unsigned char **p, unsigned int *len);
	void *ctx;
};

struct inflate_sink {
	int (*write)(void *ctx, const unsigned char *data, unsigned int len);
	void *ctx;
};

struct inflate_huff {
	unsigned short count[INFLATE_MAXBITS + 1];
	unsigned short symbol[288];
};

struct inflate {
	struct inflate_src src;
	struct inflate_sink sink;
	void (*progress)(void *ctx);    /* optional UI/service pump */
	void *progress_ctx;

	unsigned int bitbuf;
	int bitcnt;
	unsigned long long total_bits;

	unsigned char win[32768];   /* LZ77 history / pending output */
	unsigned int wpos;
	unsigned int pending;

	unsigned long long total;   /* bytes produced */
	unsigned int crc;           /* running CRC32 (pre-inverted) */
	unsigned int adler_a, adler_b;
};

void inflate_init(struct inflate *z, struct inflate_src *src,
		  struct inflate_sink *sink);

/* Raw DEFLATE stream from the current source position. */
int inflate_raw(struct inflate *z, unsigned int *out_len);

/* zlib stream (2-byte header, DEFLATE, Adler-32). */
int inflate_zlib(struct inflate *z, unsigned int *out_len);

/* gzip stream (10+ byte header, DEFLATE, CRC32 + ISIZE). */
int inflate_gzip(struct inflate *z, unsigned int *out_len);

/* Entire in-memory helpers.  `ws` must point at sizeof(struct inflate) bytes
 * with at least 16-byte alignment.  Return 0 on success. */
int inflate_zlib_mem(const void *src, unsigned int len, void *dst,
		     unsigned int cap, unsigned int *out_len, void *ws);
int inflate_gzip_mem(const void *src, unsigned int len, void *dst,
		     unsigned int cap, unsigned int *out_len, void *ws);
int inflate_raw_mem(const void *src, unsigned int len, void *dst,
		    unsigned int cap, unsigned int *out_len, void *ws);

unsigned int crc32_bytes(const void *data, unsigned int len);
unsigned int adler32_bytes(const void *data, unsigned int len);

#endif
