/*
 * AetherOS — AArch64 Cooperative Context Switch
 * File: kernel/arch/arm64/context_switch.S
 *
 * void context_switch(cpu_context_t *from, cpu_context_t *to)
 *   x0 = pointer to 'from' task's cpu_context_t
 *   x1 = pointer to 'to'   task's cpu_context_t
 *
 * ═══════════════════════════════════════════════════════════
 * Why only save x19–x30 and sp?
 * ═══════════════════════════════════════════════════════════
 * AArch64 (like x86-64) divides registers into two classes:
 *
 *   Caller-saved  (x0–x18): the CALLING function is responsible for
 *     saving these if it needs them after a function call. The callee
 *     (context_switch) is free to trash them.
 *
 *   Callee-saved  (x19–x28, x29, x30): the CALLED function must
 *     restore these before returning. We save/restore these here.
 *
 * In x86 terms:
 *   Caller-saved = eax, ecx, edx  (scratch registers)
 *   Callee-saved = ebx, esi, edi, ebp, esp
 *
 * When task A calls task_yield() → context_switch():
 *   - x0–x18 are already saved on task A's stack by the compiler
 *     (if task A cares about them)
 *   - We save x19–x30 + sp into task A's cpu_context_t
 *   - We load x19–x30 + sp from task B's cpu_context_t
 *   - ret → jumps to x30 of task B (where task B last called context_switch,
 *     or the entry function if task B is new)
 *
 * ═══════════════════════════════════════════════════════════
 * cpu_context_t memory layout (must match scheduler.h):
 * ═══════════════════════════════════════════════════════════
 *   Offset  Register
 *   0       x19
 *   8       x20
 *   16      x21
 *   24      x22
 *   32      x23
 *   40      x24
 *   48      x25
 *   56      x26
 *   64      x27
 *   72      x28
 *   80      x29  (frame pointer)
 *   88      x30  (link register — where ret will jump)
 *   96      sp   (stack pointer)
 */

.section ".text"
.global context_switch

/*
 * context_switch(cpu_context_t *from, cpu_context_t *to)
 *
 * After this function returns, we are running in the 'to' task's context:
 *   - SP is task B's stack
 *   - x19–x29 are task B's callee-saved registers
 *   - ret jumps to task B's x30 (either its entry point or where it
 *     last called context_switch)
 */
context_switch:
    /* ── Save 'from' task's callee-saved registers ── */
    /*
     * stp = Store Pair: stores two 64-bit registers to consecutive memory.
     * [x0, #offset] = address stored in x0 plus offset bytes.
     * This is like "MOV [ebx+offset], reg" in x86.
     */
    stp  x19, x20, [x0, #0]
    stp  x21, x22, [x0, #16]
    stp  x23, x24, [x0, #32]
    stp  x25, x26, [x0, #48]
    stp  x27, x28, [x0, #64]
    stp  x29, x30, [x0, #80]   /* x29=fp, x30=lr (return address) */

    /* Save stack pointer — can't use stp with sp directly, need mov first */
    mov  x8, sp
    str  x8,       [x0, #96]

    /* ── Load 'to' task's callee-saved registers ── */
    /*
     * ldp = Load Pair: inverse of stp.
     * After loading x30 from 'to', the final 'ret' will jump there.
     */
    ldp  x19, x20, [x1, #0]
    ldp  x21, x22, [x1, #16]
    ldp  x23, x24, [x1, #32]
    ldp  x25, x26, [x1, #48]
    ldp  x27, x28, [x1, #64]
    ldp  x29, x30, [x1, #80]   /* x30 now = 'to' task's return address */

    ldr  x8,       [x1, #96]
    mov  sp, x8               /* switch to 'to' task's stack */

    /*
     * ret — branches to the address in x30.
     *
     * If 'to' is an existing task: x30 = the return address inside
     *   context_switch() from when that task last called task_yield().
     *   So we appear to "return from task_yield()" in the other task.
     *
     * If 'to' is a NEW task (first time scheduled): x30 = entry function.
     *   ret jumps directly to the task's entry function, using the
     *   pre-set stack. The task starts running from its first instruction.
     *
     * This is the elegant core of cooperative scheduling:
     * every context_switch looks like a normal function return to both tasks.
     */
    ret

/* ── context_switch_smp ──────────────────────────────────────────────────── */
/*
 * context_switch_smp(cpu_context_t *from, cpu_context_t *to, spinlock_t *lock)
 *
 * SMP-safe context switch used by task_yield() on multi-core systems.
 *
 * The caller holds *lock (the scheduler spinlock) when it calls this function.
 * This function:
 *   1. Saves 'from' callee-saved registers + DAIF to *from
 *   2. Masks IRQs on THIS core           — must happen BEFORE the STLR;
 *      after STLR another core can immediately pick 'from' and load its SP,
 *      so any IRQ on this core after the STLR but before SP switch would
 *      push 288 bytes onto 'from' stack while another core is already there.
 *   3. Releases *lock via STLR           — 'from' is now schedulable
 *   4. Loads 'to' callee-saved registers from *to
 *   5. Switches SP to 'to' stack         — 'from' stack no longer in use
 *   6. Restores DAIF from 'to' context   — correct IRQ state for 'to' task
 *   7. ret → branches into 'to' task
 *
 * cpu_context_t offsets (must match scheduler.h):
 *   x19@0  x20@8  x21@16  x22@24  x23@32  x24@40  x25@48  x26@56
 *   x27@64 x28@72 x29@80  x30@88  sp@96   daif@104
 *
 * x0 = from cpu_context_t*, x1 = to cpu_context_t*, x2 = spinlock_t*
 */
.global context_switch_smp
context_switch_smp:
    /* Step 1 — save 'from' callee-saved registers */
    stp  x19, x20, [x0, #0]
    stp  x21, x22, [x0, #16]
    stp  x23, x24, [x0, #32]
    stp  x25, x26, [x0, #48]
    stp  x27, x28, [x0, #64]
    stp  x29, x30, [x0, #80]
    mov  x8, sp
    str  x8,       [x0, #96]
    mrs  x8, DAIF
    str  x8,       [x0, #104]  /* save IRQ mask state */

    /* Step 2 — mask IRQs on this core BEFORE releasing the lock.
     * After STLR another core can immediately pick 'from', load from->ctx.sp,
     * and start using 'from' kernel stack.  If a timer IRQ then fires on
     * this core in the gap between STLR and daifset, EXCEPTION_ENTRY pushes
     * 288 bytes onto from's stack while the other core is already there —
     * concurrent writes corrupt return addresses (seen as ELR=0x60000345).
     * Masking IRQs BEFORE the STLR closes this window completely: by the time
     * any other core can see 'from' as READY, IRQs are already masked here. */
    msr  daifset, #2

    /* Step 3 — release the scheduler spinlock now that from->ctx is committed.
     * STLR = store-release: all prior stores are globally visible before the
     * lock word is cleared.  Other cores can now safely pick 'from'. */
    stlr  wzr, [x2]

    /* Step 4 — load 'to' callee-saved registers.
     * Every load reads from [x1+offset], not from the stack — safe even
     * while SP_EL1 still points to 'from' stack. */
    ldp  x19, x20, [x1, #0]
    ldp  x21, x22, [x1, #16]
    ldp  x23, x24, [x1, #32]
    ldp  x25, x26, [x1, #48]
    ldp  x27, x28, [x1, #64]
    ldp  x29, x30, [x1, #80]   /* x30 = 'to' task's saved LR (next PC after ret) */

    /* Step 5 — switch stack pointer.
     * From this point 'from' stack is not touched by this core. */
    ldr  x8,       [x1, #96]
    mov  sp, x8

    /* Step 6 — restore 'to' task's DAIF (IRQ mask state).
     * If 'to' was in a SVC handler with IRQs enabled, this re-enables them.
     * If 'to' was in an IRQ handler with IRQs masked, they stay masked.
     * 'eret' in the exception-return path will restore DAIF again from SPSR
     * when returning to EL0, so this is always correct. */
    ldr  x8,       [x1, #104]
    msr  DAIF, x8

    /* ret branches to 'to' task's x30:
     *   – existing task: the resume point inside its last context_switch_smp call
     *   – new task:      its entry function (trampoline, set in alloc_task) */
    ret
