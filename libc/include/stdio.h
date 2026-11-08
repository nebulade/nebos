/* Minimal stdio for ring-3 commands.
 *
 * The file layer is read-only and whole-file for now; see libc/stdio_file.c
 * for the fd-layer TODO.  stdout/stderr are console streams.
 */
#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct __libc_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

int printf(const char *format, ...);
int vprintf(const char *format, va_list ap);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list ap);
int sprintf(char *buf, const char *format, ...);
int snprintf(char *buf, size_t size, const char *format, ...);
int vsprintf(char *buf, const char *format, va_list ap);
int vsnprintf(char *buf, size_t size, const char *format, va_list ap);

int puts(const char *s);
int fputs(const char *s, FILE *stream);
int putchar(int c);
int fputc(int c, FILE *stream);
int fgetc(FILE *stream);
int getchar(void);
char *fgets(char *s, int size, FILE *stream);

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int fflush(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fileno(FILE *stream);

void perror(const char *s);

#endif
