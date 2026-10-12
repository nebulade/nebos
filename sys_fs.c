/* File service for ring-3 commands (see syscall.h for the ABI).
 *
 * Filesystem-agnostic: paths resolve through the VFS (vfs.c), so FAT volumes
 * and the ramfs behave the same.  Two properties shape how these run:
 *   - the FAT driver keeps state in static buffers, so no other process may
 *     run its code concurrently;
 *   - the floppy driver waits for IRQ6, so interrupts must stay enabled.
 * So instead of a lock we set `sys_fs_busy` for the duration: the scheduler
 * (process.c) will not switch away from the running process while it is set.
 * That keeps each operation atomic (and immune to Ctrl+C killing the process
 * mid-write) at the cost of a brief stall during disk I/O.  Buffers point
 * into the caller's address space, which is also the active one during the
 * syscall, so no copy in/out is needed. */

#include "syscall.h"
#include "process.h"
#include "vfs.h"

volatile int sys_fs_busy;

void sys_fs_enter(void)
{
	sys_fs_busy = 1;
	__asm__ volatile ("sti");   /* the disk may need its IRQ */
}

void sys_fs_exit(void)
{
	__asm__ volatile ("cli");
	sys_fs_busy = 0;
}

static int sys_err(int vfs_err)
{
	switch (vfs_err) {
	case VFS_EIO:    return SYS_EIO;
	case VFS_ENOENT: return SYS_ENOENT;
	case VFS_EROFS:  return SYS_EROFS;
	default:         return SYS_EINVAL;
	}
}

/* Resolve an absolute path to (mount, relative path); returns 0 or a SYS_E*
 * code (mirrors the old "no such device" vs "no filesystem" distinction). */
static int fs_resolve(const char *path, struct vfs_mount **m,
		      char *rel, unsigned int rel_size)
{
	char abs[VFS_PATH_MAX * 2];

	if (!path)
		return SYS_ENODEV;

	/* Relative paths resolve against the calling process's cwd (inherited
	 * from the terminal that spawned it); "" means the cwd itself. */
	if (path[0] != '/') {
		if (vfs_join_path(procs[current_pid].cwd, path, abs, sizeof(abs)) != 0)
			return SYS_EINVAL;
		path = abs;
	}
	if (path[0] == '/' && path[1] == '\0')
		path = "/";     /* the root listing goes through vfs_resolve below */
	if (vfs_resolve(path, m, rel, rel_size) != 0)
		return vfs_device_known(path) ? SYS_ENOFS : SYS_ENODEV;
	return 0;
}

int sys_read_file(const char *path, void *buf, unsigned int max)
{
	struct vfs_mount *m;
	char rel[VFS_PATH_MAX];
	int r;

	sys_fs_enter();
	r = fs_resolve(path, &m, rel, sizeof(rel));
	if (r == 0) {
		r = m->ops->read_file(m, rel, buf, max);
		if (r < 0)
			r = sys_err(r);
	}
	sys_fs_exit();
	return r;
}

int sys_write_file(const char *path, const void *buf, unsigned int len)
{
	struct vfs_mount *m;
	char rel[VFS_PATH_MAX];
	int r;

	sys_fs_enter();
	r = fs_resolve(path, &m, rel, sizeof(rel));
	if (r == 0) {
		r = m->ops->write_file(m, rel, buf, len);
		if (r < 0)
			r = sys_err(r);
	}
	sys_fs_exit();
	return r;
}

int sys_unlink(const char *path)
{
	struct vfs_mount *m;
	char rel[VFS_PATH_MAX];
	int r;

	sys_fs_enter();
	r = fs_resolve(path, &m, rel, sizeof(rel));
	if (r == 0) {
		r = m->ops->unlink(m, rel);
		if (r < 0)
			r = sys_err(r);
	}
	sys_fs_exit();
	return r;
}

int sys_mkdir(const char *path)
{
	struct vfs_mount *m;
	char rel[VFS_PATH_MAX];
	int r;

	sys_fs_enter();
	r = fs_resolve(path, &m, rel, sizeof(rel));
	if (r == 0) {
		r = m->ops->mkdir(m, rel);
		if (r < 0)
			r = sys_err(r);
	}
	sys_fs_exit();
	return r;
}

int sys_rmdir(const char *path)
{
	struct vfs_mount *m;
	char rel[VFS_PATH_MAX];
	int r;

	sys_fs_enter();
	r = fs_resolve(path, &m, rel, sizeof(rel));
	if (r == 0) {
		r = m->ops->rmdir(m, rel);
		if (r < 0)
			r = sys_err(r);
	}
	sys_fs_exit();
	return r;
}

int sys_list_dir(const char *path, unsigned int index, struct sys_dirent *out)
{
	struct vfs_mount *m;
	char rel[VFS_PATH_MAX];
	int r;

	sys_fs_enter();
	r = fs_resolve(path, &m, rel, sizeof(rel));
	if (r == 0) {
		struct vfs_dirent d;
		unsigned int i;

		r = m->ops->list_dir(m, rel, index, &d);
		if (r == 1) {
			for (i = 0; i < sizeof(out->name) - 1 && d.name[i]; i++)
				out->name[i] = d.name[i];
			out->name[i] = '\0';
			out->size = d.size;
			out->is_dir = d.is_dir;
			r = 1;
		} else if (r < 0) {
			r = sys_err(r);
		} else {
			r = 0;
		}
	}
	sys_fs_exit();
	return r;
}

static void copy_bounded(char *dst, const char *src, unsigned int size)
{
	unsigned int i = 0;

	while (i + 1 < size && src[i]) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

int sys_list_mounts(unsigned int index, struct sys_mount *out)
{
	if (index >= vfs_mount_count)
		return 0;
	copy_bounded(out->name, vfs_mounts[index].name, sizeof(out->name));
	copy_bounded(out->type, vfs_mounts[index].type, sizeof(out->type));
	out->total_sectors = vfs_mounts[index].total_sectors;
	copy_bounded(out->label, vfs_mounts[index].label, sizeof(out->label));
	return 1;
}
