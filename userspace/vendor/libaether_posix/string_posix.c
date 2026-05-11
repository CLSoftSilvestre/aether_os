/*
 * libaether_posix/string_posix.c
 *
 * Implements the string functions not present in libaether/string.c:
 *   strdup, strndup, strerror, strnlen, stpcpy, strcasecmp, strncasecmp,
 *   strcasestr, strtok_r, strxfrm, strtol, strtoul, strtoll, strtoull,
 *   strtoimax, strtoumax.
 *
 * The base functions (memcpy, memset, strlen, strcmp, strchr, …) are
 * re-implemented here so that libaether_posix.a is fully self-contained
 * and external libs do not need to also link libaether.a.
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <inttypes.h>

/* ── Base string / memory functions ────────────────────────────────────── */

size_t strlen(const char *s)
{
    size_t n = 0; while (s[n]) n++; return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0; while (n < max && s[n]) n++; return n;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *D = d; const unsigned char *S = s;
    while (n--) *D++ = *S++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *D = d; const unsigned char *S = s;
    if (D < S || D >= S + n) { while (n--) *D++ = *S++; }
    else { D += n; S += n; while (n--) *--D = *--S; }
    return d;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s; while (n--) *p++ = (unsigned char)c; return s;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = a, *pb = b;
    while (n--) { if (*pa != *pb) return *pa - *pb; pa++; pb++; }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    while (n--) { if (*p == (unsigned char)c) return (void *)p; p++; }
    return 0;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && *a == *b) { a++; b++; }
    if ((ssize_t)n < 0) return 0; /* cast to avoid -Wsign-compare */
    return (unsigned char)*a - (unsigned char)*b;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        { a++; b++; }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    while (n-- && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
        { a++; b++; }
    if ((ssize_t)n < 0) return 0;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strcoll(const char *a, const char *b) { return strcmp(a, b); }

size_t strxfrm(char *dst, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (n) { size_t cp = len < n - 1 ? len : n - 1; memcpy(dst, src, cp); dst[cp] = '\0'; }
    return len;
}

char *strcpy(char *d, const char *s)
{
    char *r = d; while ((*d++ = *s++)); return r;
}

char *stpcpy(char *d, const char *s)
{
    while ((*d = *s++)) d++;
    return d;
}

char *strncpy(char *d, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = '\0';
    return d;
}

char *strcat(char *d, const char *s)
{
    char *r = d; d += strlen(d); while ((*d++ = *s++)); return r;
}

char *strncat(char *d, const char *s, size_t n)
{
    char *r = d; d += strlen(d);
    while (n-- && *s) *d++ = *s++;
    *d = '\0'; return r;
}

char *strchr(const char *s, int c)
{
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) return (char *)s;
    return c == '\0' ? (char *)s : 0;
}

char *strrchr(const char *s, int c)
{
    const char *last = 0;
    for (; *s; s++) if ((unsigned char)*s == (unsigned char)c) last = s;
    return c == '\0' ? (char *)s : (char *)last;
}

char *strstr(const char *hay, const char *needle)
{
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return 0;
}

char *strcasestr(const char *hay, const char *needle)
{
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n))
            { h++; n++; }
        if (!*n) return (char *)hay;
    }
    return 0;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++)
        for (const char *a = accept; *a; a++)
            if (*s == *a) return (char *)s;
    return 0;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    for (; *s; s++, n++) {
        const char *a = accept;
        while (*a && *a != *s) a++;
        if (!*a) break;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    for (; *s; s++, n++) {
        const char *r = reject;
        while (*r && *r != *s) r++;
        if (*r) break;
    }
    return n;
}

/* strtok (non-reentrant) */
static char *_strtok_ptr;
char *strtok(char *s, const char *delim)
{
    if (s) _strtok_ptr = s;
    if (!_strtok_ptr) return 0;
    while (*_strtok_ptr && strchr(delim, *_strtok_ptr)) _strtok_ptr++;
    if (!*_strtok_ptr) return 0;
    char *tok = _strtok_ptr;
    while (*_strtok_ptr && !strchr(delim, *_strtok_ptr)) _strtok_ptr++;
    if (*_strtok_ptr) { *_strtok_ptr = '\0'; _strtok_ptr++; }
    return tok;
}

char *strtok_r(char *s, const char *delim, char **sv)
{
    if (s) *sv = s;
    if (!*sv) return 0;
    while (**sv && strchr(delim, **sv)) (*sv)++;
    if (!**sv) return 0;
    char *tok = *sv;
    while (**sv && !strchr(delim, **sv)) (*sv)++;
    if (**sv) { **sv = '\0'; (*sv)++; }
    return tok;
}

/* ── String duplication ──────────────────────────────────────────────── */

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}

/* ── Error strings ───────────────────────────────────────────────────── */

char *strerror(int e)
{
    switch (e) {
    case 0:        return "Success";
    case EPERM:    return "Operation not permitted";
    case ENOENT:   return "No such file or directory";
    case EIO:      return "I/O error";
    case ENOMEM:   return "Out of memory";
    case EACCES:   return "Permission denied";
    case EBADF:    return "Bad file descriptor";
    case EINVAL:   return "Invalid argument";
    case ENOSYS:   return "Function not implemented";
    case ENOTSOCK: return "Not a socket";
    case ECONNREFUSED: return "Connection refused";
    case ETIMEDOUT:    return "Connection timed out";
    case ENETUNREACH:  return "Network unreachable";
    case EPIPE:        return "Broken pipe";
    default:       return "Unknown error";
    }
}

/* ── Integer conversion ─────────────────────────────────────────────── */

static unsigned long long _strtou(const char *s, char **endp, int base,
                                   int is_signed, int *neg_out)
{
    while (isspace((unsigned char)*s)) s++;
    int neg = 0;
    if (*s == '+') s++;
    else if (*s == '-') { neg = 1; s++; }
    if (neg_out) *neg_out = neg;

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
        else if (s[0] == '0') { base = 8; s++; }
        else base = 10;
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }

    unsigned long long v = 0;
    int any = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9')     d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
        any = 1;
        s++;
    }
    (void)any;
    if (endp) *endp = (char *)s;
    return v;
}

long strtol(const char *s, char **endp, int base)
{
    int neg;
    unsigned long long v = _strtou(s, endp, base, 1, &neg);
    if (neg) {
        if (v > (unsigned long long)LONG_MAX + 1) { errno = ERANGE; return LONG_MIN; }
        return -(long)v;
    }
    if (v > (unsigned long long)LONG_MAX) { errno = ERANGE; return LONG_MAX; }
    return (long)v;
}

unsigned long strtoul(const char *s, char **endp, int base)
{
    unsigned long long v = _strtou(s, endp, base, 0, 0);
    if (v > ULONG_MAX) { errno = ERANGE; return ULONG_MAX; }
    return (unsigned long)v;
}

long long strtoll(const char *s, char **endp, int base)
{
    int neg;
    unsigned long long v = _strtou(s, endp, base, 1, &neg);
    if (neg) {
        if (v > (unsigned long long)LLONG_MAX + 1) { errno = ERANGE; return LLONG_MIN; }
        return -(long long)v;
    }
    if (v > (unsigned long long)LLONG_MAX) { errno = ERANGE; return LLONG_MAX; }
    return (long long)v;
}

unsigned long long strtoull(const char *s, char **endp, int base)
{
    return _strtou(s, endp, base, 0, 0);
}

intmax_t strtoimax(const char *s, char **endp, int base)
{
    return (intmax_t)strtoll(s, endp, base);
}

uintmax_t strtoumax(const char *s, char **endp, int base)
{
    return (uintmax_t)strtoull(s, endp, base);
}
