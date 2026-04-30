/* AetherOS bare-metal math library — software implementations for Lua 5.4 */
#include <stdint.h>
#include <float.h>

/* ── hardware-backed ops provided as real functions too ──────────────────── */
/* These inline to a single instruction on AArch64 but must also exist as    */
/* callable symbols for translation units that don't see the macros.         */
double sqrt(double x)  { return __builtin_sqrt(x); }
double floor(double x) { return __builtin_floor(x); }
double ceil(double x)  { return __builtin_ceil(x); }
double fabs(double x)  { return __builtin_fabs(x); }
double round(double x) { return __builtin_round(x); }
double trunc(double x) { return __builtin_trunc(x); }

typedef union { double d; uint64_t u; } _du64;

/* ── ldexp ───────────────────────────────────────────────────────────────── */
double ldexp(double x, int n) {
    _du64 u; u.d = x;
    if (!x) return x;
    unsigned e = (u.u >> 52) & 0x7ff;
    if (e == 0x7ff) return x; /* inf/nan passthrough */
    if (e == 0) {             /* subnormal: normalise */
        u.d *= 0x1p52; e = (u.u >> 52) & 0x7ff; n -= 52;
    }
    int ne = (int)e + n;
    if (ne >= 0x7ff) return x > 0 ? __builtin_huge_val() : -__builtin_huge_val();
    if (ne <= 0) {
        if (ne < -52) return 0.0 * x;
        ne += 52;
        u.u = (u.u & ~(0x7ffULL << 52)) | ((uint64_t)ne << 52);
        return u.d * 0x1p-52;
    }
    u.u = (u.u & ~(0x7ffULL << 52)) | ((uint64_t)ne << 52);
    return u.d;
}

/* ── frexp ───────────────────────────────────────────────────────────────── */
double frexp(double x, int *ep) {
    _du64 u; u.d = x;
    int e = (u.u >> 52) & 0x7ff;
    if (e == 0x7ff) { *ep = 0; return x; }
    if (e == 0) {
        if (!x) { *ep = 0; return 0.0; }
        u.d *= 0x1p52; e = (u.u >> 52) & 0x7ff; *ep = e - 1022 - 52;
    } else {
        *ep = e - 1022;
    }
    u.u = (u.u & ~(0x7ffULL << 52)) | (0x3feULL << 52);
    return u.d;
}

/* ── modf ────────────────────────────────────────────────────────────────── */
double modf(double x, double *iptr) {
    *iptr = __builtin_trunc(x);
    if (__builtin_isinf(x)) return 0.0 * x;
    return x - *iptr;
}

/* ── fmod ────────────────────────────────────────────────────────────────── */
double fmod(double x, double y) {
    if (__builtin_isnan(x) || __builtin_isnan(y) || y == 0.0 || __builtin_isinf(x))
        return __builtin_nan("");
    if (__builtin_isinf(y) || x == 0.0) return x;
    return x - __builtin_trunc(x / y) * y;
}

/* ── exp ─────────────────────────────────────────────────────────────────── */
double exp(double x) {
    if (__builtin_isnan(x)) return x;
    if (x > 709.78) return __builtin_huge_val();
    if (x < -745.13) return 0.0;
    /* Cody-Waite range reduction: x = n*ln2 + r, |r| <= ln2/2 */
    static const double inv_ln2 = 1.4426950408889634;
    static const double ln2h    = 0.6931471803691238;
    static const double ln2l    = 1.9082149292705877e-10;
    int n = (int)(x * inv_ln2 + (x > 0 ? 0.5 : -0.5));
    double r = (x - n * ln2h) - n * ln2l;
    double r2 = r * r;
    double p = r2 * (0.5 + r * (1.0/6 + r * (1.0/24 + r * (1.0/120 + r * (1.0/720 + r/5040)))));
    return ldexp(1.0 + (r + p), n);
}

/* ── log ─────────────────────────────────────────────────────────────────── */
double log(double x) {
    if (__builtin_isnan(x)) return x;
    if (x < 0) return __builtin_nan("");
    if (x == 0.0) return -__builtin_huge_val();
    if (__builtin_isinf(x)) return x;
    int e; x = frexp(x, &e);
    /* Map x into [sqrt(2)/2, sqrt(2)] for better convergence */
    if (x < 0.7071067811865476) { x *= 2.0; e--; }
    /* log(x) = 2*atanh((x-1)/(x+1)), series expansion */
    double t = (x - 1.0) / (x + 1.0), t2 = t * t;
    double r = t * (2.0 + t2 * (2.0/3 + t2 * (2.0/5 + t2 * (2.0/7 + t2 * (2.0/9 + t2 * 2.0/11)))));
    return r + e * 0.6931471805599453;
}

double log2(double x)  { return log(x) * 1.4426950408889634; }
double log10(double x) { return log(x) * 0.4342944819032518; }

/* ── pow ─────────────────────────────────────────────────────────────────── */
double pow(double x, double y) {
    if (y == 0.0) return 1.0;
    if (x == 1.0) return 1.0;
    if (__builtin_isnan(x) || __builtin_isnan(y)) return __builtin_nan("");
    if (x == 0.0) return y > 0.0 ? 0.0 : __builtin_huge_val();
    if (__builtin_isinf(y)) {
        double ax = x < 0 ? -x : x;
        if (ax > 1.0) return y > 0 ? __builtin_huge_val() : 0.0;
        if (ax < 1.0) return y > 0 ? 0.0 : __builtin_huge_val();
        return __builtin_nan("");
    }
    if (x > 0.0) return exp(y * log(x));
    /* Negative base — only valid for integer exponents */
    long long iy = (long long)y;
    if ((double)iy != y) return __builtin_nan("");
    double r = exp(y * log(-x));
    return (iy & 1) ? -r : r;
}

/* ── sin / cos kernels ───────────────────────────────────────────────────── */
static const double _2PI = 6.283185307179586476;
static const double _PI  = 3.141592653589793238;
static const double _PI2 = 1.570796326794896619;

/* Taylor series for sin(x), x in [-pi/4, pi/4] — 11 terms, ~15-digit accuracy */
static double _sinK(double x) {
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6 + x2 * (1.0/120 + x2 * (-1.0/5040
           + x2 * (1.0/362880 + x2 * (-1.0/39916800 + x2/6227020800.0))))));
}
/* Taylor series for cos(x), x in [-pi/4, pi/4] */
static double _cosK(double x) {
    double x2 = x * x;
    return 1.0 + x2 * (-0.5 + x2 * (1.0/24 + x2 * (-1.0/720
           + x2 * (1.0/40320 + x2 * (-1.0/3628800 + x2/479001600.0)))));
}

double sin(double x) {
    if (!__builtin_isfinite(x)) return __builtin_nan("");
    if (x < -_2PI || x > _2PI) x = fmod(x, _2PI);
    if (x < 0) x += _2PI;
    int q = (int)(x / _PI2);
    double r = x - q * _PI2;
    switch (q & 3) {
    case 0: return  _sinK(r);
    case 1: return  _cosK(r);
    case 2: return -_sinK(r);
    default:return -_cosK(r);
    }
}

double cos(double x) {
    if (!__builtin_isfinite(x)) return __builtin_nan("");
    if (x < -_2PI || x > _2PI) x = fmod(x, _2PI);
    if (x < 0) x += _2PI;
    int q = (int)(x / _PI2);
    double r = x - q * _PI2;
    switch (q & 3) {
    case 0: return  _cosK(r);
    case 1: return -_sinK(r);
    case 2: return -_cosK(r);
    default:return  _sinK(r);
    }
}

double tan(double x) {
    double c = cos(x);
    return c != 0.0 ? sin(x) / c : __builtin_huge_val();
}

/* ── atan ────────────────────────────────────────────────────────────────── */
double atan(double x) {
    if (__builtin_isnan(x)) return x;
    if (__builtin_isinf(x)) return x > 0 ? _PI2 : -_PI2;
    int neg = x < 0; if (neg) x = -x;
    int inv = x > 1.0; if (inv) x = 1.0 / x;
    /* Horner form of atan series, adequate for x in [0,1] */
    double x2 = x * x;
    double p = x * (1.0 + x2 * (-1.0/3 + x2 * (1.0/5 + x2 * (-1.0/7
               + x2 * (1.0/9 + x2 * (-1.0/11 + x2 * (1.0/13 + x2 * (-1.0/15 + x2/17))))))));
    if (inv) p = _PI2 - p;
    return neg ? -p : p;
}

double atan2(double y, double x) {
    if (x == 0.0) {
        if (y == 0.0) return 0.0;
        return y > 0.0 ? _PI2 : -_PI2;
    }
    double t = atan(y / x);
    if (x < 0.0) t += (y >= 0.0) ? _PI : -_PI;
    return t;
}

double asin(double x) {
    if (__builtin_isnan(x) || x < -1.0 || x > 1.0) return __builtin_nan("");
    return atan2(x, __builtin_sqrt(1.0 - x * x));
}

double acos(double x) {
    if (__builtin_isnan(x) || x < -1.0 || x > 1.0) return __builtin_nan("");
    return atan2(__builtin_sqrt(1.0 - x * x), x);
}

/* ── hyperbolic ──────────────────────────────────────────────────────────── */
double sinh(double x) { double e = exp(x); return (e - 1.0 / e) * 0.5; }
double cosh(double x) { double e = exp(x); return (e + 1.0 / e) * 0.5; }
double tanh(double x) { double e = exp(2.0 * x); return (e - 1.0) / (e + 1.0); }

/* ── misc ────────────────────────────────────────────────────────────────── */
double hypot(double x, double y) { return __builtin_sqrt(x * x + y * y); }
double cbrt(double x) {
    if (x == 0.0) return 0.0;
    int neg = x < 0; if (neg) x = -x;
    double r = exp(log(x) / 3.0);
    return neg ? -r : r;
}
