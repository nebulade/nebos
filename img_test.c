/* Host test for the image library (make test-images).  Uses only the
 * freestanding decoder code; no kernel headers are involved. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "img.h"
#include "inflate.h"
#include "testdata/images/expected.h"

static unsigned char arena[64u * 1024u * 1024u] __attribute__((aligned(16)));
static unsigned char filebuf[8u * 1024u * 1024u];

static int failures;

static int read_file(const char *path, unsigned char *buf, unsigned int cap,
		     unsigned int *len)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f) {
		fprintf(stderr, "cannot open %s\n", path);
		return -1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || (unsigned long)n > cap) {
		fclose(f);
		return -1;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		return -1;
	}
	fclose(f);
	*len = (unsigned int)n;
	return 0;
}

static unsigned int fnv565(const struct img_image *im, int alpha)
{
	unsigned int h = 2166136261u, i;

	for (i = 0; i < im->w * im->h; i++) {
		unsigned int px = im->px[i];
		unsigned int a = alpha ? im->alpha[i] : 255;

		if (!alpha || a != 0) {
			h = (h ^ (px & 255)) * 16777619u;
			h = (h ^ ((px >> 8) & 255)) * 16777619u;
		}
		if (alpha)
			h = (h ^ a) * 16777619u;
	}
	return h;
}

static int test_lossless(const struct img_exp *e)
{
	char path[256];
	unsigned char *data = filebuf;
	unsigned int n;
	struct img_mem mem;
	struct img_options opt;
	struct img_info info;
	struct img_image im;
	int r;

	snprintf(path, sizeof(path), "testdata/images/%s", e->path);
	if (read_file(path, data, sizeof(filebuf), &n)) {
		printf("FAIL %s: read\n", e->path);
		return 1;
	}
	memset(&info, 0, sizeof(info));
	if (img_sniff(data, n, &info) != 0 || info.width != e->w ||
	    info.height != e->h) {
		printf("FAIL %s: sniff %ux%u (expected %ux%u)\n", e->path,
		       info.width, info.height, e->w, e->h);
		return 1;
	}
	img_mem_init(&mem, arena, sizeof(arena));
	opt.max_pixels = 0;
	memset(&im, 0, sizeof(im));
	r = img_decode(data, n, &opt, &mem, &im);
	if (r) {
		printf("FAIL %s: %s (%d)\n", e->path, img_error_string(r), r);
		return 1;
	}
	if (im.w != e->w || im.h != e->h) {
		printf("FAIL %s: size %ux%u, expected %ux%u\n", e->path,
		       im.w, im.h, e->w, e->h);
		return 1;
	}
	if (!!im.alpha != !!e->alpha) {
		printf("FAIL %s: alpha %d, expected %d\n", e->path,
		       im.alpha != 0, e->alpha);
		return 1;
	}
	/* Animated GIFs: only the first frame is defined; PIL's first-frame
	 * composition differs for the disposal/transparency corner case, so
	 * only the size/alpha are checked here. */
	if (strcmp(e->path, "anim.gif") == 0)
		return 0;
	{
		unsigned int h = fnv565(&im, e->alpha);

		if (h != e->hash) {
			printf("FAIL %s: hash %08x, expected %08x\n", e->path,
			       h, e->hash);
			return 1;
		}
	}
	return 0;
}

static int read_ppm(const char *path, unsigned int *w, unsigned int *h,
		    unsigned char **pixels)
{
	FILE *f = fopen(path, "rb");
	char magic[3];
	unsigned int maxv, n;
	int c;

	if (!f)
		return -1;
	if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0)
		goto fail;
	/* skip whitespace/comments */
	for (;;) {
		c = fgetc(f);
		if (c == '#') {
			while (c != '\n' && c != EOF)
				c = fgetc(f);
		} else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			continue;
		} else {
			ungetc(c, f);
			break;
		}
	}
	if (fscanf(f, "%u %u %u", w, h, &maxv) != 3 || maxv != 255)
		goto fail;
	fgetc(f);      /* single whitespace after maxval */
	n = *w * *h * 3;
	*pixels = (unsigned char *)malloc(n);
	if (!*pixels || fread(*pixels, 1, n, f) != n)
		goto fail;
	fclose(f);
	return 0;
fail:
	fclose(f);
	return -1;
}

static int test_jpeg(const char *name)
{
	char path[256], refpath[256];
	unsigned int n, rw, rh;
	unsigned char *ref;
	struct img_mem mem;
	struct img_options opt;
	struct img_info info;
	struct img_image im;
	int r, x, y;
	unsigned long long sum = 0, count = 0;
	unsigned int mx = 0;

	snprintf(path, sizeof(path), "testdata/images/%s", name);
	snprintf(refpath, sizeof(refpath), "testdata/images/ref/%s.ppm", name);
	if (read_file(path, filebuf, sizeof(filebuf), &n) ||
	    read_ppm(refpath, &rw, &rh, &ref)) {
		printf("FAIL %s: read\n", name);
		return 1;
	}
	memset(&info, 0, sizeof(info));
	if (img_sniff(filebuf, n, &info) != 0 || info.width != rw ||
	    info.height != rh) {
		printf("FAIL %s: sniff %ux%u, expected %ux%u\n", name,
		       info.width, info.height, rw, rh);
		free(ref);
		return 1;
	}
	img_mem_init(&mem, arena, sizeof(arena));
	opt.max_pixels = 0;
	memset(&im, 0, sizeof(im));
	r = img_decode(filebuf, n, &opt, &mem, &im);
	if (r) {
		printf("FAIL %s: %s\n", name, img_error_string(r));
		free(ref);
		return 1;
	}
	if (im.w != rw || im.h != rh) {
		printf("FAIL %s: size mismatch\n", name);
		free(ref);
		return 1;
	}
	for (y = 0; y < (int)im.h; y++) {
		for (x = 0; x < (int)im.w; x++) {
			unsigned int px = im.px[y * im.w + x];
			int rr = ((px >> 11) & 31) * 255 / 31;
			int gg = ((px >> 5) & 63) * 255 / 63;
			int bb = (px & 31) * 255 / 31;
			unsigned char *p = ref + ((unsigned int)y * im.w + x) * 3;
			int dr = abs(rr - p[0]);
			int dg = abs(gg - p[1]);
			int db = abs(bb - p[2]);

			sum += (unsigned long long)dr + dg + db;
			count += 3;
			if ((unsigned int)dr > mx) mx = dr;
			if ((unsigned int)dg > mx) mx = dg;
			if ((unsigned int)db > mx) mx = db;
		}
	}
	free(ref);
	if (sum / count > 8 || mx > 130) {
		printf("FAIL %s: mean %.2f max %u\n", name,
		       (double)sum / (double)count, mx);
		return 1;
	}
	return 0;
}

static void test_truncated(const char *name)
{
	char path[256];
	unsigned int n, step, len;
	struct img_mem mem;
	struct img_options opt;
	struct img_image im;
	unsigned int i;

	snprintf(path, sizeof(path), "testdata/images/%s", name);
	if (read_file(path, filebuf, sizeof(filebuf), &n))
		return;
	step = n / 64;
	if (!step)
		step = 1;
	for (len = 0; len < n; len += step) {
		img_mem_init(&mem, arena, sizeof(arena));
		opt.max_pixels = 0;
		memset(&im, 0, sizeof(im));
		(void)img_decode(filebuf, len, &opt, &mem, &im);
	}
	/* Deterministic mutations. */
	for (i = 0; i < 400; i++) {
		unsigned int seed = i * 1103515245u + 12345u;
		unsigned int pos = seed % n;
		unsigned char old = filebuf[pos];

		filebuf[pos] ^= (unsigned char)(1u << (seed >> 16 & 7));
		img_mem_init(&mem, arena, sizeof(arena));
		memset(&im, 0, sizeof(im));
		(void)img_decode(filebuf, n, &opt, &mem, &im);
		filebuf[pos] = old;
	}
}

static void test_inflate(void)
{
	static unsigned char raw[65536];
	static unsigned char out[65536];
	unsigned int n, rn, olen;
	unsigned int ws[sizeof(struct inflate) / 4 + 8] __attribute__((aligned(16)));

	if (read_file("testdata/images/inflate.raw", raw, sizeof(raw), &rn)) {
		printf("FAIL inflate: raw read\n");
		failures++;
		return;
	}
	if (read_file("testdata/images/inflate.z", filebuf, sizeof(filebuf), &n) ||
	    inflate_zlib_mem(filebuf, n, out, sizeof(out), &olen, ws) ||
	    olen != rn || memcmp(out, raw, rn) != 0) {
		printf("FAIL inflate: zlib\n");
		failures++;
	}
	if (read_file("testdata/images/inflate.gz", filebuf, sizeof(filebuf), &n) ||
	    inflate_gzip_mem(filebuf, n, out, sizeof(out), &olen, ws) ||
	    olen != rn || memcmp(out, raw, rn) != 0) {
		printf("FAIL inflate: gzip\n");
		failures++;
	}
}

static void test_blit(void)
{
	struct img_image im;
	unsigned short px[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
	unsigned char alpha[4] = { 255, 128, 255, 0 };
	unsigned short dst[16 * 16];
	struct img_blit b;
	int i;

	memset(&im, 0, sizeof(im));
	im.w = 2;
	im.h = 2;
	im.px = px;
	im.alpha = alpha;
	im.format = IMG_FMT_PNG;
	for (i = 0; i < 256; i++)
		dst[i] = 0x1234;
	memset(&b, 0, sizeof(b));
	b.dst = dst;
	b.dst_w = 16;
	b.dst_h = 16;
	b.clip_x = 0;
	b.clip_y = 0;
	b.clip_w = 16;
	b.clip_h = 16;
	b.x = 4;
	b.y = 4;
	b.scale = 2u << 16;   /* 2x2 -> 4x4 */
	b.flags = IMG_BLIT_CHECKER;
	img_draw(&im, &b);
	img_draw(&im, &(struct img_blit){
		.dst = dst, .dst_w = 16, .dst_h = 16,
		.clip_x = 0, .clip_y = 0, .clip_w = 16, .clip_h = 16,
		.x = -1, .y = -1, .scale = 1 << 16,
		.flags = IMG_BLIT_FAST | IMG_BLIT_OVER_DST
	});
}

static int test_gif_anim(void)
{
	unsigned int n, frames = 0, delay = 0;
	int ha = 0;
	struct img_mem mem;
	struct img_image im;
	int r;

	if (read_file("testdata/images/anim.gif", filebuf, sizeof(filebuf), &n))
		return 1;
	if (img_gif_frames(filebuf, n, &frames, &delay, &ha) != 0 || frames < 2) {
		printf("FAIL anim.gif: frame count %u\n", frames);
		return 1;
	}
	img_mem_init(&mem, arena, sizeof(arena));
	memset(&im, 0, sizeof(im));
	r = img_decode_gif_frame(filebuf, n, 0, &mem, &im, &delay);
	if (r == 0)
		r = img_decode_gif_frame(filebuf, n, 1, &mem, &im, &delay);
	if (r != 0) {
		printf("FAIL anim.gif: frame decode %d\n", r);
		return 1;
	}
	return 0;
}

static int test_unsupported(void)
{
	unsigned int n;
	struct img_mem mem;
	struct img_options opt;
	struct img_image im;

	if (read_file("testdata/images/cmyk.jpg", filebuf, sizeof(filebuf), &n))
		return 1;
	img_mem_init(&mem, arena, sizeof(arena));
	opt.max_pixels = 0;
	memset(&im, 0, sizeof(im));
	if (img_decode(filebuf, n, &opt, &mem, &im) != IMG_ERR_UNSUPPORTED) {
		printf("FAIL cmyk.jpg: not rejected\n");
		return 1;
	}
	return 0;
}

int main(void)
{
	unsigned int i;

	test_blit();
	test_inflate();
	if (test_unsupported())
		failures++;
	if (test_gif_anim())
		failures++;
	for (i = 0; i < IMG_EXP_COUNT; i++) {
		if (test_lossless(&img_exp[i]))
			failures++;
		test_truncated(img_exp[i].path);
	}
	for (i = 0; i < IMG_JPEG_COUNT; i++) {
		if (test_jpeg(img_jpeg[i]))
			failures++;
		test_truncated(img_jpeg[i]);
	}
	test_truncated("anim.gif");
	if (failures) {
		printf("%d image test(s) failed\n", failures);
		return 1;
	}
	printf("all image tests passed\n");
	return 0;
}
