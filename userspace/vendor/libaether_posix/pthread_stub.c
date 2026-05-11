/*
 * libaether_posix/pthread_stub.c — single-threaded pthread stubs
 *
 * AetherOS Phase 7.0 runs ported apps in single-threaded mode.
 * NetSurf's fetch subsystem is compiled with NSFB_SURFACE_NONE / no threads.
 * These stubs satisfy link-time symbol references from NetSurf support
 * libraries (libcss, libdom) that may call pthread_mutex_lock in headers.
 *
 * All mutex operations succeed immediately (no-op).
 * pthread_create runs the thread function inline and returns 0.
 */

#include <pthread.h>
#include <errno.h>
#include <stdlib.h>

/* ── Key store (single-threaded: one value per key) ─────────────────── */
#define MAX_KEYS 32
static void *_tls_vals[MAX_KEYS];
static void (*_tls_dtors[MAX_KEYS])(void *);
static int _key_count = 0;

/* ── Mutex ───────────────────────────────────────────────────────────── */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{ (void)a; *m = 0; return 0; }

int pthread_mutex_lock(pthread_mutex_t *m)
{ (void)m; return 0; }

int pthread_mutex_trylock(pthread_mutex_t *m)
{ (void)m; return 0; }

int pthread_mutex_unlock(pthread_mutex_t *m)
{ (void)m; return 0; }

int pthread_mutex_destroy(pthread_mutex_t *m)
{ (void)m; return 0; }

int pthread_mutexattr_init(pthread_mutexattr_t *a)
{ a->dummy = 0; return 0; }

int pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{ (void)a; return 0; }

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{ a->dummy = type; return 0; }

/* ── Condition variable ──────────────────────────────────────────────── */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a)
{ (void)a; *c = 0; return 0; }

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{ (void)c; (void)m; return 0; }

int pthread_cond_signal(pthread_cond_t *c)   { (void)c; return 0; }
int pthread_cond_broadcast(pthread_cond_t *c){ (void)c; return 0; }
int pthread_cond_destroy(pthread_cond_t *c)  { (void)c; return 0; }

/* ── Read-write lock ─────────────────────────────────────────────────── */

int pthread_rwlock_init(pthread_rwlock_t *rw, const pthread_rwlockattr_t *a)
{ (void)a; *rw = 0; return 0; }

int pthread_rwlock_rdlock(pthread_rwlock_t *rw){ (void)rw; return 0; }
int pthread_rwlock_wrlock(pthread_rwlock_t *rw){ (void)rw; return 0; }
int pthread_rwlock_unlock(pthread_rwlock_t *rw){ (void)rw; return 0; }
int pthread_rwlock_destroy(pthread_rwlock_t *rw){ (void)rw; return 0; }

/* ── Thread creation ─────────────────────────────────────────────────── */

int pthread_create(pthread_t *t, const pthread_attr_t *a,
                   void *(*fn)(void *), void *arg)
{
    (void)a;
    /* Run the thread function inline — single-threaded mode */
    void *ret = fn(arg);
    (void)ret;
    if (t) *t = 1;
    return 0;
}

int pthread_join(pthread_t t, void **retval)
{
    (void)t;
    if (retval) *retval = NULL;
    return 0;
}

int pthread_detach(pthread_t t) { (void)t; return 0; }

pthread_t pthread_self(void) { return 1; }

/* ── Once ────────────────────────────────────────────────────────────── */

int pthread_once(pthread_once_t *once, void (*fn)(void))
{
    if (*once == 0) { *once = 1; fn(); }
    return 0;
}

/* ── Thread-local storage ────────────────────────────────────────────── */

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    if (_key_count >= MAX_KEYS) { errno = EAGAIN; return -1; }
    *key = _key_count;
    _tls_vals[_key_count] = NULL;
    _tls_dtors[_key_count] = destructor;
    _key_count++;
    return 0;
}

int pthread_key_delete(pthread_key_t key)
{
    if (key < 0 || key >= _key_count) return EINVAL;
    return 0;
}

void *pthread_getspecific(pthread_key_t key)
{
    if (key < 0 || key >= _key_count) return NULL;
    return _tls_vals[key];
}

int pthread_setspecific(pthread_key_t key, const void *val)
{
    if (key < 0 || key >= _key_count) return EINVAL;
    _tls_vals[key] = (void *)val;
    return 0;
}
