/* ramfs: read-only in-memory filesystem backed by the command pack (mkpack).
 * The pack is copied into page-allocated memory at boot (see ramfs_init), so
 * the boot path's copy in high RAM can be released.  Paths inside it are
 * relative to the mount root (e.g. "bin/ls"). */
#ifndef RAMFS_H
#define RAMFS_H

#define RAMFS_PATH_MAX 32
#define RAMFS_MAX_FILES 128

struct ramfs_dirent {
	char name[RAMFS_PATH_MAX];
	unsigned int size;
	unsigned int is_dir;
};

/* Validate a pack and make it the ramfs contents.  The pack may be gzip or
 * raw; it is copied/inflated into page-allocated memory.  Returns 0 on
 * success. */
int ramfs_init(const void *pack, unsigned int size);

/* Replace the contents with a freshly received pack (page-allocated): the
 * old pack is validated first, so a bad image leaves the current one in
 * place; on success the previous heap copy is freed.  Returns 0 on success. */
int ramfs_replace(const void *pack, unsigned int size);

/* Look up one path (no leading '/', e.g. "bin/ls"); returns a pointer into
 * the pack and its size, or NULL when absent. */
const void *ramfs_lookup(const char *path, unsigned int *size);

/* List one directory: "" for the mount root, "bin" for /ramfs/bin.  Returns
 * 1 when an entry was filled, 0 at the end. */
int ramfs_list(const char *dir, unsigned int index, struct ramfs_dirent *out);

/* Number of files in the pack (0 when no pack is loaded). */
unsigned int ramfs_count(void);

/* Size of the loaded pack in bytes (0 when none). */
unsigned int ramfs_pack_size(void);

/* Owned pack base (NULL when there is no pack), for kexec handoff. */
const void *ramfs_pack_base(void);

#endif
