#ifndef _POSIX_ASSERT_H
#define _POSIX_ASSERT_H

#ifdef NDEBUG
#  define assert(e) ((void)(e))
#else
#  define assert(e) ((e) ? (void)0 : __builtin_trap())
#endif

#define static_assert _Static_assert

#endif /* _POSIX_ASSERT_H */
