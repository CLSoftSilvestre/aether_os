#ifndef _AETHER_ASSERT_H
#define _AETHER_ASSERT_H

#ifdef NDEBUG
#define assert(e) ((void)(e))
#else
#define assert(e) ((e) ? (void)0 : __builtin_trap())
#endif

#endif /* _AETHER_ASSERT_H */
