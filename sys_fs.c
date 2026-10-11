/* File service for ring-3 commands (see syscall.h for the ABI).
 *
 * The VFS/FAT layer and the disk drivers stay in the kernel; these calls
 * expose whole-file operations on absolute paths.  Two properties shape how
 * they run:
 *   - the FAT driver keeps state in static buffers, so no other process may
 *     run its FAT code concurrently;
 *   - the floppy driver waits for IRQ6, so interrupts must stay enabled.
 * So instead of a lock we set `sys_fs_busy` for the duration: the scheduler
 * (schedule() in process.c) will not switch away from the running process
 * while it is set.  That keeps each operation atomic (and immune to Ctrl+C
 * killing the process mid-write) at the cost of a brief system stall during
 * disk I/O.  Buffers point into the caller's address space, which is also the
 * active one during the syscall, so no copy in/out is needed. */

#include "syscall.h"
#include "fat.h"
#include "vfs.h"

volatile int sys_fs_busy;

static void fs_enter(void)
{
	sys_fs_busy = 1;
	__asm__ volatile ("sti");   /* the disk may need its IRQ */
}

static void fs_exit(void)
{
	__asm__ volatile ("cli");
	sys_fs_busy = 0;
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

static const char *fat_type_name(fat_type_t t)
{
	switch (t) {
	case FAT12: return "FAT12";
	case FAT16: return "FAT16";
	case FAT32: return "FAT32";
	default:    return "FAT";
	}
}

/* Resolve `path` to (filesystem, parent directory cluster, final 8.3 name).
 * Returns 0 or a SYS_E* code. */
static int fs_parent(const char *path, fat_fs_t **fs_out,
		     unsigned int *dir_cluster, char name_8_3[12])
{
	fat_fs_t *fs;
	const char *fs_path;
	int r;

	if (!path || !path[0])
		return SYS_ENODEV;
	if (vfs_resolve(path, &fs, &fs_path) != 0)
		return vfs_device_known(path) ? SYS_ENOFS : SYS_ENODEV;
	r = fat_resolve_path(fs, fs_path, dir_cluster, name_8_3);
	if (r == -2)
		return SYS_EIO;
	if (r != 0)
		return SYS_ENOENT;
	*fs_out = fs;
	return 0;
}

/* Resolve `path` to (filesystem, directory cluster). */
static int fs_dir(const char *path, fat_fs_t **fs_out, unsigned int *dir_cluster)
{
	fat_fs_t *fs;
	const char *fs_path;
	int r;

	if (!path || !path[0])
		return SYS_ENODEV;
	if (vfs_resolve(path, &fs, &fs_path) != 0)
		return vfs_device_known(path) ? SYS_ENOFS : SYS_ENODEV;
	r = fat_resolve_dir(fs, fs_path, dir_cluster);
	if (r == -2)
		return SYS_EIO;
	if (r != 0)
		return SYS_EINVAL;      /* exists, but not a directory */
	*fs_out = fs;
	return 0;
}

int sys_read_file(const char *path, void *buf, unsigned int max)
{
	fat_fs_t *fs;
	unsigned int dir;
	char name[12];
	int r;

	fs_enter();
	r = fs_parent(path, &fs, &dir, name);
	if (r == 0) {
		r = fat_read_file_in(fs, dir, name, buf, max);
		if (r == -2)
			r = SYS_EIO;
		else if (r < 0)
			r = SYS_ENOENT;
	}
	fs_exit();
	return r;
}

int sys_write_file(const char *path, const void *buf, unsigned int len)
{
	fat_fs_t *fs;
	unsigned int dir;
	char name[12];
	int r;

	fs_enter();
	r = fs_parent(path, &fs, &dir, name);
	if (r == 0) {
		r = fat_write_file_in(fs, dir, name, buf, len);
		if (r == -2)
			r = SYS_EIO;
		else if (r != 0)
			r = SYS_EWRITE;
	}
	fs_exit();
	return r;
}

int sys_unlink(const char *path)
{
	fat_fs_t *fs;
	unsigned int dir;
	char name[12];
	int r;

	fs_enter();
	r = fs_parent(path, &fs, &dir, name);
	if (r == 0) {
		r = fat_delete_file_in(fs, dir, name);
		if (r == -2)
			r = SYS_EIO;
		else if (r != 0)
			r = SYS_ENOENT;
	}
	fs_exit();
	return r;
}

int sys_mkdir(const char *path)
{
	fat_fs_t *fs;
	unsigned int dir;
	char name[12];
	int r;

	fs_enter();
	r = fs_parent(path, &fs, &dir, name);
	if (r == 0) {
		r = fat_mkdir(fs, dir, name);
		if (r == -2)
			r = SYS_EIO;
		else if (r != 0)
			r = SYS_EWRITE;
	}
	fs_exit();
	return r;
}

int sys_rmdir(const char *path)
{
	fat_fs_t *fs;
	unsigned int dir;
	char name[12];
	int r;

	fs_enter();
	r = fs_parent(path, &fs, &dir, name);
	if (r == 0) {
		r = fat_rmdir(fs, dir, name);
		if (r == -2)
			r = SYS_EIO;
		else if (r != 0)
			r = SYS_EWRITE;
	}
	fs_exit();
	return r;
}

/* Directory listing: index-based, so the kernel keeps no per-process state.
 * `fat_list_dir` walks from the start each call; directories are small. */
static unsigned int list_index;
static unsigned int list_seen;
static struct sys_dirent *list_out;
static int list_done;

static void dirent_cb(const char *name, int is_dir, unsigned int size)
{
	unsigned int i;

	if (list_done || list_seen < list_index) {
		list_seen++;
		return;
	}
	for (i = 0; i < 11; i++)
		list_out->name[i] = name[i] ? name[i] : ' ';
	list_out->name[11] = '\0';
	list_out->size = size;
	list_out->is_dir = (unsigned int)is_dir;
	list_done = 1;
	list_seen++;
}

int sys_list_dir(const char *path, unsigned int index, struct sys_dirent *out)
{
	fat_fs_t *fs;
	unsigned int dir;
	int r;

	fs_enter();
	r = fs_dir(path, &fs, &dir);
	if (r == 0) {
		list_index = index;
		list_seen = 0;
		list_out = out;
		list_done = 0;
		r = fat_list_dir(fs, dir, dirent_cb);
		if (r == -2)
			r = SYS_EIO;
		else
			r = list_done ? 1 : 0;   /* 1 = entry returned, 0 = end */
	}
	fs_exit();
	return r;
}

int sys_list_mounts(unsigned int index, struct sys_mount *out)
{
	if (index >= vfs_mount_count)
		return 0;
	copy_bounded(out->name, vfs_mounts[index].name, sizeof(out->name));
	copy_bounded(out->type, fat_type_name(vfs_mounts[index].fs.type),
		     sizeof(out->type));
	out->total_sectors = vfs_mounts[index].fs.total_sectors;
	copy_bounded(out->label, vfs_mounts[index].fs.label, sizeof(out->label));
	return 1;
}
