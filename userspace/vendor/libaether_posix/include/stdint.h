#ifndef _POSIX_STDINT_H
#define _POSIX_STDINT_H

/* AArch64 LP64: char=8, short=16, int=32, long=64, long long=64 */
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long        int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;

typedef signed long        intptr_t;
typedef unsigned long      uintptr_t;
typedef signed long long   intmax_t;
typedef unsigned long long uintmax_t;

typedef int8_t   int_least8_t;
typedef int16_t  int_least16_t;
typedef int32_t  int_least32_t;
typedef int64_t  int_least64_t;
typedef uint8_t  uint_least8_t;
typedef uint16_t uint_least16_t;
typedef uint32_t uint_least32_t;
typedef uint64_t uint_least64_t;

typedef int32_t  int_fast8_t;
typedef int32_t  int_fast16_t;
typedef int32_t  int_fast32_t;
typedef int64_t  int_fast64_t;
typedef uint32_t uint_fast8_t;
typedef uint32_t uint_fast16_t;
typedef uint32_t uint_fast32_t;
typedef uint64_t uint_fast64_t;

#define INT8_MIN    (-128)
#define INT16_MIN   (-32768)
#define INT32_MIN   (-2147483648)
#define INT64_MIN   (-9223372036854775807LL - 1)
#define INT8_MAX    127
#define INT16_MAX   32767
#define INT32_MAX   2147483647
#define INT64_MAX   9223372036854775807LL
#define UINT8_MAX   255U
#define UINT16_MAX  65535U
#define UINT32_MAX  4294967295U
#define UINT64_MAX  18446744073709551615ULL

#define INTPTR_MIN  INT64_MIN
#define INTPTR_MAX  INT64_MAX
#define UINTPTR_MAX UINT64_MAX
#define INTMAX_MIN  INT64_MIN
#define INTMAX_MAX  INT64_MAX
#define UINTMAX_MAX UINT64_MAX

#define SIZE_MAX    UINT64_MAX
#define PTRDIFF_MAX INT64_MAX
#define PTRDIFF_MIN INT64_MIN

#define INT8_C(v)   (v)
#define INT16_C(v)  (v)
#define INT32_C(v)  (v)
#define INT64_C(v)  (v ## LL)
#define UINT8_C(v)  (v ## U)
#define UINT16_C(v) (v ## U)
#define UINT32_C(v) (v ## U)
#define UINT64_C(v) (v ## ULL)
#define INTMAX_C(v)  (v ## LL)
#define UINTMAX_C(v) (v ## ULL)

#endif /* _POSIX_STDINT_H */
