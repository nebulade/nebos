/* Ring-3 FILE layer.
 *
 * Files are read-only and whole-file: fopen() stats the path, allocates one
 * buffer and reads it through SYS_READ_FILE.  stdout/stderr are console
 * streams.  This is enough for MuJS's js_dofile and default reporter.
 *
 * TODO(fd-layer): replace whole-file FILE with SYS_OPEN/READ/WRITE/LSEEK/
 * CLOSE and offset-capable fs_ops when a real fd layer lands.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ucmd_lib.h>

#define FILE_MAX (4u * 1024u * 1024u)

struct __libc_file {
	int kind;       /* 0 = empty/stdin, 1 = console, 2 = whole-file */
	int owned;
	int eof;
	int error;
	unsigned char *data;
	unsigned int size;
	unsigned int pos;
};

static struct __libc_file console_out = { 1, 0, 0, 0, 0, 0, 0 };
static struct __libc_file console_err = { 1, 0, 0, 0, 0, 0, 0 };
static struct __libc_file stdin_file = { 0, 0, 0, 0, 0, 0, 0 };

FILE *stdin = &stdin_file;
FILE *stdout = &console_out;
FILE *stderr = &console_err;

static int map_sys_error(int r)
{
	switch (r) {
	case SYS_ENOENT: return ENOENT;
	case SYS_ENODEV: return ENODEV;
	case SYS_ENOFS:  return ENODEV;
	case SYS_EINVAL: return EINVAL;
	case SYS_EROFS:  return EROFS;
	case SYS_ENOSPC: return ENOSPC;
	case SYS_EWRITE: return EIO;
	case SYS_EIO:    return EIO;
	default:         return EIO;
	}
}

FILE *fopen(const char *path, const char *mode)
{
	struct sys_stat st;
	struct __libc_file *f;
	int r;

	if (!path || !mode) {
		errno = EINVAL;
		return 0;
	}
	if (!(mode[0] == 'r' && (mode[1] == '\0' || mode[1] == 'b'))) {
		/* TODO(fd-layer): writable FILE objects need the kernel fd layer. */
		errno = EROFS;
		return 0;
	}

	r = u_syscall2(SYS_FS_STAT, (int)path, (int)&st);
	if (r != 0) {
		errno = map_sys_error(r);
		return 0;
	}
	if (st.is_dir) {
		errno = EISDIR;
		return 0;
	}
	if (st.size > FILE_MAX) {
		errno = EFBIG;
		return 0;
	}

	f = (struct __libc_file *)malloc(sizeof(*f));
	if (!f) {
		errno = ENOMEM;
		return 0;
	}
	memset(f, 0, sizeof(*f));
	f->kind = 2;
	f->owned = 1;
	f->data = (unsigned char *)malloc(st.size ? st.size : 1);
	if (!f->data) {
		free(f);
		errno = ENOMEM;
		return 0;
	}
	if (st.size) {
		r = u_syscall3(SYS_READ_FILE, (int)path, (int)f->data,
			       (int)st.size);
		if (r < 0) {
			free(f->data);
			free(f);
			errno = map_sys_error(r);
			return 0;
		}
		f->size = (unsigned int)r;
	}
	return f;
}

int fclose(FILE *stream)
{
	struct __libc_file *f = (struct __libc_file *)stream;

	if (!f)
		return EOF;
	if (f->owned) {
		free(f->data);
		free(f);
	}
	return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	struct __libc_file *f = (struct __libc_file *)stream;
	size_t total;
	size_t avail;
	size_t got;

	if (!f || !ptr || size == 0 || nmemb == 0)
		return 0;
	if (size && nmemb > (size_t)-1 / size) {
		errno = EOVERFLOW;
		return 0;
	}
	if (f->kind == 0) {
		f->eof = 1;
		return 0;
	}
	if (f->kind != 2) {
		errno = EBADF;
		return 0;
	}
	total = size * nmemb;
	avail = (size_t)f->size - f->pos;
	if (total > avail)
		total = avail;
	got = total / size;
	if (got) {
		memcpy(ptr, f->data + f->pos, got * size);
		f->pos += (unsigned int)(got * size);
	}
	if (got < nmemb)
		f->eof = 1;
	return got;
}

static int console_write(FILE *stream, const void *ptr, size_t len)
{
	const unsigned char *p = (const unsigned char *)ptr;
	size_t i;

	(void)stream;
	for (i = 0; i < len; i++)
		u_putc((char)p[i]);
	return 0;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	struct __libc_file *f = (struct __libc_file *)stream;

	if (!f || !ptr || size == 0 || nmemb == 0)
		return 0;
	if (size && nmemb > (size_t)-1 / size) {
		errno = EOVERFLOW;
		return 0;
	}
	if (f->kind != 1) {
		errno = EBADF;
		return 0;
	}
	console_write(stream, ptr, size * nmemb);
	return nmemb;
}

int fputc(int c, FILE *stream)
{
	unsigned char ch = (unsigned char)c;

	if (fwrite(&ch, 1, 1, stream) != 1)
		return EOF;
	return ch;
}

int fgetc(FILE *stream)
{
	struct __libc_file *f = (struct __libc_file *)stream;
	int c;

	if (!f || f->kind == 0) {
		if (f)
			f->eof = 1;
		return EOF;
	}
	if (f->kind != 2) {
		errno = EBADF;
		return EOF;
	}
	if (f->pos >= f->size) {
		f->eof = 1;
		return EOF;
	}
	c = f->data[f->pos++];
	return c;
}

int getchar(void)
{
	return fgetc(stdin);
}

char *fgets(char *s, int size, FILE *stream)
{
	int n = 0;

	if (!s || size <= 0)
		return 0;
	while (n + 1 < size) {
		int c = fgetc(stream);
		if (c == EOF) {
			if (n == 0)
				return 0;
			break;
		}
		s[n++] = (char)c;
		if (c == '\n')
			break;
	}
	s[n] = '\0';
	return s;
}

int fputs(const char *s, FILE *stream)
{
	size_t n;

	if (!s)
		return EOF;
	n = strlen(s);
	if (fwrite(s, 1, n, stream) != n)
		return EOF;
	return 0;
}

int fseek(FILE *stream, long offset, int whence)
{
	struct __libc_file *f = (struct __libc_file *)stream;
	long base;
	long target;

	if (!f || f->kind != 2) {
		errno = ESPIPE;
		return -1;
	}
	if (whence == SEEK_SET)
		base = 0;
	else if (whence == SEEK_CUR)
		base = (long)f->pos;
	else if (whence == SEEK_END)
		base = (long)f->size;
	else {
		errno = EINVAL;
		return -1;
	}
	if (offset > LONG_MAX - base || offset < LONG_MIN + base) {
		errno = EOVERFLOW;
		return -1;
	}
	target = base + offset;
	if (target < 0 || (unsigned long)target > f->size) {
		errno = EINVAL;
		return -1;
	}
	f->pos = (unsigned int)target;
	f->eof = 0;
	return 0;
}

long ftell(FILE *stream)
{
	struct __libc_file *f = (struct __libc_file *)stream;

	if (!f || f->kind != 2) {
		errno = ESPIPE;
		return -1;
	}
	return (long)f->pos;
}

void rewind(FILE *stream)
{
	(void)fseek(stream, 0, SEEK_SET);
	clearerr(stream);
}

int fflush(FILE *stream)
{
	(void)stream;
	return 0;
}

int feof(FILE *stream)
{
	struct __libc_file *f = (struct __libc_file *)stream;

	return f ? f->eof : 0;
}

int ferror(FILE *stream)
{
	struct __libc_file *f = (struct __libc_file *)stream;

	return f ? f->error : 0;
}

void clearerr(FILE *stream)
{
	struct __libc_file *f = (struct __libc_file *)stream;

	if (f) {
		f->eof = 0;
		f->error = 0;
	}
}

int fileno(FILE *stream)
{
	(void)stream;
	errno = EBADF;
	return -1;
}
