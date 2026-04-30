#ifndef _AETHER_LUA_LOCALE_H
#define _AETHER_LUA_LOCALE_H
/* struct lconv and localeconv() are defined in luaconf.h (included first). */
#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
static inline char *setlocale(int cat, const char *locale)
{ (void)cat; (void)locale; return (char *)"C"; }
#endif /* _AETHER_LUA_LOCALE_H */
