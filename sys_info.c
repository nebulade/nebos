/* Kernel-state info for the info commands (meminfo/mouseinfo/storage).
 * Small, read-only snapshots; storage additionally refreshes the mount cache,
 * which reads disks and therefore runs under the fs-busy gate (see sys_fs.c). */

#include "syscall.h"
#include "ahci.h"
#include "blkdev.h"
#include "it87.h"
#include "mm.h"
#include "mouse.h"
#include "sched.h"
#include "sys_fs.h"
#include "thermal.h"
#include "usb.h"
#include "usbhid.h"
#include "vfs.h"

static void copy_str(char *dst, const char *src, unsigned int size)
{
	unsigned int i = 0;

	while (i + 1 < size && src[i]) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

int sys_meminfo(struct sys_meminfo *out)
{
	out->total_kb = mm_total_ram_kb();
	out->free_pages = mm_free_pages();
	out->page_size = PAGE_SIZE;
	out->e820_count = mm_e820_count();
	return 0;
}

int sys_e820(unsigned int index, struct sys_e820 *out)
{
	const struct e820_entry *e = mm_e820_entry(index);

	if (!e)
		return 0;
	out->base = (unsigned int)e->base;
	out->length = (unsigned int)e->length;
	out->type = e->type;
	return 1;
}

int sys_mouseinfo(struct sys_mouseinfo *out)
{
	unsigned int i;

	for (i = 0; i < sizeof(*out); i++)
		((unsigned char *)out)[i] = 0;

	out->present = mouse_is_present() ? 1 : 0;
	out->cursor_visible = mouse_cursor_visible() ? 1 : 0;
	copy_str(out->backend, mouse_cursor_backend_name(), sizeof(out->backend));
	if (!out->present)
		return 0;

	out->ps2 = mouse_ps2_present() ? 1 : 0;
	if (out->ps2) {
		out->ps2_id = mouse_device_id() & 0xFFu;
		out->ps2_packet_size = mouse_packet_size();
		out->ps2_streaming = mouse_streaming_enabled() ? 1 : 0;
	}
	out->usb = usbhid_mouse_attached() ? 1 : 0;
	if (out->usb) {
		unsigned int ep, mps;
		int low;

		usbhid_get_info(&out->usb_ctrl, &out->usb_addr, &ep, &mps, &low);
		out->usb_ep = ep;
		out->usb_mps = mps;
		out->usb_low = (unsigned int)low;
		copy_str(out->hcd, usb_hcd_name(), sizeof(out->hcd));
	}
	out->usb_keyboard = usbhid_keyboard_attached() ? 1 : 0;
	return 1;
}

int sys_storage_info(struct sys_storage_info *out)
{
	sys_fs_enter();                 /* vfs_mount_all() reads disks */
	vfs_mount_all();
	sys_fs_exit();

	out->ahci_present = ahci_controller_present() ? 1 : 0;
	out->blkdev_count = blk_dev_count;
	return 0;
}

int sys_blkdev(unsigned int index, struct sys_blkdev *out)
{
	const blk_dev_t *d;

	if (index >= blk_dev_count)
		return 0;
	d = &blk_devs[index];
	copy_str(out->name, d->name, sizeof(out->name));
	copy_str(out->model, d->model, sizeof(out->model));
	out->kind = (unsigned int)d->kind;
	out->present = d->present ? 1 : 0;
	out->total_sectors = d->total_sectors;
	return 1;
}

int sys_cputemp(struct sys_cputemp *out)
{
	unsigned int i;

	_Static_assert(MAX_CPUS <= SYS_CPU_MAX,
		       "sys_cputemp.cpu_temp_c is too small");
	if (!out)
		return -1;
	out->temp_c = thermal_max_c();
	out->tjmax_c = thermal_tjmax();
	out->delta_c = -1;
	out->status = 0;
	if (thermal_status_this_cpu(&out->status) == 0)
		out->delta_c = (int)((out->status >> 16) & 0x7F);
	out->ncpus = cpu_count;
	for (i = 0; i < SYS_CPU_MAX; i++)
		out->cpu_temp_c[i] = cpus[i].temp_c;
	return 0;
}

int sys_sensors(struct sys_sensors *out)
{
	struct it87_info info;
	unsigned int i;

	if (!out)
		return -1;
	it87_read(&info);
	out->present = info.present ? 1u : 0u;
	out->chip_id = info.chip_id;
	out->base = info.base;
	out->temp_enable = info.temp_enable;
	out->temp_extra = info.temp_extra;
	for (i = 0; i < 3; i++) {
		out->temp_c[i] = info.temp_c[i];
		out->fan_rpm[i] = info.fan_rpm[i];
	}
	return 0;
}
