/*
 * AetherOS — AArch64 SMP Spinlock
 * File: kernel/arch/arm64/spinlock.S
 *
 * Implements a test-and-set spinlock using AArch64 exclusive memory access.
 *
 * Instructions used:
 *   LDAXR  — Load-Acquire Exclusive Register.
 *            Reads a word and places an exclusive reservation on the address.
 *            "Acquire" means: no load/store AFTER this can be reordered BEFORE it.
 *            In x86 terms: like LOCK CMPXCHG with an implicit LFENCE.
 *
 *   STLXR  — Store-Release Exclusive Register.
 *            Attempts to store a word; succeeds (result=0) only if the exclusive
 *            reservation is still valid (no other core has written the address).
 *            "Release" means: no load/store BEFORE this can be reordered AFTER it.
 *            If another core grabbed the reservation first, result=1 and we retry.
 *
 *   STLR   — Store-Release Register (no exclusive, used for unlock).
 *            Guarantees that all prior memory operations are visible before the
 *            store. Equivalent to: memory_barrier(); *lock = 0;
 *
 * Together LDAXR+STLXR give the correct acquire/release ordering required
 * by the ARMv8 memory model for spinlocks on weakly-ordered hardware.
 *
 * 8086 analogy:
 *   spin_lock   ≈  LOCK BTS [lock], 0   (set bit, spin if already set)
 *   spin_unlock ≈  LOCK BTR [lock], 0   (clear bit)
 */

.section ".text"
.global spin_lock
.global spin_unlock

/* ── spin_lock(spinlock_t *lock) ─────────────────────────────────────────── */
/*
 * Busy-waits until *lock can be atomically changed from 0 (free) to 1 (held).
 * x0 = pointer to the lock (u32).
 */
spin_lock:
.L_spin_retry:
    ldaxr   w1, [x0]           /* load-acquire exclusive: w1 = *lock           */
    cbnz    w1, .L_spin_retry  /* already locked (1) — spin without exclusive   */
    mov     w2, #1
    stlxr   w3, w2, [x0]      /* try store-release 1; w3=0 on success, 1 on fail */
    cbnz    w3, .L_spin_retry  /* another core raced — retry from the top        */
    ret

/* ── spin_unlock(spinlock_t *lock) ──────────────────────────────────────── */
/*
 * Releases the lock.  STLR ensures all writes done inside the critical
 * section are globally visible before the lock word is cleared.
 * x0 = pointer to the lock.
 */
spin_unlock:
    stlr    wzr, [x0]          /* store-release 0 to *lock                      */
    ret
