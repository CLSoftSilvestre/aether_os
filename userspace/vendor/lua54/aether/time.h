#ifndef _AETHER_LUA_TIME_H
#define _AETHER_LUA_TIME_H
/* time_t, clock_t, clock(), time(), struct tm, localtime(), etc.
   are all defined in luaconf.h (included first via lprefix.h). */
#ifndef CLOCKS_PER_SEC
#define CLOCKS_PER_SEC 100L
#endif
#endif /* _AETHER_LUA_TIME_H */
