#ifndef _POSIX_STDATOMIC_H
#define _POSIX_STDATOMIC_H

/* AetherOS stdatomic.h stub — single-threaded bare-metal.
 * QuickJS defines CONFIG_ATOMICS unconditionally and uses atomic types to
 * implement the JS Atomics object and SharedArrayBuffer.  Since AetherOS has
 * no real threads, all operations are plain loads/stores; the JS Atomics API
 * exists in the engine but is never exercised in our integration tests. */

#include <stdint.h>

/* Memory orders (values match the C11 standard) */
typedef enum memory_order {
    memory_order_relaxed = 0,
    memory_order_consume = 1,
    memory_order_acquire = 2,
    memory_order_release = 3,
    memory_order_acq_rel = 4,
    memory_order_seq_cst = 5,
} memory_order;

/* Atomic integer types — plain non-atomic on single-threaded bare-metal */
typedef int               atomic_int;
typedef unsigned int      atomic_uint;
typedef int               atomic_bool;
typedef long              atomic_long;
typedef unsigned long     atomic_ulong;
typedef unsigned int      atomic_flag;
typedef uint8_t           atomic_uint_least8_t;
typedef uint16_t          atomic_uint_least16_t;
typedef uint32_t          atomic_uint_least32_t;
typedef uint64_t          atomic_uint_least64_t;
typedef int8_t            atomic_int_least8_t;
typedef int16_t           atomic_int_least16_t;
typedef int32_t           atomic_int_least32_t;
typedef int64_t           atomic_int_least64_t;
typedef intptr_t          atomic_intptr_t;
typedef uintptr_t         atomic_uintptr_t;
typedef size_t            atomic_size_t;
typedef ptrdiff_t         atomic_ptrdiff_t;

/* Load / store — plain memory access (single-threaded safe) */
#define atomic_load(obj)                        (*(obj))
#define atomic_load_explicit(obj, order)        (*(obj))
#define atomic_store(obj, val)                  (*(obj) = (val))
#define atomic_store_explicit(obj, val, order)  (*(obj) = (val))

/* Read-modify-write */
#define atomic_exchange(obj, val)               ({ __typeof__(*(obj)) _o = *(obj); *(obj) = (val); _o; })
#define atomic_exchange_explicit(obj, val, ord) atomic_exchange(obj, val)

#define atomic_fetch_add(obj, val)              ({ __typeof__(*(obj)) _o = *(obj); *(obj) += (val); _o; })
#define atomic_fetch_sub(obj, val)              ({ __typeof__(*(obj)) _o = *(obj); *(obj) -= (val); _o; })
#define atomic_fetch_or(obj, val)               ({ __typeof__(*(obj)) _o = *(obj); *(obj) |= (val); _o; })
#define atomic_fetch_and(obj, val)              ({ __typeof__(*(obj)) _o = *(obj); *(obj) &= (val); _o; })
#define atomic_fetch_xor(obj, val)              ({ __typeof__(*(obj)) _o = *(obj); *(obj) ^= (val); _o; })

#define atomic_fetch_add_explicit(o,v,m) atomic_fetch_add(o,v)
#define atomic_fetch_sub_explicit(o,v,m) atomic_fetch_sub(o,v)
#define atomic_fetch_or_explicit(o,v,m)  atomic_fetch_or(o,v)
#define atomic_fetch_and_explicit(o,v,m) atomic_fetch_and(o,v)

/* Compare-and-swap */
#define atomic_compare_exchange_strong(obj, exp, des) \
    ({ int _r = (*(obj) == *(exp)); if (_r) *(obj) = (des); else *(exp) = *(obj); _r; })
#define atomic_compare_exchange_weak(obj, exp, des) \
    atomic_compare_exchange_strong(obj, exp, des)
#define atomic_compare_exchange_strong_explicit(o,e,d,s,f) atomic_compare_exchange_strong(o,e,d)
#define atomic_compare_exchange_weak_explicit(o,e,d,s,f)   atomic_compare_exchange_strong(o,e,d)

/* Flags */
#define ATOMIC_FLAG_INIT 0
#define atomic_flag_test_and_set(f)         ({ int _v = *(f); *(f) = 1; _v; })
#define atomic_flag_clear(f)                (*(f) = 0)
#define atomic_flag_test_and_set_explicit(f,o) atomic_flag_test_and_set(f)
#define atomic_flag_clear_explicit(f,o)        atomic_flag_clear(f)

/* Fence / init */
#define atomic_thread_fence(order)  ((void)(order))
#define atomic_signal_fence(order)  ((void)(order))
#define atomic_init(obj, val)       (*(obj) = (val))
#define ATOMIC_VAR_INIT(val)        (val)

/* is_lock_free: always report true on bare-metal (no contention possible) */
#define atomic_is_lock_free(obj)  1

#endif /* _POSIX_STDATOMIC_H */
