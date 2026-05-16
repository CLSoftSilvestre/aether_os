#ifndef _POSIX_INTTYPES_H
#define _POSIX_INTTYPES_H

#include <stdint.h>

/* AArch64: int=32-bit, long=64-bit, long long=64-bit */
#define PRId8    "d"
#define PRId16   "d"
#define PRId32   "d"
#define PRId64   "ld"
#define PRIi8    "i"
#define PRIi16   "i"
#define PRIi32   "i"
#define PRIi64   "li"
#define PRIu8    "u"
#define PRIu16   "u"
#define PRIu32   "u"
#define PRIu64   "lu"
#define PRIx8    "x"
#define PRIx16   "x"
#define PRIx32   "x"
#define PRIx64   "lx"
#define PRIX8    "X"
#define PRIX16   "X"
#define PRIX32   "X"
#define PRIX64   "lX"
#define PRIdMAX  "ld"
#define PRIuMAX  "lu"
#define PRIxMAX  "lx"
#define PRIdPTR  "ld"
#define PRIuPTR  "lu"
#define PRIxPTR  "lx"

/* scanf format specifiers */
#define SCNd8    "hhd"
#define SCNd16   "hd"
#define SCNd32   "d"
#define SCNd64   "ld"
#define SCNu8    "hhu"
#define SCNu16   "hu"
#define SCNu32   "u"
#define SCNu64   "lu"
#define SCNx8    "hhx"
#define SCNx16   "hx"
#define SCNx32   "x"
#define SCNx64   "lx"
#define SCNdMAX  "ld"
#define SCNuMAX  "lu"
#define SCNxMAX  "lx"

typedef long long intmax_t;
typedef unsigned long long uintmax_t;

intmax_t  strtoimax(const char *s, char **endp, int base);
uintmax_t strtoumax(const char *s, char **endp, int base);

#endif /* _POSIX_INTTYPES_H */
