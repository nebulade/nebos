/* ramfs: see ramfs.h.  The pack format is produced by mkpack.c:
 *
 *   char magic[8] = "NBPAK1";  u32 count;  u32 total;
 *   count x { char path[32]; u32 off; u32 size; }   (offsets from pack start)
 *   file data, 4-byte aligned
 */

#include "ramfs.h"
#include "inflate.h"
#include "mm.h"
#include "serial.h"

#define RAMFS_MAX_DECOMPRESSED (8u * 1024u * 1024u)

struct ramfs_hdr {
	char magic[8];
	unsigned int count;
	unsigned int total;
};

struct ramfs_ent {
	char path[RAMFS_PATH_MAX];
	unsigned int off;
	unsigned int size;
};

static const unsigned char *pack_base;
static unsigned int pack_size;
static const struct ramfs_hdr *pack_hdr;
static unsigned int pack_pages;   /* heap pages, when not embedded */

static int str_eq(const char *a, const char *b)
{
	unsigned int i = 0;

	while (a[i] && b[i] && a[i] == b[i])
		i++;
	return a[i] == '\0' && b[i] == '\0';
}

static unsigned int first_len(const char *path)
{
	unsigned int n = 0;

	while (path[n] && path[n] != '/')
		n++;
	return n;
}

/* Same first component ("bin/a" vs "bin/b" -> 1)? */
static int first_component_same(const char *a, const char *b)
{
	unsigned int i, na = first_len(a), nb = first_len(b);

	if (na != nb)
		return 0;
	for (i = 0; i < na; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

/* Skip a leading "dir/" prefix; NULL when the entry is not inside `dir`. */
static const char *strip_dir(const char *path, const char *dir)
{
	unsigned int i = 0;

	if (dir[0] == '\0')
		return path;
	while (dir[i] && path[i] == dir[i])
		i++;
	if (dir[i] != '\0')
		return 0;
	if (path[i] == '/')
		return path + i + 1;
	return 0;
}

/* Basic name check: no empties, no "." / "..". */
static int path_ok(const char *path)
{
	unsigned int i = 0, len = 0;

	if (!path || !path[0])
		return 0;
	for (;;) {
		char c = path[i];
		if (c == '\0' || c == '/') {
			if (len == 0)
				return 0;
			if (len == 1 && path[i - 1] == '.')
				return 0;
			if (len == 2 && path[i - 1] == '.' && path[i - 2] == '.')
				return 0;
			len = 0;
			if (c == '\0')
				return 1;
		} else {
			len++;
		}
		i++;
	}
}

/* Shape + bounds check of a pack; NULL when it is not a valid pack. */
static const struct ramfs_hdr *ramfs_validate(const void *pack, unsigned int size)
{
	const struct ramfs_hdr *h = (const struct ramfs_hdr *)pack;
	const struct ramfs_ent *ents;
	unsigned int i, need;

	if (size < sizeof(*h))
		return 0;
	if (h->magic[0] != 'N' || h->magic[1] != 'B' || h->magic[2] != 'P' ||
	    h->magic[3] != 'A' || h->magic[4] != 'K' || h->magic[5] != '1')
		return 0;
	if (h->count == 0 || h->count > RAMFS_MAX_FILES)
		return 0;
	need = (unsigned int)(sizeof(*h) + h->count * sizeof(*ents));
	if (need > size || h->total > size || h->total < need)
		return 0;

	ents = (const struct ramfs_ent *)((const unsigned char *)pack + sizeof(*h));
	for (i = 0; i < h->count; i++) {
		if (!path_ok(ents[i].path))
			return 0;
		if (ents[i].off > size || ents[i].size > size - ents[i].off)
			return 0;
	}
	return h;
}

int ramfs_init(const void *pack, unsigned int size)
{
	const struct ramfs_hdr *h;
	const unsigned char *p = (const unsigned char *)pack;
	unsigned int pages;

	if (!pack || size < sizeof(struct ramfs_hdr) ||
	    size > RAMFS_MAX_DECOMPRESSED)
		return -1;

	/* The embedded pack is gzip-compressed (the same inflate code that the
	 * PNG decoder uses); a raw pack is still accepted, which is what a
	 * kexec reload passes when it carries the running rootfs over.  Either
	 * way the pack is copied into page-allocated memory, so the boot-time
	 * region can be released. */
	if (size >= 18 && p[0] == 0x1F && p[1] == 0x8B) {
		unsigned int isize = (unsigned int)p[size - 4] |
				     ((unsigned int)p[size - 3] << 8) |
				     ((unsigned int)p[size - 2] << 16) |
				     ((unsigned int)p[size - 1] << 24);
		unsigned int wpages, olen = 0;
		unsigned char *out, *ws;

		if (isize == 0 || isize > RAMFS_MAX_DECOMPRESSED)
			return -1;
		pages = (isize + 0xFFF) >> 12;
		wpages = (sizeof(struct inflate) + 0xFFF) >> 12;
		out = (unsigned char *)mm_alloc_pages(pages);
		if (!out)
			return -1;
		ws = (unsigned char *)mm_alloc_pages(wpages);
		if (!ws) {
			unsigned int i;

			for (i = 0; i < pages; i++)
				mm_free_page((unsigned int)out + i * 0x1000);
			return -1;
		}
		if (inflate_gzip_mem(pack, size, out, isize, &olen, ws) != 0 ||
		    olen != isize) {
			unsigned int i;

			for (i = 0; i < pages; i++)
				mm_free_page((unsigned int)out + i * 0x1000);
			for (i = 0; i < wpages; i++)
				mm_free_page((unsigned int)ws + i * 0x1000);
			return -1;
		}
		{
			unsigned int i;

			for (i = 0; i < wpages; i++)
				mm_free_page((unsigned int)ws + i * 0x1000);
		}
		h = ramfs_validate(out, isize);
		if (!h) {
			unsigned int i;

			for (i = 0; i < pages; i++)
				mm_free_page((unsigned int)out + i * 0x1000);
			return -1;
		}
		pack_base = out;
		pack_size = isize;
		pack_hdr = h;
		pack_pages = pages;
		return 0;
	}

	/* Raw pack: copy it so the caller can release the boot region. */
	pages = (size + 0xFFF) >> 12;
	{
		unsigned char *copy = (unsigned char *)mm_alloc_pages(pages);
		unsigned int i;

		if (!copy)
			return -1;
		for (i = 0; i < size; i++)
			copy[i] = p[i];
		h = ramfs_validate(copy, size);
		if (!h) {
			for (i = 0; i < pages; i++)
				mm_free_page((unsigned int)copy + i * 0x1000);
			return -1;
		}
		pack_base = copy;
	}
	pack_size = size;
	pack_hdr = h;
	pack_pages = pages;
	return 0;
}

int ramfs_replace(const void *pack, unsigned int size)
{
	const struct ramfs_hdr *h = ramfs_validate(pack, size);
	const unsigned char *old_base = pack_base;
	unsigned int old_size = pack_size, old_pages = pack_pages;
	unsigned int i;

	if (!h)
		return -1;

	pack_base = (const unsigned char *)pack;
	pack_size = size;
	pack_hdr = h;
	pack_pages = (size + 0xFFFu) >> 12;

	/* Free a previously received pack. */
	for (i = 0; i < old_pages; i++)
		mm_free_page((unsigned int)old_base + i * 0x1000);
	(void)old_size;
	return 0;
}

const void *ramfs_lookup(const char *path, unsigned int *size)
{
	const struct ramfs_ent *ents;
	unsigned int i;

	if (!pack_hdr || !path_ok(path))
		return 0;
	ents = (const struct ramfs_ent *)(pack_base + sizeof(*pack_hdr));
	for (i = 0; i < pack_hdr->count; i++) {
		if (str_eq(ents[i].path, path)) {
			if (size)
				*size = ents[i].size;
			return pack_base + ents[i].off;
		}
	}
	return 0;
}

int ramfs_list(const char *dir, unsigned int index, struct ramfs_dirent *out)
{
	const struct ramfs_ent *ents;
	unsigned int i, seen = 0;

	if (!pack_hdr)
		return 0;
	ents = (const struct ramfs_ent *)(pack_base + sizeof(*pack_hdr));

	for (i = 0; i < pack_hdr->count; i++) {
		const char *name;
		unsigned int j, len;

		if (dir[0] == '\0') {
			/* Root: the unique first components, reported as dirs. */
			unsigned int dup = 0;

			for (j = 0; j < i; j++) {
				if (first_component_same(ents[i].path, ents[j].path)) {
					dup = 1;
					break;
				}
			}
			if (dup)
				continue;
			len = first_len(ents[i].path);
			if (len >= RAMFS_PATH_MAX)
				continue;
			if (seen != index) {
				seen++;
				continue;
			}
			for (j = 0; j < len; j++)
				out->name[j] = ents[i].path[j];
			out->name[len] = '\0';
			out->size = 0;
			out->is_dir = 1;
			return 1;
		}

		name = strip_dir(ents[i].path, dir);
		if (!name)
			continue;
		if (seen != index) {
			seen++;
			continue;
		}
		len = 0;
		while (name[len] && name[len] != '/' && len < RAMFS_PATH_MAX - 1)
			len++;
		for (j = 0; j < len; j++)
			out->name[j] = name[j];
		out->name[len] = '\0';
		out->size = ents[i].size;
		out->is_dir = (name[len] == '/') ? 1 : 0;
		return 1;
	}
	return 0;
}

unsigned int ramfs_count(void)
{
	return pack_hdr ? pack_hdr->count : 0;
}

unsigned int ramfs_pack_size(void)
{
	return pack_hdr ? pack_size : 0;
}

/* Base of the owned pack (NULL when there is no pack or it is not ours).
 * A kexec reload passes this to the new kernel so a kernel-only update can
 * keep the commands. */
const void *ramfs_pack_base(void)
{
	return pack_pages ? pack_base : 0;
}
