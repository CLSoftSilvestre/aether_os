/* stub iconv.h for AetherOS — iconv is not implemented */
#ifndef _ICONV_H
#define _ICONV_H

#include <stddef.h>

typedef void *iconv_t;

#define ICONV_NONE ((iconv_t)(-1))

static inline iconv_t iconv_open(const char *tocode, const char *fromcode)
{
    (void)tocode; (void)fromcode;
    return ICONV_NONE;
}

static inline size_t iconv(iconv_t cd,
                            char **inbuf, size_t *inbytesleft,
                            char **outbuf, size_t *outbytesleft)
{
    (void)cd; (void)inbuf; (void)inbytesleft; (void)outbuf; (void)outbytesleft;
    return (size_t)(-1);
}

static inline int iconv_close(iconv_t cd)
{
    (void)cd;
    return 0;
}

#endif /* _ICONV_H */
