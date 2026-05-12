#ifndef _POSIX_FENV_H
#define _POSIX_FENV_H

/* AetherOS floating-point environment stub.
 * Rounding-mode changes are no-ops on bare-metal — the hardware default
 * (FE_TONEAREST) is always in effect.  QuickJS only uses fesetround for
 * double→string rounding refinements; the stubs are safe for all Phase 7 use. */

#define FE_TONEAREST  0
#define FE_DOWNWARD   1
#define FE_UPWARD     2
#define FE_TOWARDZERO 3

#define FE_INVALID    (1 << 0)
#define FE_DIVBYZERO  (1 << 1)
#define FE_OVERFLOW   (1 << 2)
#define FE_UNDERFLOW  (1 << 3)
#define FE_INEXACT    (1 << 4)
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT)

typedef int fenv_t;
typedef int fexcept_t;

static inline int fesetround(int r)           { (void)r; return 0; }
static inline int fegetround(void)            { return FE_TONEAREST; }
static inline int feclearexcept(int e)        { (void)e; return 0; }
static inline int fetestexcept(int e)         { (void)e; return 0; }
static inline int feraiseexcept(int e)        { (void)e; return 0; }
static inline int fegetenv(fenv_t *e)         { if (e) *e = 0; return 0; }
static inline int fesetenv(const fenv_t *e)   { (void)e; return 0; }
static inline int feholdexcept(fenv_t *e)     { if (e) *e = 0; return 0; }
static inline int feupdateenv(const fenv_t *e){ (void)e; return 0; }

#endif /* _POSIX_FENV_H */
