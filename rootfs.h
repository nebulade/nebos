/* Rootfs image format (see mkrootfs.c).
 *
 * The command pack is the system rootfs: a gzip-compressed NBPAK1 pack with
 * a one-sector header, so the boot loader can find its length and checksum
 * without a filesystem.  The header is sector 0 of the image (or of the
 * rootfs partition); the pack starts at byte 512.
 *
 *   0   char magic[8]        "NBROOT1\0"
 *   8   u32 version
 *   12  u32 pack_bytes       exact gzip length (the ISIZE trailer matters)
 *   16  u32 pack_csum        sum of the pack bytes, mod 2^32
 *   20  u32 flags            ROOTFS_FLAG_GZIP
 *   ... zero padding to 512
 */
#ifndef ROOTFS_H
#define ROOTFS_H

#define ROOTFS_MAGIC         "NBROOT1"
#define ROOTFS_VERSION       1u
#define ROOTFS_FLAG_GZIP     0x00000001u

#define ROOTFS_HDR_SIZE      512u
#define ROOTFS_PACK_OFFSET   512u

struct rootfs_hdr {
	char magic[8];
	unsigned int version;
	unsigned int pack_bytes;
	unsigned int pack_csum;
	unsigned int flags;
	unsigned int reserved[(ROOTFS_HDR_SIZE - 24) / 4];
} __attribute__((packed));

#endif
