#ifndef _AETHER_MATH_H
#define _AETHER_MATH_H

#include <float.h>

/* Constants */
#ifndef HUGE_VAL
#define HUGE_VAL    (__builtin_huge_val())
#endif
#ifndef HUGE_VALF
#define HUGE_VALF   (__builtin_huge_valf())
#endif
#define INFINITY    (__builtin_inff())
#define NAN         (__builtin_nanf(""))

#define M_PI        3.14159265358979323846
#define M_E         2.71828182845904523536
#define M_LN2       0.69314718055994530942
#define M_LOG2E     1.44269504088896340736
#define M_SQRT2     1.41421356237309504880

/*
 * Function declarations MUST come before the macros below, because the
 * macros would otherwise expand the function name in the declaration itself.
 */
double sqrt(double x);
double floor(double x);
double ceil(double x);
double fabs(double x);
double round(double x);
double trunc(double x);
double pow(double x, double y);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double fmod(double x, double y);
double modf(double x, double *iptr);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
double hypot(double x, double y);
double cbrt(double x);
double copysign(double x, double y);

/* Classification macros (always intrinsics — no call) */
#define isnan(x)    __builtin_isnan(x)
#define isinf(x)    __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)

/*
 * Hardware-accelerated ops: expand to a single AArch64 instruction.
 * The function declarations above remain valid for address-of usage.
 */
#define sqrt(x)       __builtin_sqrt(x)
#define sqrtf(x)      __builtin_sqrtf(x)
#define floor(x)      __builtin_floor(x)
#define floorf(x)     __builtin_floorf(x)
#define ceil(x)       __builtin_ceil(x)
#define ceilf(x)      __builtin_ceilf(x)
#define round(x)      __builtin_round(x)
#define trunc(x)      __builtin_trunc(x)
#define fabs(x)       __builtin_fabs(x)
#define fabsf(x)      __builtin_fabsf(x)
#define copysign(x,y) __builtin_copysign(x,y)

#endif /* _AETHER_MATH_H */
