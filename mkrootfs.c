/* mkrootfs: wrap a gzip command pack in a bootable rootfs image.
 *
 * Usage: mkrootfs <out.img> <pack.gz> [size]
 *
 * The image starts with a struct rootfs_hdr (rootfs.h) in its first 512-byte
 * sector and the pack at byte 512.  A non-zero size pads the image to that
 * many bytes (the USB rootfs partition); size 0 keeps it minimal (the floppy
 * image appends the header + pack directly after the kernel).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "rootfs.h"

static void put32le(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)(v & 0xFF);
	p[1] = (unsigned char)((v >> 8) & 0xFF);
	p[2] = (unsigned char)((v >> 16) & 0xFF);
	p[3] = (unsigned char)((v >> 24) & 0xFF);
}

int main(int argc, char **argv)
{
	unsigned char hdr[ROOTFS_HDR_SIZE];
	const char *out_path, *pack_path;
	unsigned long size = 0, total;
	unsigned char *pack;
	uint32_t csum = 0;
	FILE *f;
	long n;
	unsigned long i;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s <out.img> <pack.gz> [size]\n",
			argv[0]);
		return 1;
	}
	out_path = argv[1];
	pack_path = argv[2];
	if (argc == 4) {
		char *end;

		size = strtoul(argv[3], &end, 0);
		if (*end || size == 0) {
			fprintf(stderr, "mkrootfs: bad size '%s'\n", argv[3]);
			return 1;
		}
	}

	f = fopen(pack_path, "rb");
	if (!f) {
		fprintf(stderr, "mkrootfs: %s: cannot open\n", pack_path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) {
		fprintf(stderr, "mkrootfs: %s: empty\n", pack_path);
		fclose(f);
		return 1;
	}
	pack = malloc((size_t)n);
	if (!pack || fread(pack, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "mkrootfs: %s: read failed\n", pack_path);
		fclose(f);
		free(pack);
		return 1;
	}
	fclose(f);

	for (i = 0; i < (unsigned long)n; i++)
		csum += pack[i];

	total = ROOTFS_HDR_SIZE + (unsigned long)n;
	if (size && size < total) {
		fprintf(stderr, "mkrootfs: size %lu is smaller than the image "
			"(%lu bytes)\n", size, total);
		free(pack);
		return 1;
	}

	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, ROOTFS_MAGIC, 7);
	put32le(hdr + 8, ROOTFS_VERSION);
	put32le(hdr + 12, (uint32_t)n);
	put32le(hdr + 16, csum);
	put32le(hdr + 20, ROOTFS_FLAG_GZIP);

	f = fopen(out_path, "wb");
	if (!f) {
		fprintf(stderr, "mkrootfs: %s: cannot create\n", out_path);
		free(pack);
		return 1;
	}
	if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
	    fwrite(pack, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "mkrootfs: %s: write failed\n", out_path);
		fclose(f);
		free(pack);
		return 1;
	}
	free(pack);

	if (size) {
		static const unsigned char zero[4096];

		for (i = total; i < size; ) {
			unsigned long chunk = size - i;

			if (chunk > sizeof(zero))
				chunk = sizeof(zero);
			if (fwrite(zero, 1, chunk, f) != chunk) {
				fprintf(stderr, "mkrootfs: %s: pad failed\n",
					out_path);
				fclose(f);
				return 1;
			}
			i += chunk;
		}
	}

	if (fclose(f) != 0) {
		fprintf(stderr, "mkrootfs: %s: close failed\n", out_path);
		return 1;
	}

	fprintf(stderr, "mkrootfs: %s: pack %ld bytes, total %lu bytes\n",
		out_path, n, size ? size : total);
	return 0;
}
