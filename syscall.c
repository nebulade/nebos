/* Syscall dispatcher: services int 0x80 requests from ring-3. */

#include "syscall.h"
#include "io.h"
#include "process.h"
#include "serial.h"
#include "sys_fs.h"
#include "timer.h"

int syscall_dispatch(int num, int a1, int a2, int a3)
{
	switch (num) {
	case SYS_PUTSTR:
		io_putstr((const char *)a1);
		return 0;
	case SYS_GET_TICKS_MS:
		return (int)get_ticks_ms();
	case SYS_SLEEP_MS:
		process_sleep_ms((unsigned int)a1);
		return 0;
	case SYS_EXIT:
		process_exit();   /* never returns */
		return 0;
	case SYS_READ_FILE:
		return sys_read_file((const char *)a1, (void *)a2, (unsigned int)a3);
	case SYS_WRITE_FILE:
		return sys_write_file((const char *)a1, (const void *)a2,
				      (unsigned int)a3);
	case SYS_LIST_DIR:
		return sys_list_dir((const char *)a1, (unsigned int)a2,
				    (struct sys_dirent *)a3);
	case SYS_UNLINK:
		return sys_unlink((const char *)a1);
	case SYS_MKDIR:
		return sys_mkdir((const char *)a1);
	case SYS_RMDIR:
		return sys_rmdir((const char *)a1);
	case SYS_LIST_MOUNTS:
		return sys_list_mounts((unsigned int)a1, (struct sys_mount *)a2);
	default:
		return -1;
	}
}
