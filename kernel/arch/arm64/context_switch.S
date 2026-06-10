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
 *   1. Saves 'from' callee-saved registers to *from   — 'from' context committed
 *   2. Releases *lock via STLR                        — safe: from->ctx is complete
 *   3. Loads  'to'  callee-saved registers from *to
 *   4. Switches SP to 'to' stack, branches to 'to' x30
 *
 * Releasing the lock BETWEEN save and load is the critical design point:
 *   - Before the release: no other core can pick 'from' (lock held, state=READY but blocked)
 *   - After  the release: 'from' is schedulable; another core may load from->ctx
 *     concurrently, but we are already loading 'to' ctx into OUR registers — no conflict.
 *   - We never touch 'from' stack after the STLR, so no use-after-free hazard.
 *
 * x0 = from cpu_context_t*, x1 = to cpu_context_t*, x2 = spinlock_t*
 */
.global context_switch_smp
context_switch_smp:
    /* Step 1 — save 'from' callee-saved registers (same layout as context_switch) */
    stp  x19, x20, [x0, #0]
    stp  x21, x22, [x0, #16]
    stp  x23, x24, [x0, #32]
    stp  x25, x26, [x0, #48]
    stp  x27, x28, [x0, #64]
    stp  x29, x30, [x0, #80]
    mov  x8, sp
    str  x8,       [x0, #96]

    /* Step 2 — release the scheduler spinlock now that from->ctx is committed.
     * STLR = store-release: all prior stores (the stp/str above) are globally
     * visible before *x2 is cleared.  Other cores can now safely pick 'from'. */
    stlr  wzr, [x2]

    /* Step 3 — load 'to' callee-saved registers.
     * We are still physically on 'from' stack, but we do NOT access it — every
     * load below reads from [x1+offset], not from the stack.  Safe. */
    ldp  x19, x20, [x1, #0]
    ldp  x21, x22, [x1, #16]
    ldp  x23, x24, [x1, #32]
    ldp  x25, x26, [x1, #48]
    ldp  x27, x28, [x1, #64]
    ldp  x29, x30, [x1, #80]   /* x30 = 'to' task's saved LR (next PC after ret) */

    /* Step 4 — switch stack pointer and branch into 'to' task */
    ldr  x8,       [x1, #96]
    mov  sp, x8               /* switch to 'to' task's stack                    */

    /* ret branches to 'to' task's x30:
     *   – existing task: the instruction after its last context_switch_smp call
     *   – new task:      its entry function (set in alloc_task)                 */
    ret
