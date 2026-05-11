#ifndef _POSIX_PTHREAD_H
#define _POSIX_PTHREAD_H

#include <sys/types.h>

/* All pthread primitives are single-threaded stubs.
 * NetSurf's fetch subsystem is compiled with threading disabled; these stubs
 * satisfy link-time dependencies from library headers without requiring a
 * real thread scheduler.                                                      */

typedef int pthread_t;
typedef int pthread_mutex_t;
typedef int pthread_cond_t;
typedef int pthread_rwlock_t;
typedef int pthread_key_t;
typedef int pthread_once_t;

typedef struct {
    int dummy;
} pthread_attr_t;

typedef struct {
    int dummy;
} pthread_mutexattr_t;

typedef struct {
    int dummy;
} pthread_condattr_t;

typedef struct {
    int dummy;
} pthread_rwlockattr_t;

#define PTHREAD_MUTEX_INITIALIZER 0
#define PTHREAD_ONCE_INIT         0

/* Mutex */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int pthread_mutex_destroy(pthread_mutex_t *m);

/* Attributes (no-ops) */
int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_destroy(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);

/* Condition variable (no-ops) */
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);
int pthread_cond_destroy(pthread_cond_t *c);

/* Read-write lock (no-ops) */
int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *a);
int pthread_rwlock_rdlock(pthread_rwlock_t *rw);
int pthread_rwlock_wrlock(pthread_rwlock_t *rw);
int pthread_rwlock_unlock(pthread_rwlock_t *rw);
int pthread_rwlock_destroy(pthread_rwlock_t *rw);

/* Thread creation — runs fn(arg) inline then returns */
int pthread_create(pthread_t *t, const pthread_attr_t *a,
                   void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **retval);
int pthread_detach(pthread_t t);
pthread_t pthread_self(void);

/* Once */
int pthread_once(pthread_once_t *once, void (*fn)(void));

/* Thread-local storage (single-thread: just a global value) */
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *));
int pthread_key_delete(pthread_key_t key);
void *pthread_getspecific(pthread_key_t key);
int pthread_setspecific(pthread_key_t key, const void *val);

#define PTHREAD_MUTEX_NORMAL     0
#define PTHREAD_MUTEX_ERRORCHECK 1
#define PTHREAD_MUTEX_RECURSIVE  2

#endif /* _POSIX_PTHREAD_H */
