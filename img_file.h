/* Ring-3 convenience: read a whole file through SYS_READ_FILE and decode it.
 * The codec core stays OS-independent; this is the only image module that
 * includes ucmd_lib.h. */
#ifndef IMG_FILE_H
#define IMG_FILE_H

#include "img.h"

int img_read_file(const char *path, void *buf, unsigned int cap);
int img_decode_file(const char *path, void *filebuf, unsigned int filecap,
		    const struct img_options *opt, struct img_mem *mem,
		    struct img_image *out);

#endif
