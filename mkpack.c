/* mkpack: build the command pack that the kernel embeds and mounts at /ramfs.
 *
 * Usage: mkpack <out.pack> name=file [name=file ...]
 *
 * Format (little-endian):
 *   offset 0    char magic[8] = "NBPAK1\0\0"
 *   offset 8    u32 count
 *   offset 12   u32 total_size
 *   offset 16   count entries: { char path[32]; u32 offset; u32 size; }
 *   ...         file data, each 4-byte aligned
 * All offsets are relative to the start of the pack. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const char MAGIC[8] = { 'N', 'B', 'P', 'A', 'K', '1', '\0', '\0' };
#define PATH_MAX_ 32

struct hdr {
	char magic[8];
	uint32_t count;
	uint32_t total;
};

struct ent {
	char path[PATH_MAX_];
	uint32_t off;
	uint32_t size;
};

static void put32le(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
	p[2] = (unsigned char)((v >> 16) & 0xFF);
	p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static unsigned char *read_file(const char *path, long *size)
{
	FILE *f = fopen(path, "rb");
	unsigned char *data;
	long n;

	if (!f) {
		fprintf(stderr, "mkpack: %s: cannot open\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) {
		fprintf(stderr, "mkpack: %s: empty\n", path);
		exit(1);
	}
	data = malloc((size_t)n);
	if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "mkpack: %s: read failed\n", path);
		exit(1);
	}
	fclose(f);
	*size = n;
	return data;
}

int main(int argc, char **argv)
{
	struct hdr h;
	struct ent *ents;
	unsigned char *out;
	unsigned int i, off, total;
	const char *eq;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <out.pack> name=file [name=file ...]\n",
			argv[0]);
		return 1;
	}

	ents = calloc((size_t)(argc - 2), sizeof(*ents));
	if (!ents) {
		fprintf(stderr, "mkpack: out of memory\n");
		return 1;
	}

	/* Reserve the header + table; data follows.  Build in memory: the
	 * pack is a few hundred KB at most. */
	off = (unsigned int)(sizeof(h) + (unsigned int)(argc - 2) * sizeof(*ents));
	out = malloc(8u * 1024u * 1024u);
	if (!out) {
		fprintf(stderr, "mkpack: out of memory\n");
		return 1;
	}

	for (i = 0; i < (unsigned int)(argc - 2); i++) {
		unsigned char *data;
		long n;
		unsigned int name_len;

		eq = strchr(argv[2 + i], '=');
		if (!eq || eq == argv[2 + i]) {
			fprintf(stderr, "mkpack: expected name=file, got '%s'\n",
				argv[2 + i]);
			return 1;
		}
		name_len = (unsigned int)(eq - argv[2 + i]);
		if (name_len >= PATH_MAX_) {
			fprintf(stderr, "mkpack: name too long: %.*s\n",
				(int)name_len, argv[2 + i]);
			return 1;
		}
		memcpy(ents[i].path, argv[2 + i], name_len);
		ents[i].path[name_len] = '\0';

		data = read_file(eq + 1, &n);
		off = (off + 3u) & ~3u;              /* align each file */
		if (off + (unsigned int)n > 8u * 1024u * 1024u) {
			fprintf(stderr, "mkpack: pack too large\n");
			return 1;
		}
		memcpy(out + off, data, (size_t)n);
		ents[i].off = off;
		ents[i].size = (uint32_t)n;
		off += (unsigned int)n;
		free(data);
	}

	total = off;
	memcpy(h.magic, MAGIC, sizeof(MAGIC));
	put32le((unsigned char *)&h.count, (uint32_t)(argc - 2));
	put32le((unsigned char *)&h.total, total);
	memcpy(out, &h, sizeof(h));
	for (i = 0; i < (unsigned int)(argc - 2); i++) {
		unsigned char *e = out + sizeof(h) + i * sizeof(*ents);
		memcpy(e, ents[i].path, PATH_MAX_);
		put32le(e + PATH_MAX_, ents[i].off);
		put32le(e + PATH_MAX_ + 4, ents[i].size);
	}

	{
		FILE *f = fopen(argv[1], "wb");
		if (!f || fwrite(out, 1, total, f) != total) {
			fprintf(stderr, "mkpack: %s: write failed\n", argv[1]);
			return 1;
		}
		fclose(f);
	}

	fprintf(stderr, "mkpack: %s: %u files, %u bytes\n", argv[1],
		(unsigned int)(argc - 2), total);
	return 0;
}
