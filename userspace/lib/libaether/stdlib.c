#include <stdlib.h>
#include <sys.h>

/* ── atoi / atol ─────────────────────────────────────────────────────── */

long atol(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    long sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    long v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); }
    return sign * v;
}

int atoi(const char *s)
{
    return (int)atol(s);
}

int abs(int x)   { return x < 0 ? -x : x; }
long labs(long x){ return x < 0 ? -x : x; }

/* ── exit ────────────────────────────────────────────────────────────── */

__attribute__((noreturn))
void exit(int code)
{
    sys_exit(code);
}
