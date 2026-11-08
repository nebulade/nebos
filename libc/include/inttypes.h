/* Minimal inttypes.h for ring-3 commands. */
#ifndef LIBC_INTTYPES_H
#define LIBC_INTTYPES_H

#include <stdint.h>

#define PRId8  "d"
#define PRIi8  "i"
#define PRIu8  "u"
#define PRIx8  "x"
#define PRIX8  "X"
#define PRId16 "d"
#define PRIi16 "i"
#define PRIu16 "u"
#define PRIx16 "x"
#define PRIX16 "X"
#define PRId32 "d"
#define PRIi32 "i"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"
#define PRIdPTR "d"
#define PRIuPTR "u"
#define PRIxPTR "x"

intmax_t imaxabs(intmax_t value);
intmax_t strtoimax(const char *str, char **endptr, int base);
uintmax_t strtoumax(const char *str, char **endptr, int base);

#endif
