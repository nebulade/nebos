/* Whole-file image loading for ring-3 commands (see img_file.h). */

#include "img_file.h"
#include "ucmd_lib.h"

int img_read_file(const char *path, void *buf, unsigned int cap)
{
	int n;

	if (!path || !buf || cap == 0)
		return IMG_ERR_IO;
	n = u_syscall3(SYS_READ_FILE, (int)path, (int)buf, (int)cap);
	if (n < 0)
		return IMG_ERR_IO;
	if (n == 0)
		return IMG_ERR_EMPTY;
	if ((unsigned int)n >= cap)
		return IMG_ERR_TOOBIG;    /* filled the buffer: file is larger */
	return n;
}

int img_decode_file(const char *path, void *filebuf, unsigned int filecap,
		    const struct img_options *opt, struct img_mem *mem,
		    struct img_image *out)
{
	int n = img_read_file(path, filebuf, filecap);

	if (n < 0)
		return n;
	return img_decode(filebuf, (unsigned int)n, opt, mem, out);
}
