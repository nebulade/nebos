/* ufont: face registry, glyph cache and text metrics on top of the
 * fixed-point TrueType backend in ufont_ttf.c. */
#include "ufont.h"
#include "ufont_internal.h"
#include <stdlib.h>
#include <string.h>

struct ufont_cache_ent {
	const struct ufont_face *face;
	unsigned int cp;
	unsigned char px;
	unsigned char used;
	struct ufont_glyph g;
};

static unsigned char *arena;
static unsigned int arena_cap;
static unsigned int arena_used;
static unsigned int glyph_base;
static struct ufont_face *faces[UFONT_MAX_FACES];
static int nfaces;
static struct ufont_cache_ent cache[UFONT_CACHE_SLOTS];

void *ufont_arena_alloc(unsigned int bytes, unsigned int align)
{
	unsigned int p;

	if (!arena || align == 0)
		return 0;
	p = (arena_used + align - 1u) & ~(align - 1u);
	if (p > arena_cap || bytes > arena_cap - p)
		return 0;
	arena_used = p + bytes;
	return arena + p;
}

void ufont_arena_mark(unsigned int mark)
{
	if (mark <= arena_cap)
		arena_used = mark;
}

unsigned int ufont_arena_used(void)
{
	return arena_used;
}

static void cache_clear(void)
{
	unsigned int i;

	for (i = 0; i < UFONT_CACHE_SLOTS; i++)
		cache[i].used = 0;
	if (glyph_base)
		arena_used = glyph_base;
}

int ufont_arena_init(void)
{
	if (arena)
		return 0;
	arena = (unsigned char *)malloc(UFONT_ARENA_BYTES);
	if (!arena)
		return -1;
	arena_cap = UFONT_ARENA_BYTES;
	arena_used = 0;
	glyph_base = 0;
	nfaces = 0;
	memset(faces, 0, sizeof(faces));
	memset(cache, 0, sizeof(cache));
	return 0;
}

void ufont_reset(void)
{
	if (arena)
		free(arena);
	arena = 0;
	arena_cap = 0;
	arena_used = 0;
	glyph_base = 0;
	nfaces = 0;
	memset(faces, 0, sizeof(faces));
	memset(cache, 0, sizeof(cache));
}

struct ufont_face *ufont_face_open_mem(const void *data, unsigned int len)
{
	struct ufont_face *f;
	unsigned char *copy;
	unsigned int mark;

	if (!data || len == 0 || len > UFONT_MAX_FILE)
		return 0;
	if (ufont_arena_init() != 0)
		return 0;
	if (nfaces >= UFONT_MAX_FACES)
		return 0;
	if (glyph_base)
		cache_clear();
	mark = arena_used;
	copy = (unsigned char *)ufont_arena_alloc(len, 4);
	if (!copy) {
		ufont_arena_mark(mark);
		return 0;
	}
	memcpy(copy, data, len);
	f = (struct ufont_face *)ufont_arena_alloc(sizeof(*f), 8);
	if (!f) {
		ufont_arena_mark(mark);
		return 0;
	}
	memset(f, 0, sizeof(*f));
	f->id = (unsigned int)nfaces;
	if (ufont_ttf_load(f, copy, len) != 0) {
		ufont_arena_mark(mark);
		return 0;
	}
	faces[nfaces++] = f;
	glyph_base = arena_used;
	return f;
}

void ufont_metrics(const struct ufont_face *f, int px, struct ufont_metrics *m)
{
	int asc, desc, gap;

	if (!m)
		return;
	memset(m, 0, sizeof(*m));
	if (!f || px < 4)
		return;
	asc = (int)(((long)f->ascent * px + f->upem / 2) / f->upem);
	desc = (int)(((long)-f->descent * px + f->upem / 2) / f->upem);
	gap = (int)(((long)f->line_gap * px + f->upem / 2) / f->upem);
	if (asc < 1)
		asc = px - 2;
	if (desc < 1)
		desc = 2;
	if (gap < 0)
		gap = 0;
	m->ascent = asc;
	m->descent = desc;
	m->line_gap = gap;
	m->line_height = asc + desc + gap;
}

static unsigned int next_cp(const char *s, int *i, int n, int utf8)
{
	unsigned int c;
	int p = *i;

	if (!utf8) {
		c = (unsigned char)s[p];
		*i = p + 1;
		return c;
	}
	c = (unsigned char)s[p++];
	if (c < 0x80) {
		*i = p;
		return c;
	}
	if ((c & 0xE0) == 0xC0 && (n < 0 || p < n)) {
		unsigned int c2 = (unsigned char)s[p];
		if ((c2 & 0xC0) == 0x80) {
			*i = p + 1;
			return ((c & 0x1F) << 6) | (c2 & 0x3F);
		}
	} else if ((c & 0xF0) == 0xE0 && (n < 0 || p + 1 < n)) {
		unsigned int c2 = (unsigned char)s[p];
		unsigned int c3 = (unsigned char)s[p + 1];
		if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80) {
			*i = p + 2;
			return ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) |
			       (c3 & 0x3F);
		}
	} else if ((c & 0xF8) == 0xF0 && (n < 0 || p + 2 < n)) {
		unsigned int c2 = (unsigned char)s[p];
		unsigned int c3 = (unsigned char)s[p + 1];
		unsigned int c4 = (unsigned char)s[p + 2];
		if ((c2 & 0xC0) == 0x80 && (c3 & 0xC0) == 0x80 &&
		    (c4 & 0xC0) == 0x80) {
			*i = p + 3;
			return ((c & 0x07) << 18) | ((c2 & 0x3F) << 12) |
			       ((c3 & 0x3F) << 6) | (c4 & 0x3F);
		}
	}
	*i = p;
	return 0xFFFD;
}

int ufont_text_width(const struct ufont_face *f, int px,
		     const char *s, int n, int utf8)
{
	int i = 0, total = 0;

	if (!f || !s || px < 4)
		return 0;
	if (n < 0) {
		n = 0;
		while (s[n])
			n++;
	}
	while (i < n) {
		unsigned int cp = next_cp(s, &i, n, utf8);
		int glyph = ufont_ttf_glyph_index(f, cp);

		total += ufont_ttf_advance(f, glyph, px);
	}
	return (total + 32) >> 6;
}

static struct ufont_cache_ent *cache_lookup(const struct ufont_face *f,
					    int px, unsigned int cp)
{
	unsigned int start, i;

	start = (cp * 2654435761u) ^ ((unsigned int)px << 16) ^
		(f->id * 0x9E3779B1u);
	start &= UFONT_CACHE_SLOTS - 1u;
	for (i = 0; i < UFONT_CACHE_SLOTS; i++) {
		struct ufont_cache_ent *e =
			&cache[(start + i) & (UFONT_CACHE_SLOTS - 1u)];

		if (!e->used)
			return 0;
		if (e->face == f && e->px == (unsigned char)px && e->cp == cp)
			return e;
	}
	return 0;
}

static struct ufont_cache_ent *cache_free_slot(const struct ufont_face *f,
					       int px, unsigned int cp)
{
	unsigned int start, i;

	start = (cp * 2654435761u) ^ ((unsigned int)px << 16) ^
		(f->id * 0x9E3779B1u);
	start &= UFONT_CACHE_SLOTS - 1u;
	for (i = 0; i < UFONT_CACHE_SLOTS; i++) {
		struct ufont_cache_ent *e =
			&cache[(start + i) & (UFONT_CACHE_SLOTS - 1u)];

		if (!e->used) {
			e->used = 1;
			e->face = f;
			e->px = (unsigned char)px;
			e->cp = cp;
			memset(&e->g, 0, sizeof(e->g));
			return e;
		}
	}
	return 0;
}

const struct ufont_glyph *ufont_glyph(const struct ufont_face *f, int px,
				      unsigned int cp)
{
	struct ufont_cache_ent *e;
	int glyph, advance, xoff, yoff, w, h;
	unsigned char *mask;
	int attempt;

	if (!f || px < 4)
		return 0;
	if (px > 96)
		px = 96;
	e = cache_lookup(f, px, cp);
	if (e)
		return &e->g;

	for (attempt = 0; attempt < 2; attempt++) {
		e = cache_free_slot(f, px, cp);
		if (!e) {
			cache_clear();
			continue;
		}
		glyph = ufont_ttf_glyph_index(f, cp);
		advance = ufont_ttf_advance(f, glyph, px);
		xoff = yoff = w = h = 0;
		ufont_ttf_bbox(f, glyph, px, &xoff, &yoff, &w, &h);
		e->g.advance = advance;
		e->g.xoff = (short)xoff;
		e->g.yoff = (short)yoff;
		e->g.w = (unsigned short)w;
		e->g.h = (unsigned short)h;
		if (w <= 0 || h <= 0)
			return &e->g;
		mask = (unsigned char *)ufont_arena_alloc(
			(unsigned int)w * (unsigned int)h, 1);
		if (!mask) {
			e->used = 0;
			cache_clear();
			continue;
		}
		memset(mask, 0, (unsigned int)w * (unsigned int)h);
		ufont_ttf_raster(f, glyph, px, xoff, yoff, w, h, mask);
		e->g.mask = mask;
		return &e->g;
	}
	return 0;
}
