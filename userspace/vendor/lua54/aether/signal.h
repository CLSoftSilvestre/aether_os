#ifndef _AETHER_LUA_SIGNAL_H
#define _AETHER_LUA_SIGNAL_H
/* sig_atomic_t, SIGINT, signal() are defined in luaconf.h (included first). */
#ifndef SIGTERM
#define SIGTERM  15
#endif
#ifndef SIGABRT
#define SIGABRT   6
#endif
#ifndef SIG_DFL
#define SIG_DFL  ((void (*)(int))0)
#endif
#ifndef SIG_IGN
#define SIG_IGN  ((void (*)(int))1)
#endif
#ifndef SIG_ERR
#define SIG_ERR  ((void (*)(int))-1)
#endif
#endif /* _AETHER_LUA_SIGNAL_H */
