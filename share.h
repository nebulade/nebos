/* /share: writable, page-backed in-memory filesystem (see share.c).
 *
 * The board is usually netbooted without a writable disk, so the WebDAV
 * server needs somewhere to put uploads.  Names are plain strings (not 8.3)
 * and may contain spaces; everything is volatile (a reboot/kexec starts
 * empty).  The VFS mounts it at /share. */
#ifndef SHARE_H
#define SHARE_H

struct fs_ops;

#define SHARE_PATH_MAX     64
#define SHARE_MAX_ENTRIES  64
#define SHARE_MAX_BYTES    (16u * 1024u * 1024u)

/* Allocate the entry table.  Idempotent; 0 when ready, -1 when out of
 * memory.  Files added before a second call are preserved. */
int share_init(void);

extern const struct fs_ops share_ops;

#endif
