#ifndef AETHER_SPINLOCK_H
#define AETHER_SPINLOCK_H

/*
 * AetherOS — SMP Spinlock
 * File: kernel/include/aether/spinlock.h
 *
 * Ticket-less test-and-set spinlock using AArch64 exclusive access.
 * Implementation is in kernel/arch/arm64/spinlock.S.
 *
 * Usage:
 *   spinlock_t lock = SPINLOCK_INIT;
 *   spin_lock(&lock);
 *   // critical section
 *   spin_unlock(&lock);
 */

#include "aether/types.h"

typedef u32 spinlock_t;

#define SPINLOCK_INIT  0u

/*
 * spin_lock — busy-wait until the lock is acquired.
 * Uses LDAXR/STLXR for acquire semantics: no load or store after
 * spin_lock() can be reordered to execute before it.
 */
void spin_lock(spinlock_t *lock);

/*
 * spin_unlock — release the lock.
 * Uses STLR for release semantics: no load or store before
 * spin_unlock() can be reordered to execute after it.
 */
void spin_unlock(spinlock_t *lock);

#endif /* AETHER_SPINLOCK_H */
