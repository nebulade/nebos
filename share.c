/* /share: a small writable in-memory filesystem.
 *
 * Files live in whole pages from the frame allocator (identity mapped, so a
 * borrowed pointer works), and the directory namespace is the full relative
 * path of every entry, like the pack ramfs.  That keeps mkdir/list/rmdir
 * simple without an inode tree.  A directory exists as an entry of its own,
 * so empty directories survive.  Everything is volatile.
 */

#include "vfs.h"
#include "share.h"
#include "kmalloc.h"
#include "mm.h"

struct share_ent {
	char path[SHARE_PATH_MAX];
	unsigned int base;     /* first page, 0 when the file is empty */
	unsigned int size;
	unsigned int is_dir;
};

static struct share_ent *ents;
static unsigned int ent_count;
static unsigned int used_bytes;

static int s_eq(const char *a, const char *b)
{
	unsigned int i = 0;

	while (a[i] && b[i] && a[i] == b[i])
		i++;
	return a[i] == '\0' && b[i] == '\0';
}

static unsigned int s_len(const char *s)
{
	unsigned int n = 0;

	while (s[n])
		n++;
	return n;
}

static void s_copy(char *d, const char *s, unsigned int cap)
{
	unsigned int i = 0;

	while (i + 1 < cap && s[i]) {
		d[i] = s[i];
		i++;
	}
	d[i] = '\0';
}

static void mem_copy(void *d, const void *s, unsigned int n)
{
	unsigned char *dd = d;
	const unsigned char *ss = s;

	while (n--)
		*dd++ = *ss++;
}

/* A relative path: components separated by '/', no leading/trailing slash,
 * no "." or "..", no control characters, total length < SHARE_PATH_MAX. */
static int path_ok(const char *path)
{
	unsigned int i = 0, comp = 0;

	if (!path || !path[0])
		return 0;
	while (path[i]) {
		char c = path[i];

		if (c == '/') {
			if (comp == 0)
				return 0;
			if (comp == 1 && path[i - 1] == '.')
				return 0;
			if (comp == 2 && path[i - 1] == '.' &&
			    path[i - 2] == '.')
				return 0;
			comp = 0;
		} else {
			if ((unsigned char)c < 0x20)
				return 0;
			if (++comp > SHARE_PATH_MAX - 2)
				return 0;
		}
		if (++i >= SHARE_PATH_MAX)
			return 0;
	}
	if (comp == 0)
		return 0;
	if (comp == 1 && path[i - 1] == '.')
		return 0;
	if (comp == 2 && path[i - 1] == '.' && path[i - 2] == '.')
		return 0;
	return 1;
}

static void parent_of(const char *path, char *parent)
{
	int i = (int)s_len(path) - 1;

	while (i >= 0 && path[i] != '/')
		i--;
	if (i < 0) {
		parent[0] = '\0';
		return;
	}
	s_copy(parent, path, (unsigned int)i + 1);
	parent[i] = '\0';
}

static int lookup(const char *path)
{
	unsigned int i;

	for (i = 0; i < ent_count; i++)
		if (s_eq(ents[i].path, path))
			return (int)i;
	return -1;
}

static void free_data(unsigned int base, unsigned int size)
{
	unsigned int pages = (size + 0xFFFu) >> 12;
	unsigned int i;

	if (!base)
		return;
	for (i = 0; i < pages; i++)
		mm_free_page(base + i * 0x1000);
}

static void remove_ent(unsigned int idx)
{
	free_data(ents[idx].base, ents[idx].size);
	used_bytes -= ents[idx].size;
	ent_count--;
	if (idx != ent_count)
		ents[idx] = ents[ent_count];
}

/* Full path of the child of `dir` containing `p`, into `name`.  Returns 0
 * when the entry is not directly inside `dir`. */
static int child_name(const char *p, const char *dir, char *name,
		      unsigned int cap)
{
	unsigned int dl = s_len(dir), n = 0, i;
	const char *rest;

	if (dl) {
		for (i = 0; i < dl; i++)
			if (p[i] != dir[i])
				return 0;
		if (p[dl] != '/')
			return 0;
		rest = p + dl + 1;
	} else {
		rest = p;
	}
	if (!rest[0])
		return 0;
	while (rest[n] && rest[n] != '/')
		n++;
	if (n >= cap)
		return 0;
	mem_copy(name, rest, n);
	name[n] = '\0';
	return 1;
}

/* ---- fs_ops ---- */

static int share_op_read_file(struct vfs_mount *m, const char *path,
			      void *buf, unsigned int max)
{
	int idx;

	(void)m;
	idx = lookup(path);
	if (idx < 0)
		return VFS_ENOENT;
	if (ents[idx].is_dir)
		return VFS_EINVAL;
	if (ents[idx].size > max)
		return (int)max;
	mem_copy(buf, (const void *)ents[idx].base, ents[idx].size);
	return (int)ents[idx].size;
}

static int share_op_load_file(struct vfs_mount *m, const char *path,
			      struct vfs_file *out)
{
	int idx;

	(void)m;
	idx = lookup(path);
	if (idx < 0)
		return VFS_ENOENT;
	if (ents[idx].is_dir)
		return VFS_EINVAL;
	out->data = (const void *)ents[idx].base;
	out->size = ents[idx].size;
	return 0;
}

static void share_op_release_file(struct vfs_mount *m, struct vfs_file *f)
{
	(void)m;
	(void)f;
}

static int share_op_stat(struct vfs_mount *m, const char *path,
			 unsigned int *size, int *is_dir)
{
	int idx;

	(void)m;
	if (path[0] == '\0') {
		if (size)
			*size = 0;
		if (is_dir)
			*is_dir = 1;
		return 0;
	}
	idx = lookup(path);
	if (idx < 0)
		return VFS_ENOENT;
	if (size)
		*size = ents[idx].is_dir ? 0 : ents[idx].size;
	if (is_dir)
		*is_dir = (int)ents[idx].is_dir;
	return 0;
}

static int share_op_write_file(struct vfs_mount *m, const char *path,
			       const void *buf, unsigned int len)
{
	char parent[SHARE_PATH_MAX];
	unsigned int base = 0, old_size = 0;
	int idx, pidx;

	(void)m;
	if (!path_ok(path))
		return VFS_EINVAL;

	idx = lookup(path);
	if (idx >= 0 && ents[idx].is_dir)
		return VFS_EINVAL;

	parent_of(path, parent);
	if (parent[0]) {
		pidx = lookup(parent);
		if (pidx < 0 || !ents[pidx].is_dir)
			return VFS_EINVAL;
	}

	if (idx >= 0)
		old_size = ents[idx].size;
	if (used_bytes - old_size + len > SHARE_MAX_BYTES)
		return VFS_ENOSPC;

	if (len) {
		unsigned int pages = (len + 0xFFFu) >> 12;

		base = mm_alloc_pages(pages);
		if (!base)
			return VFS_ENOSPC;
		mem_copy((void *)base, buf, len);
	}

	if (idx < 0) {
		if (ent_count >= SHARE_MAX_ENTRIES) {
			free_data(base, len);
			return VFS_ENOSPC;
		}
		idx = (int)ent_count++;
		s_copy(ents[idx].path, path, sizeof(ents[idx].path));
		ents[idx].base = 0;
		ents[idx].size = 0;
		ents[idx].is_dir = 0;
	}

	free_data(ents[idx].base, ents[idx].size);
	used_bytes -= ents[idx].size;
	ents[idx].base = base;
	ents[idx].size = len;
	ents[idx].is_dir = 0;
	used_bytes += len;
	return 0;
}

static int share_op_unlink(struct vfs_mount *m, const char *path)
{
	int idx = lookup(path);

	(void)m;
	if (idx < 0)
		return VFS_ENOENT;
	if (ents[idx].is_dir)
		return VFS_EINVAL;
	remove_ent((unsigned int)idx);
	return 0;
}

static int share_op_mkdir(struct vfs_mount *m, const char *path)
{
	char parent[SHARE_PATH_MAX];
	unsigned int i;
	int pidx;

	(void)m;
	if (!path_ok(path) || lookup(path) >= 0)
		return VFS_EINVAL;

	parent_of(path, parent);
	if (parent[0]) {
		pidx = lookup(parent);
		if (pidx < 0 || !ents[pidx].is_dir)
			return VFS_EINVAL;
	}

	/* Do not create a directory whose path is already a file prefix. */
	for (i = 0; i < ent_count; i++) {
		unsigned int pl = s_len(path);

		if (s_len(ents[i].path) > pl &&
		    ents[i].path[pl] == '/') {
			unsigned int j;

			for (j = 0; j < pl && ents[i].path[j] == path[j]; j++)
				;
			if (j == pl)
				return VFS_EINVAL;
		}
	}

	if (ent_count >= SHARE_MAX_ENTRIES)
		return VFS_ENOSPC;
	i = ent_count++;
	s_copy(ents[i].path, path, sizeof(ents[i].path));
	ents[i].base = 0;
	ents[i].size = 0;
	ents[i].is_dir = 1;
	return 0;
}

static int share_op_rmdir(struct vfs_mount *m, const char *path)
{
	unsigned int i, pl;
	int idx = lookup(path);

	(void)m;
	if (idx < 0)
		return VFS_ENOENT;
	if (!ents[idx].is_dir)
		return VFS_EINVAL;

	pl = s_len(path);
	for (i = 0; i < ent_count; i++) {
		unsigned int j;

		if (s_len(ents[i].path) <= pl || ents[i].path[pl] != '/')
			continue;
		for (j = 0; j < pl && ents[i].path[j] == path[j]; j++)
			;
		if (j == pl)
			return VFS_EINVAL;      /* not empty */
	}
	remove_ent((unsigned int)idx);
	return 0;
}

static int share_op_list_dir(struct vfs_mount *m, const char *path,
			     unsigned int index, struct vfs_dirent *out)
{
	char name[SHARE_PATH_MAX], seen_name[SHARE_PATH_MAX];
	unsigned int i, seen = 0;
	int idx;

	(void)m;
	if (path[0]) {
		idx = lookup(path);
		if (idx < 0)
			return VFS_ENOENT;
		if (!ents[idx].is_dir)
			return VFS_EINVAL;
	}

	for (i = 0; i < ent_count; i++) {
		unsigned int j;
		int dup = 0, exact;
		char full[SHARE_PATH_MAX];

		if (!child_name(ents[i].path, path, name, sizeof(name)))
			continue;
		for (j = 0; j < i; j++) {
			if (child_name(ents[j].path, path, seen_name,
				       sizeof(seen_name)) &&
			    s_eq(seen_name, name)) {
				dup = 1;
				break;
			}
		}
		if (dup)
			continue;
		if (seen++ != index)
			continue;

		/* The child is a directory when its own entry says so. */
		if (path[0]) {
			unsigned int k = 0;

			for (; path[k] && k + 1 < sizeof(full); k++)
				full[k] = path[k];
			if (k + 1 < sizeof(full))
				full[k++] = '/';
			for (j = 0; name[j] && k + 1 < sizeof(full); j++)
				full[k++] = name[j];
			full[k] = '\0';
		} else {
			s_copy(full, name, sizeof(full));
		}
		exact = lookup(full);
		s_copy(out->name, name, sizeof(out->name));
		out->size = (exact >= 0 && !ents[exact].is_dir) ?
			    ents[exact].size : 0;
		out->is_dir = (exact >= 0 && ents[exact].is_dir) ? 1 : 0;
		out->raw_83 = 0;
		return 1;
	}
	return 0;
}

const struct fs_ops share_ops = {
	.raw_83 = 0,
	.read_file = share_op_read_file,
	.load_file = share_op_load_file,
	.release_file = share_op_release_file,
	.stat = share_op_stat,
	.write_file = share_op_write_file,
	.unlink = share_op_unlink,
	.mkdir = share_op_mkdir,
	.rmdir = share_op_rmdir,
	.list_dir = share_op_list_dir,
};

int share_init(void)
{
	if (ents)
		return 0;
	ents = kmalloc(sizeof(*ents) * SHARE_MAX_ENTRIES);
	if (!ents)
		return -1;
	ent_count = 0;
	used_bytes = 0;
	return 0;
}
