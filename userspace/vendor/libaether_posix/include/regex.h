/* stub regex.h for AetherOS — POSIX regex is not implemented;
 * NetSurf only uses it for the save_complete CSS rewrite, which is
 * a non-critical desktop feature.  All calls return failure. */
#ifndef _REGEX_H
#define _REGEX_H

#include <stddef.h>
#include <sys/types.h>

#define REG_NOMATCH   1
#define REG_EXTENDED  1
#define REG_ICASE    (1 << 1)
#define REG_NEWLINE  (1 << 2)
#define REG_NOSUB    (1 << 3)

typedef ssize_t regoff_t;

typedef struct {
    size_t re_nsub;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

static inline int regcomp(regex_t *preg, const char *regex, int cflags)
{
    (void)preg; (void)regex; (void)cflags;
    return REG_NOMATCH;
}

static inline int regexec(const regex_t *preg, const char *str,
                           size_t nmatch, regmatch_t pmatch[], int eflags)
{
    (void)preg; (void)str; (void)nmatch; (void)pmatch; (void)eflags;
    return REG_NOMATCH;
}

static inline size_t regerror(int err, const regex_t *preg,
                               char *errbuf, size_t errbuf_size)
{
    (void)err; (void)preg; (void)errbuf; (void)errbuf_size;
    return 0;
}

static inline void regfree(regex_t *preg)
{
    (void)preg;
}

#endif /* _REGEX_H */
