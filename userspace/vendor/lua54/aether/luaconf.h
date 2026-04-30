/*
 * AetherOS — Lua 5.4 platform configuration
 * File: userspace/vendor/lua54/aether/luaconf.h
 *
 * Complete replacement for Lua's standard luaconf.h for bare-metal AArch64.
 * Includes both Lua's numeric type system (hardcoded for double + long) and
 * all AetherOS-specific stubs (FILE, locale, time, errno, signal, etc.).
 */

#ifndef LUACONF_AETHER_H
#define LUACONF_AETHER_H

#include <stddef.h>     /* size_t, ptrdiff_t, NULL */
#include <stdint.h>     /* uint64_t, int64_t */
#include <limits.h>     /* LONG_MAX, LONG_MIN, ULONG_MAX */
#include <float.h>      /* DBL_MANT_DIG, DBL_MAX etc. */
#include <stdarg.h>     /* va_list */
#include <string.h>     /* memcpy, strlen, etc. */
#include <setjmp.h>     /* jmp_buf, setjmp, longjmp */

/* ── Lua API / internal visibility ──────────────────────────────────────── */
#define LUA_API         extern
#define LUALIB_API      extern
#define LUAMOD_API      extern

#define LUAI_FUNC       extern
#define LUAI_DDEF       /* empty */
#define LUAI_DDEC(dec)  extern dec

/* ── Compiler helpers ────────────────────────────────────────────────────── */
/* l_sinline is intentionally left to llimits.h (it defines it as static inline) */
#define l_noret         __attribute__((noreturn)) void
#define l_likely(x)     __builtin_expect(!!(x), 1)
#define l_unlikely(x)   __builtin_expect(!!(x), 0)

/* AArch64: int is 32-bit but the OS is 64-bit */
#define LUAI_IS32INT    0

/* ── Lua float type: double ──────────────────────────────────────────────── */
#define LUA_NUMBER              double
#define LUAI_UACNUMBER          double
#define LUA_NUMBER_FRMLEN       ""
#define LUA_NUMBER_FMT          "%.14g"
#define l_floatatt(x)           (DBL_##x)
#define l_mathop(op)            op

/* String ↔ number conversion */
static inline double strtod(const char *s, char **ep)
{
    double v = 0.0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10.0 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * frac; frac *= 0.1; }
    }
    if (*s == 'e' || *s == 'E') {
        s++; int eneg = 0;
        if (*s == '-') { eneg = 1; s++; } else if (*s == '+') s++;
        int exp = 0;
        while (*s >= '0' && *s <= '9') exp = exp * 10 + (*s++ - '0');
        double sc = 1.0;
        for (int i = 0; i < exp; i++) sc *= 10.0;
        if (eneg) v /= sc; else v *= sc;
    }
    if (ep) *ep = (char *)s;
    return neg ? -v : v;
}
#define lua_str2number(s,p)  strtod((s),(p))
#define lua_number2str(s,sz,n) snprintf((s),(sz),LUA_NUMBER_FMT,(n))

static inline float strtof(const char *s, char **ep)
{ return (float)strtod(s, ep); }

/* ── Lua integer type: long (64-bit on AArch64) ──────────────────────────── */
#define LUA_INTEGER             long
#define LUAI_UACINT             long
#define LUA_UNSIGNED            unsigned long
#define LUA_MAXINTEGER          LONG_MAX
#define LUA_MININTEGER          LONG_MIN
#define LUA_MAXUNSIGNED         ULONG_MAX
#define LUA_INTEGER_FRMLEN      "l"
#define LUA_INTEGER_FMT         "%" LUA_INTEGER_FRMLEN "d"
#define lua_integer2str(s,sz,n) snprintf((s),(sz),LUA_INTEGER_FMT,(long)(n))

static inline long strtol(const char *s, char **ep, int base)
{
    long v = 0; int neg = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1]=='x'||s[1]=='X'))
        { base = 16; s += 2; }
    else if (base == 0 && s[0] == '0') { base = 8; s++; }
    else if (base == 0) base = 10;
    while (1) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d; s++;
    }
    if (ep) *ep = (char *)s;
    return neg ? -v : v;
}
static inline unsigned long strtoul(const char *s, char **ep, int base)
{ return (unsigned long)strtol(s, ep, base); }
static inline long long strtoll(const char *s, char **ep, int base)
{ return (long long)strtol(s, ep, base); }
static inline unsigned long long strtoull(const char *s, char **ep, int base)
{ return (unsigned long long)strtol(s, ep, base); }

/* ── Continuation context ────────────────────────────────────────────────── */
#define LUA_KCONTEXT            ptrdiff_t

/* ── Misc Lua config ─────────────────────────────────────────────────────── */
#define LUA_IDSIZE              60
#define LUAL_BUFFERSIZE         4096   /* 0x80 * sizeof(void*) * sizeof(long) */
#define LUA_USE_LONGJMP         1   /* use C setjmp/longjmp for errors */
#define LUA_NOENV               1   /* no getenv() for path lookups */
#define LUAI_MAXSTACK           800
#define LUAI_MAXCSTACK          200
#define LUAI_MAXNUMBER2STR      44
#define LUA_EXTRASPACE          (sizeof(void *))

/* Alignment for lua_State header */
#define LUAI_MAXALIGN  volatile double u_; void *s_; long i_; long long li_

/* ── Path defaults ───────────────────────────────────────────────────────── */
#define LUA_PATH_DEFAULT    ""
#define LUA_CPATH_DEFAULT   ""
#define LUA_PATH_SEP        ";"
#define LUA_PATH_MARK       "?"
#define LUA_EXEC_DIR        "!"
#define LUA_LDIR            ""
#define LUA_CDIR            ""

/* ── Disable dynamic loading ─────────────────────────────────────────────── */
#undef LUA_USE_DLOPEN

/* Locale decimal point — always '.' */
#define lua_getlocaledecpoint() '.'

/* ── Minimal FILE* substitute ────────────────────────────────────────────── */

typedef struct { int fd; } LuaFile_t;
#define FILE LuaFile_t

extern LuaFile_t *_aether_stdout;
extern LuaFile_t *_aether_stderr;
#define stdout (_aether_stdout)
#define stderr (_aether_stderr)
#define stdin  ((LuaFile_t *)0)

extern int printf(const char *fmt, ...);
extern int snprintf(char *buf, size_t size, const char *fmt, ...);
extern int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

static inline size_t fwrite(const void *ptr, size_t size, size_t n, LuaFile_t *f)
{
    if (!f || !ptr || !n || !size) return 0;
    register long x0 asm("x0") = (long)f->fd;
    register long x1 asm("x1") = (long)ptr;
    register long x2 asm("x2") = (long)(size * n);
    register long x8 asm("x8") = 34; /* SYS_WRITE */
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return n;
}
static inline int fputs(const char *s, LuaFile_t *f)
{
    if (!s || !f) return -1;
    size_t len = 0; while (s[len]) len++;
    fwrite(s, 1, len, f); return (int)len;
}
static inline int fputc(int c, LuaFile_t *f)
{ char ch = (char)c; fwrite(&ch, 1, 1, f); return c; }

static inline int fprintf(LuaFile_t *f, const char *fmt, ...)
{
    char buf[256]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (n > 0) fwrite(buf, 1, (size_t)n, f);
    return n;
}
static inline int fflush(LuaFile_t *f) { (void)f; return 0; }
static inline LuaFile_t *fopen(const char *p, const char *m) { (void)p;(void)m; return NULL; }
static inline int fclose(LuaFile_t *f) { (void)f; return 0; }
static inline size_t fread(void *p, size_t s, size_t n, LuaFile_t *f) { (void)p;(void)s;(void)n;(void)f; return 0; }
static inline int feof(LuaFile_t *f) { (void)f; return 1; }
static inline long ftell(LuaFile_t *f) { (void)f; return -1L; }
static inline int fseek(LuaFile_t *f, long o, int w) { (void)f;(void)o;(void)w; return -1; }
static inline void rewind(LuaFile_t *f) { (void)f; }
static inline int ferror(LuaFile_t *f) { (void)f; return 1; }
static inline LuaFile_t *tmpfile(void) { return NULL; }
static inline int fileno(LuaFile_t *f) { return f ? f->fd : -1; }
static inline int setvbuf(LuaFile_t *f, char *b, int t, size_t s) { (void)f;(void)b;(void)t;(void)s; return 0; }
static inline void setbuf(LuaFile_t *f, char *b) { (void)f;(void)b; }
static inline int ungetc(int c, LuaFile_t *f) { (void)c;(void)f; return -1; }

/* ── locale stub ─────────────────────────────────────────────────────────── */
struct lconv { char *decimal_point; };
static inline struct lconv *localeconv(void)
{ static struct lconv lc = { (char *)"." }; return &lc; }

/* ── time stub ───────────────────────────────────────────────────────────── */
typedef long clock_t;
typedef long time_t;
#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 100L
#endif
static inline clock_t clock(void) {
    register long x8 asm("x8") = 603;
    register long x0 asm("x0") = 0;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return (clock_t)x0;
}
static inline double difftime(time_t t2, time_t t1) { return (double)(t2-t1); }
static inline time_t time(time_t *t) {
    time_t v = (time_t)(clock() / CLOCKS_PER_SEC);
    if (t) *t = v;
    return v;
}
struct tm { int tm_sec,tm_min,tm_hour,tm_mday,tm_mon,tm_year,tm_wday,tm_yday,tm_isdst; };
static inline struct tm *localtime(const time_t *t) { static struct tm s={0}; (void)t; return &s; }
static inline struct tm *gmtime(const time_t *t) { return localtime(t); }
static inline time_t mktime(struct tm *t) { (void)t; return 0; }
static inline size_t strftime(char *s, size_t m, const char *f, const struct tm *t)
{ (void)s;(void)m;(void)f;(void)t; if(m>0) s[0]='\0'; return 0; }

/* ── signal stub ─────────────────────────────────────────────────────────── */
typedef int sig_atomic_t;
#define SIGINT  2
#define SIGTERM 15
#define SIGABRT  6
#define SIG_DFL ((void(*)(int))0)
#define SIG_IGN ((void(*)(int))1)
#define SIG_ERR ((void(*)(int))-1)
#define signal(sig, handler) ((void)0)
#define raise(sig) ((void)0)

/* ── errno stub ──────────────────────────────────────────────────────────── */
extern int _aether_errno;
#define errno  _aether_errno
#define ENOMEM 12
#define EINVAL 22

/* ── process / env stubs ─────────────────────────────────────────────────── */
static inline int system(const char *cmd)    { (void)cmd; return -1; }
static inline char *getenv(const char *name) { (void)name; return NULL; }
static inline int remove(const char *path)   { (void)path; return -1; }
static inline int rename(const char *o, const char *n) { (void)o;(void)n; return -1; }

/* ── abort ───────────────────────────────────────────────────────────────── */
__attribute__((noreturn)) static inline void abort(void)
{
    register long x8 asm("x8") = 0;
    register long x0 asm("x0") = 1;
    asm volatile("svc #0" : : "r"(x8), "r"(x0) : "memory");
    for (;;) {}
}

/* ── sprintf / sscanf stubs ──────────────────────────────────────────────── */
#define sprintf(buf, ...) snprintf((buf), 256, __VA_ARGS__)

static inline int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int matched = 0;
    const char *f = fmt, *s = str;
    while (*f && *s) {
        if (*f == '%') {
            f++;
            while (*f==' '||*f=='0'||(*f>='1'&&*f<='9')) f++;
            int isl = 0;
            if (*f == 'l') { isl=1; f++; }
            if (*f == 'l') f++;
            switch (*f) {
            case 'd': case 'i': {
                long v=0; int neg=0;
                if (*s=='-'){neg=1;s++;} else if(*s=='+') s++;
                while (*s>='0'&&*s<='9') v=v*10+(*s++-'0');
                if (neg) v=-v;
                if (isl) *va_arg(ap,long*)=v; else *va_arg(ap,int*)=(int)v;
                matched++;
            } break;
            case 'u': {
                unsigned long v=0;
                while (*s>='0'&&*s<='9') v=v*10+(unsigned)(*s++-'0');
                if (isl)*va_arg(ap,unsigned long*)=v; else *va_arg(ap,unsigned int*)=(unsigned)v;
                matched++;
            } break;
            case 'f': case 'g': case 'e': {
                char tmp[64]; int j=0;
                if (*s=='-'||*s=='+') tmp[j++]=*s++;
                while ((*s>='0'&&*s<='9')||*s=='.'||*s=='e'||*s=='E'||*s=='+'||*s=='-')
                    { if(j<63) tmp[j++]=*s; s++; }
                tmp[j]='\0';
                *va_arg(ap,double*)=strtod(tmp,NULL);
                matched++;
            } break;
            default: break;
            }
            f++;
        } else { if(*f!=*s) break; f++; s++; }
    }
    va_end(ap); return matched;
}

/* ── Lua output macros ───────────────────────────────────────────────────── */
#define lua_writestring(s,l)       fwrite((s), sizeof(char), (l), stdout)
#define lua_writeline()            (lua_writestring("\n", 1), fflush(stdout))
#define lua_writestringerror(s,p)  (fprintf(stderr, (s), (p)), fflush(stderr))

/* ── Disable API checks ──────────────────────────────────────────────────── */
#define LUA_USE_APICHECK 0

/* ── Likely/unlikely — must be visible to all Lua translation units ──────── */
#define luai_likely(x)   l_likely(x)
#define luai_unlikely(x) l_unlikely(x)

/* ── Floor helpers used by llimits.h ─────────────────────────────────────── */
#define luai_floor(x)   __builtin_floor(x)
#define l_floor(x)      (luai_floor(x))

/* ── l_sprintf — used by lobject.c and others ────────────────────────────── */
#define l_sprintf(s,sz,f,...) snprintf((s),(sz),(f),__VA_ARGS__)

/* ── lua_numbertointeger — check float fits in integer range ─────────────── */
#define lua_numbertointeger(n,p) \
    ((n) >= (LUA_NUMBER)(LUA_MININTEGER) && \
     (n) < -(LUA_NUMBER)(LUA_MININTEGER) && \
     (*(p) = (LUA_INTEGER)(n), 1))

/* ── lua_pointer2str — format pointer to buffer, return length ───────────── */
#define lua_pointer2str(buf,sz,p)  snprintf((buf),(sz),"%p",(p))

/* ── strerror / getc / freopen stubs for lauxlib.c ──────────────────────── */
static inline char *strerror(int e) { (void)e; return (char *)"error"; }
static inline int getc(LuaFile_t *f) { (void)f; return -1; /* EOF */ }
static inline LuaFile_t *freopen(const char *p, const char *m, LuaFile_t *f)
    { (void)p; (void)m; (void)f; return NULL; }

#endif /* LUACONF_AETHER_H */
