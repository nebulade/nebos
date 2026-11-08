/* VFS face loading for ring-3 commands.  The host tests use
 * ufont_face_open_mem() and do not link this file. */
#include "ufont.h"
#include "ucmd_lib.h"
#include <stdlib.h>
#include <string.h>

struct ufont_face *ufont_face_open_path(const char *path)
{
	struct sys_stat st;
	unsigned char *buf;
	struct ufont_face *f;
	int n;

	if (!path || u_stat(path, &st) != 0 || st.is_dir || st.size == 0 ||
	    st.size > UFONT_MAX_FILE - 512u)
		return 0;
	/* FAT reads may write a final partial sector past the requested
	 * length; keep the slack inside the buffer. */
	buf = (unsigned char *)malloc(st.size + 512u);
	if (!buf)
		return 0;
	n = u_syscall3(SYS_READ_FILE, (int)path, (int)buf, (int)st.size);
	if (n != (int)st.size) {
		free(buf);
		return 0;
	}
	f = ufont_face_open_mem(buf, st.size);
	free(buf);
	return f;
}
