/* Host-side ufont smoke test: parse the bundled Arimo subset, measure and
 * render a specimen to /tmp/opencode/ufont-test.ppm. */
#include "ufont.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *read_file(const char *path, unsigned int *len)
{
	FILE *f = fopen(path, "rb");
	long n;
	unsigned char *p;

	if (!f)
		return 0;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0 || n > (long)UFONT_MAX_FILE) {
		fclose(f);
		return 0;
	}
	p = (unsigned char *)malloc((size_t)n);
	if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) {
		free(p);
		fclose(f);
		return 0;
	}
	fclose(f);
	*len = (unsigned int)n;
	return p;
}

static int draw_text(unsigned short *fb, int w, int h, int x, int baseline,
		     const struct ufont_face *face, int px, const char *s)
{
	int pen = 0;
	unsigned short color = 0xFFFF;

	while (*s) {
		const struct ufont_glyph *g =
			ufont_glyph(face, px, (unsigned char)*s);

		if (g) {
			ufont_blit_rgb565(fb, w, h, x + (pen >> 6), baseline,
					  g, color, 0, 0, w, h);
			pen += g->advance;
		}
		s++;
	}
	return (pen + 32) >> 6;
}

int main(void)
{
	unsigned char *data;
	unsigned int len = 0;
	struct ufont_face *face;
	struct ufont_metrics m;
	int sizes[] = { 12, 16, 24, 32 };
	unsigned int si;
	unsigned short *fb;
	int W = 720, H = 440;
	FILE *out;
	int i;

	data = read_file("fonts/sans.ttf", &len);
	if (!data) {
		fprintf(stderr, "cannot read fonts/sans.ttf\n");
		return 1;
	}
	/* A truncated sfnt must be rejected without touching the arena. */
	if (ufont_face_open_mem(data, 64) != 0) {
		fprintf(stderr, "truncated font was accepted\n");
		return 1;
	}
	face = ufont_face_open_mem(data, len);
	if (!face) {
		fprintf(stderr, "cannot parse font\n");
		return 1;
	}
	ufont_metrics(face, 16, &m);
	printf("16px: ascent=%d descent=%d gap=%d line=%d\n",
	       m.ascent, m.descent, m.line_gap, m.line_height);
	printf("width 'Hello, world!' 16=%d 24=%d 32=%d\n",
	       ufont_text_width(face, 16, "Hello, world!", -1, 0),
	       ufont_text_width(face, 24, "Hello, world!", -1, 0),
	       ufont_text_width(face, 32, "Hello, world!", -1, 0));
	printf("width composite 'Äé' 16=%d\n",
	       ufont_text_width(face, 16, "\xC3\x84\xC3\xA9", -1, 1));

	fb = (unsigned short *)calloc((size_t)W * H, sizeof(unsigned short));
	if (!fb)
		return 1;
	for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++) {
		int px = sizes[si];
		int y = 20 + (int)si * 100;

		ufont_metrics(face, px, &m);
		draw_text(fb, W, H, 8, y, face, px,
			  "The quick brown fox jumps over 1234567890");
		draw_text(fb, W, H, 8, y + 34, face, px,
			  "\xC4rger \xE9 \xFC \xDF \xA9 \xAE \xB0 \xB1");
	}
	out = fopen("/tmp/opencode/ufont-test.ppm", "wb");
	if (!out)
		return 1;
	fprintf(out, "P6\n%d %d\n255\n", W, H);
	for (i = 0; i < W * H; i++) {
		unsigned short c = fb[i];
		unsigned char rgb[3];

		rgb[0] = (unsigned char)(((c >> 11) & 0x1F) * 255 / 31);
		rgb[1] = (unsigned char)(((c >> 5) & 0x3F) * 255 / 63);
		rgb[2] = (unsigned char)((c & 0x1F) * 255 / 31);
		fwrite(rgb, 1, 3, out);
	}
	fclose(out);
	printf("wrote /tmp/opencode/ufont-test.ppm\n");
	ufont_reset();
	free(fb);
	free(data);
	return 0;
}
