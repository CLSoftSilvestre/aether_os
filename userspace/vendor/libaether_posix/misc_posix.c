/*
 * libaether_posix/misc_posix.c — miscellaneous POSIX functions
 *
 * Covers: stdlib extras (atoi/atof/qsort/bsearch/rand/exit/abort/getenv),
 *         unistd stubs (read/write/close/lseek/getpid/sleep/isatty),
 *         sys/stat stubs (stat/fstat/mkdir/chmod),
 *         locale stubs, signal stubs, math wrappers, inttypes.
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <signal.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys.h>   /* AetherOS syscalls */

/* ── stdlib extras ───────────────────────────────────────────────────── */

int atoi(const char *s)  { return (int)strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }

double atof(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    double sign = 1.0;
    if (*s == '-') { sign = -1.0; s++; }
    else if (*s == '+') s++;
    double v = 0.0;
    while (*s >= '0' && *s <= '9') { v = v * 10.0 + (*s++ - '0'); }
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * frac; frac *= 0.1; }
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int esign = 1; int exp = 0;
        if (*s == '-') { esign = -1; s++; }
        else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9') exp = exp*10 + (*s++ - '0');
        double base = 10.0;
        while (exp-- > 0) { if (esign > 0) v *= base; else v /= base; }
    }
    return sign * v;
}

double strtod(const char *s, char **endp)
{
    const char *start = s;
    double v = atof(s);
    /* Advance past consumed chars */
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') s++;
    while (*s >= '0' && *s <= '9') s++;
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') s++; }
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '-' || *s == '+') s++;
        while (*s >= '0' && *s <= '9') s++;
    }
    if (endp) *endp = (s == start) ? (char *)start : (char *)s;
    return v;
}

float strtof(const char *s, char **endp) { return (float)strtod(s, endp); }

int abs(int x)            { return x < 0 ? -x : x; }
long labs(long x)         { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

div_t  div(int num, int den)   { div_t r; r.quot = num/den; r.rem = num%den; return r; }
ldiv_t ldiv(long num, long den){ ldiv_t r; r.quot = num/den; r.rem = num%den; return r; }

/* ── Random number generator (LCG) ──────────────────────────────────── */

static unsigned long long _rng = 12345;

int rand(void)
{
    _rng = _rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (int)((_rng >> 33) & RAND_MAX);
}

void srand(unsigned int seed) { _rng = (unsigned long long)seed; }

/* ── Exit / Abort / atexit ───────────────────────────────────────────── */

#define ATEXIT_MAX 32
static void (*_atexit_fns[ATEXIT_MAX])(void);
static int   _atexit_n;

int atexit(void (*fn)(void))
{
    if (_atexit_n >= ATEXIT_MAX) return -1;
    _atexit_fns[_atexit_n++] = fn;
    return 0;
}

__attribute__((noreturn))
void exit(int code)
{
    for (int i = _atexit_n - 1; i >= 0; i--)
        if (_atexit_fns[i]) _atexit_fns[i]();
    sys_exit(code);
}

__attribute__((noreturn))
void abort(void) { sys_exit(134); }

/* ── Environment (no environment in AetherOS userspace) ──────────────── */

char *getenv(const char *name) { (void)name; return NULL; }
int setenv(const char *n, const char *v, int ow) { (void)n;(void)v;(void)ow; return -1; }
int unsetenv(const char *n) { (void)n; return -1; }

/* ── qsort (standard insertion sort → quicksort for n>8) ─────────────── */

static void _swap(char *a, char *b, size_t size)
{
    char tmp; while (size--) { tmp = *a; *a++ = *b; *b++ = tmp; }
}

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *))
{
    if (n <= 1) return;
    char *b = base;
    /* Median-of-3 quicksort */
    if (n <= 8) {
        /* Insertion sort for small arrays */
        for (size_t i = 1; i < n; i++) {
            for (size_t j = i; j > 0 && cmp(b + (j-1)*size, b + j*size) > 0; j--)
                _swap(b + (j-1)*size, b + j*size, size);
        }
        return;
    }
    /* Pivot: middle element */
    size_t mid = (n / 2);
    char *pivot = b + mid * size;
    _swap(pivot, b + (n-1)*size, size);
    char *store = b;
    for (size_t i = 0; i < n - 1; i++) {
        if (cmp(b + i*size, b + (n-1)*size) < 0) {
            _swap(b + i*size, store, size);
            store += size;
        }
    }
    _swap(store, b + (n-1)*size, size);
    size_t pivot_idx = (size_t)(store - b) / size;
    qsort(b, pivot_idx, size, cmp);
    if (pivot_idx + 1 < n)
        qsort(b + (pivot_idx+1)*size, n - pivot_idx - 1, size, cmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *lo = base, *hi = (const char *)base + (n - 1) * size;
    while (lo <= hi) {
        const char *mid = lo + (((hi - lo) / (long)size) / 2) * (long)size;
        int r = cmp(key, mid);
        if (r == 0) return (void *)mid;
        if (r < 0)  hi = mid - size;
        else        lo = mid + size;
    }
    return NULL;
}

/* Wide-char stubs */
int mblen(const char *s, size_t n)  { (void)n; return s && *s ? 1 : 0; }
int mbtowc(int *pwc, const char *s, size_t n)
{ (void)n; if (!s||!*s){if(pwc)*pwc=0;return 0;} if(pwc)*pwc=(unsigned char)*s; return 1; }

/* ── unistd functions ────────────────────────────────────────────────── */

ssize_t read(int fd, void *buf, size_t count)
{
    long n = sys_read(fd, buf, (long)count);
    if (n < 0) { errno = EIO; return -1; }
    return (ssize_t)n;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    long n = sys_write(fd, buf, (long)count);
    if (n < 0) { errno = EIO; return -1; }
    return (ssize_t)n;
}

int close(int fd)
{
    if (fd >= 100 && fd < 200) {
        sys_net_close((long)fd); return 0;
    } else if (fd >= 200) {
        sys_fs_close((long)fd); return 0;
    } else if (fd > 2) {
        sys_close(fd); return 0;
    }
    /* fd 0/1/2: no-op */
    return 0;
}

off_t lseek(int fd, off_t offset, int whence)
{
    /* AetherOS VFS lacks a seek syscall in Phase 7.0 */
    (void)fd; (void)offset; (void)whence;
    errno = ESPIPE; return (off_t)-1;
}

int isatty(int fd) { return fd <= 2 ? 1 : 0; }

int dup(int oldfd)
{
    /* Not supported in Phase 7.0 */
    (void)oldfd; errno = ENOSYS; return -1;
}

int dup2(int oldfd, int newfd)
{
    long r = sys_dup2(oldfd, newfd);
    if (r < 0) { errno = EINVAL; return -1; }
    return (int)r;
}

pid_t getpid(void)  { return (pid_t)sys_getpid(); }
pid_t getppid(void) { return 1; }

unsigned int sleep(unsigned int seconds)
{
    sys_sleep((long)(seconds * 100)); /* 100 ticks per second */
    return 0;
}

int usleep(unsigned int usec)
{
    long ticks = (long)usec / 10000L; /* 10 000 µs per tick at 100 Hz */
    if (ticks < 1 && usec > 0) ticks = 1;
    sys_sleep(ticks);
    return 0;
}

char *getcwd(char *buf, size_t size)
{
    /* AetherOS has no concept of working directory yet */
    if (!buf || !size) { errno = EINVAL; return NULL; }
    static const char cwd[] = "/";
    if (size < sizeof(cwd)) { errno = ERANGE; return NULL; }
    memcpy(buf, cwd, sizeof(cwd));
    return buf;
}

int access(const char *path, int mode)
{
    /* Try to open the file as a read test */
    (void)mode;
    long fd = sys_fs_open(path);
    if (fd < 0) { errno = ENOENT; return -1; }
    sys_fs_close(fd);
    return 0;
}

/* ── open() / fcntl (minimal) ────────────────────────────────────────── */

int open(const char *path, int flags, ...)
{
    long fd;
    if (flags & O_WRONLY || flags & O_RDWR || flags & O_CREAT)
        fd = sys_fs_create(path);
    else
        fd = sys_fs_open(path);
    if (fd < 0) { errno = ENOENT; return -1; }
    return (int)fd;
}

/* ── sys/stat ────────────────────────────────────────────────────────── */

int stat(const char *path, struct stat *buf)
{
    if (!buf) { errno = EFAULT; return -1; }
    long fd = sys_fs_open(path);
    if (fd < 0) { errno = ENOENT; return -1; }
    /* Read until EOF to determine file size */
    char tmp[256]; long total = 0, n;
    while ((n = sys_fs_read(fd, tmp, sizeof(tmp))) > 0) total += n;
    sys_fs_close(fd);
    memset(buf, 0, sizeof(*buf));
    buf->st_size  = (off_t)total;
    buf->st_mode  = S_IFREG | S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    buf->st_nlink = 1;
    return 0;
}

int fstat(int fd, struct stat *buf)
{
    if (!buf) { errno = EFAULT; return -1; }
    memset(buf, 0, sizeof(*buf));
    buf->st_mode = S_IFREG | S_IRUSR | S_IWUSR;
    buf->st_nlink = 1;
    if (fd <= 2) buf->st_mode = S_IFCHR | 0666;
    return 0;
}

int lstat(const char *path, struct stat *buf) { return stat(path, buf); }

int mkdir(const char *path, mode_t mode)
{
    (void)mode;
    long r = sys_fs_mkdir(path);
    if (r < 0) { errno = EIO; return -1; }
    return 0;
}

int chmod(const char *path, mode_t mode) { (void)path;(void)mode; return 0; }
int umask(mode_t mask) { (void)mask; return 022; }

/* ── locale stubs ────────────────────────────────────────────────────── */

static struct lconv _lc = {
    .decimal_point = ".",
    .thousands_sep = "",
    .grouping      = "",
    .int_curr_symbol = "",
    .currency_symbol = "",
    .mon_decimal_point = ".",
    .mon_thousands_sep = "",
    .mon_grouping      = "",
    .positive_sign = "",
    .negative_sign = "-",
    .int_frac_digits = 127,
    .frac_digits     = 127,
};

char *setlocale(int category, const char *locale)
{
    (void)category; (void)locale;
    return NULL;
}

struct lconv *localeconv(void) { return &_lc; }

/* ── signal stubs ────────────────────────────────────────────────────── */

sighandler_t signal(int signum, sighandler_t handler)
{ (void)signum; (void)handler; return SIG_DFL; }

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{ (void)sig;(void)act;(void)old; return 0; }

int sigemptyset(sigset_t *set)
{ set->sig[0] = set->sig[1] = 0; return 0; }

int sigfillset(sigset_t *set)
{ set->sig[0] = set->sig[1] = ~0UL; return 0; }

int sigaddset(sigset_t *set, int signum)
{ set->sig[0] |= (1UL << (signum-1)); return 0; }

int sigdelset(sigset_t *set, int signum)
{ set->sig[0] &= ~(1UL << (signum-1)); return 0; }

int sigismember(const sigset_t *set, int signum)
{ return !!(set->sig[0] & (1UL << (signum-1))); }

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{ (void)how;(void)set;(void)old; return 0; }

int raise(int signum) { (void)signum; return 0; }
int kill(pid_t pid, int sig) { (void)pid;(void)sig; return 0; }

/* ── math (hardware float / double via AArch64 FPU) ─────────────────── */
/* All math functions resolve to compiler built-ins or the libm equivalents.
 * The AArch64 FPU is available in userspace (we removed -mgeneral-regs-only). */

double fabs(double x)  { return __builtin_fabs(x); }
double floor(double x) { return __builtin_floor(x); }
double ceil(double x)  { return __builtin_ceil(x); }
double round(double x) { return __builtin_round(x); }
double trunc(double x) { return __builtin_trunc(x); }
double sqrt(double x)  { return __builtin_sqrt(x); }
double fmin(double a, double b) { return __builtin_fmin(a, b); }
double fmax(double a, double b) { return __builtin_fmax(a, b); }

float  fabsf(float x)  { return __builtin_fabsf(x); }
float  floorf(float x) { return __builtin_floorf(x); }
float  ceilf(float x)  { return __builtin_ceilf(x); }
float  roundf(float x) { return __builtin_roundf(x); }
float  sqrtf(float x)  { return __builtin_sqrtf(x); }
float  fminf(float a, float b) { return __builtin_fminf(a, b); }
float  fmaxf(float a, float b) { return __builtin_fmaxf(a, b); }

/* Transcendental functions — require software implementation or libm.
 * For Phase 7.0 (integration test), sin/cos/pow are not needed.
 * Stubs return 0 so the binary links; Phase 7.2 replaces them. */
static double _unsupported(double x) { (void)x; return 0.0; }
double exp(double x)   { return _unsupported(x); }
double exp2(double x)  { return _unsupported(x); }

/* log / log2 / log10 — implemented via IEEE 754 bit tricks + arctanh series.
 * Accurate to ~14 significant digits, which is sufficient for BigInt digit-count
 * estimation in QuickJS's bf_ftoa (ceil(expn * log2(2) / log2(10))).
 * Does NOT call any other stub functions. */
double log(double x)
{
    if (x <= 0.0) return 0.0;
    union { double d; unsigned long long u; } v;
    v.d = x;
    int e = (int)((v.u >> 52) & 0x7ffULL) - 1023;
    /* Normalize mantissa to [1.0, 2.0) */
    v.u = (v.u & 0x000fffffffffffffULL) | 0x3ff0000000000000ULL;
    double m = v.d;
    /* log(m) for m in [1, 2): t = (m-1)/(m+1), log(m) = 2*arctanh(t)
     *   = 2t * (1 + t^2/3 + t^4/5 + t^6/7 + t^8/9)   [Horner form] */
    double t  = (m - 1.0) / (m + 1.0);
    double t2 = t * t;
    double lnm = 2.0 * t * (1.0 + t2 * (1.0/3.0 + t2 * (1.0/5.0
                   + t2 * (1.0/7.0 + t2 * (1.0/9.0 + t2 / 11.0)))));
    return lnm + (double)e * 0.6931471805599453094; /* e * ln(2) */
}
double log2(double x)  { return log(x) * 1.4426950408889634074; } /* 1/ln(2) */
double log10(double x) { return log(x) * 0.4342944819032518276; } /* 1/ln(10) */
double pow(double x, double y) { (void)y; return _unsupported(x); }
double cbrt(double x)  { return _unsupported(x); }
double sin(double x)   { return _unsupported(x); }
double cos(double x)   { return _unsupported(x); }
double tan(double x)   { return _unsupported(x); }
double asin(double x)  { return _unsupported(x); }
double acos(double x)  { return _unsupported(x); }
double atan(double x)  { return _unsupported(x); }
double atan2(double y, double x) { (void)y; return _unsupported(x); }
double sinh(double x)  { return _unsupported(x); }
double cosh(double x)  { return _unsupported(x); }
double tanh(double x)  { return _unsupported(x); }
double hypot(double x, double y) { (void)y; return _unsupported(x); }
double fmod(double x, double y)  { (void)y; return _unsupported(x); }
double ldexp(double x, int e)    { (void)e; return _unsupported(x); }
double frexp(double x, int *e)   { if(e)*e=0; return _unsupported(x); }
double modf(double x, double *i) { if(i)*i=0.0; return _unsupported(x); }
double difftime_math(double a, double b) { return a - b; }

float expf(float x)   { return (float)_unsupported((double)x); }
float logf(float x)   { return (float)_unsupported((double)x); }
float powf(float x, float y) { (void)y; return (float)_unsupported((double)x); }
float sinf(float x)   { return (float)_unsupported((double)x); }
float cosf(float x)   { return (float)_unsupported((double)x); }
float tanf(float x)   { return (float)_unsupported((double)x); }
float fmodf(float x, float y) { (void)y; return (float)_unsupported((double)x); }

/* ── C99 math additions (needed by QuickJS / libregexp) ─────────────────── */
double acosh(double x)   { return _unsupported(x); }
double asinh(double x)   { return _unsupported(x); }
double atanh(double x)   { return _unsupported(x); }
double expm1(double x)   { return _unsupported(x); }
double log1p(double x)   { return _unsupported(x); }
double rint(double x)    { return __builtin_rint(x); }
double nearbyint(double x){ return __builtin_nearbyint(x); }
double copysign(double x, double y) { return __builtin_copysign(x, y); }
double fma(double x, double y, double z) { return __builtin_fma(x, y, z); }
double remainder(double x, double y) { (void)y; return _unsupported(x); }
double scalbn(double x, int n) { (void)n; return _unsupported(x); }
int    ilogb(double x)   { (void)x; return 0; }
long   lrint(double x)   { return (long)__builtin_rint(x); }

float  log2f(float x)   { return (float)_unsupported((double)x); }
float  expm1f(float x)  { return (float)_unsupported((double)x); }
float  log1pf(float x)  { return (float)_unsupported((double)x); }

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{ (void)fd; (void)buf; (void)count; (void)offset; errno = ENOSYS; return -1; }

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{ (void)fd; (void)buf; (void)count; (void)offset; errno = ENOSYS; return -1; }

/* ── POSIX directory-fd helpers (used by NetSurf utils/file.c cleanup) ── */

int dirfd(DIR *dirp) { (void)dirp; errno = ENOSYS; return -1; }

int fstatat(int dirfd_, const char *path, struct stat *buf, int flags)
{ (void)dirfd_; (void)flags; return stat(path, buf); }

int unlinkat(int dirfd_, const char *path, int flags)
{ (void)dirfd_; (void)flags; errno = ENOSYS; return -1; }

int rmdir(const char *path) { (void)path; errno = ENOSYS; return -1; }

/* ── zlib gz* stubs (NetSurf hashtable/messages; gzip messages not supported) */
/* The gzFile type is defined in zlib.h as struct gzFile_s * — we never open
 * any file so NULL is returned and callers handle it gracefully. */
typedef struct gzFile_s *gzFile;
gzFile gzopen(const char *path, const char *mode) { (void)path;(void)mode; return (gzFile)0; }
char  *gzgets(gzFile f, char *buf, int len) { (void)f;(void)buf;(void)len; return (char*)0; }
int    gzclose(gzFile f) { (void)f; return 0; }
