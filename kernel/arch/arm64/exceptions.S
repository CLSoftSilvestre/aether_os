/*
 * AetherOS — AArch64 Exception Vector Table
 * File: kernel/arch/arm64/exceptions.S
 *
 * The vector table is the ARM equivalent of the x86 IDT (Interrupt Descriptor
 * Table). When any exception occurs — timer IRQ, bad memory access, syscall —
 * the CPU looks up VBAR_EL1 and jumps to the appropriate slot.
 *
 * AArch64 vector table layout (VBAR_EL1 must be 2KB-aligned):
 *
 *   Offset  Exception type                     When
 *   0x000   Synchronous, current EL, SP_EL0    (unusual kernel config)
 *   0x080   IRQ,         current EL, SP_EL0
 *   0x100   FIQ,         current EL, SP_EL0
 *   0x180   SError,      current EL, SP_EL0
 *   0x200   Synchronous, current EL, SP_EL1    ← kernel faults
 *   0x280   IRQ,         current EL, SP_EL1    ← hardware interrupts
 *   0x300   FIQ,         current EL, SP_EL1
 *   0x380   SError,      current EL, SP_EL1
 *   0x400   Synchronous, lower EL, AArch64     ← syscalls (Phase 3)
 *   0x480   IRQ,         lower EL, AArch64     ← IRQ from userspace (Phase 3)
 *   0x500   FIQ,         lower EL, AArch64
 *   0x580   SError,      lower EL, AArch64
 *   0x600   Synchronous, lower EL, AArch32     (32-bit compat, not used)
 *   0x680   IRQ,         lower EL, AArch32
 *   0x700   FIQ,         lower EL, AArch32
 *   0x780   SError,      lower EL, AArch32
 *
 * Each slot is 128 bytes (32 instructions). If the handler is longer we
 * branch to a separate stub — our EXCEPTION_ENTRY macro is ~35 instructions
 * so we always branch out.
 *
 * === Register saving (trap frame) ===
 *
 * We push a trap_frame_t onto the kernel stack (see exceptions.h).
 * The struct layout MUST match the offsets used in EXCEPTION_ENTRY/EXIT.
 *
 *   sp +   0: x0  … x29   (30 regs × 8 = 240 bytes, 15 stp pairs)
 *   sp + 240: x30 (link register)
 *   sp + 248: ELR_EL1     (interrupted PC  — "where the CPU was")
 *   sp + 256: SPSR_EL1    (interrupted state)
 *   sp + 264: ESR_EL1     (fault reason)
 *   ──────────────────────────────────
 *   Total: 272 bytes (272 / 16 = 17 — 16-byte aligned ✓)
 */

/* ── Context save / restore macros ─────────────────────────────────────── */

/*
 * EXCEPTION_ENTRY — save all general-purpose registers + fault info.
 *
 * After this macro, sp points to the start of the trap_frame_t and
 * can be passed as the first argument (x0) to C handlers.
 *
 * stp = Store Pair (two 64-bit registers in one instruction — efficient)
 * mrs = Move from System Register
 */
.macro EXCEPTION_ENTRY
    sub  sp, sp, #272

    stp  x0,  x1,  [sp, #0]
    stp  x2,  x3,  [sp, #16]
    stp  x4,  x5,  [sp, #32]
    stp  x6,  x7,  [sp, #48]
    stp  x8,  x9,  [sp, #64]
    stp  x10, x11, [sp, #80]
    stp  x12, x13, [sp, #96]
    stp  x14, x15, [sp, #112]
    stp  x16, x17, [sp, #128]
    stp  x18, x19, [sp, #144]
    stp  x20, x21, [sp, #160]
    stp  x22, x23, [sp, #176]
    stp  x24, x25, [sp, #192]
    stp  x26, x27, [sp, #208]
    stp  x28, x29, [sp, #224]

    /* Save x30 (link register) and ELR_EL1 (interrupted PC) together */
    mrs  x0, ELR_EL1
    stp  x30, x0, [sp, #240]

    /* Save SPSR_EL1 (interrupted PSTATE) and ESR_EL1 (fault syndrome) */
    mrs  x0, SPSR_EL1
    mrs  x1, ESR_EL1
    stp  x0, x1, [sp, #256]
.endm

/*
 * EXCEPTION_EXIT — restore all registers and return via eret.
 *
 * eret restores PSTATE from SPSR_EL1 and jumps to ELR_EL1.
 * In x86 terms: it's like IRET — restores flags+CS+IP atomically.
 *
 * ldp = Load Pair (inverse of stp)
 */
.macro EXCEPTION_EXIT
    /* Restore SPSR and ELR first (need scratch regs, so do before x0-x1 restore) */
    ldp  x0, x1, [sp, #256]     /* x0=SPSR, x1=ESR (discard ESR) */
    msr  SPSR_EL1, x0

    ldp  x30, x0, [sp, #240]    /* x30=LR, x0=ELR */
    msr  ELR_EL1, x0

    ldp  x28, x29, [sp, #224]
    ldp  x26, x27, [sp, #208]
    ldp  x24, x25, [sp, #192]
    ldp  x22, x23, [sp, #176]
    ldp  x20, x21, [sp, #160]
    ldp  x18, x19, [sp, #144]
    ldp  x16, x17, [sp, #128]
    ldp  x14, x15, [sp, #112]
    ldp  x12, x13, [sp, #96]
    ldp  x10, x11, [sp, #80]
    ldp  x8,  x9,  [sp, #64]
    ldp  x6,  x7,  [sp, #48]
    ldp  x4,  x5,  [sp, #32]
    ldp  x2,  x3,  [sp, #16]
    ldp  x0,  x1,  [sp, #0]

    add  sp, sp, #272
    eret
.endm

/* ── Vector table ─────────────────────────────────────────────────────── */

/*
 * The vector table must be 2KB (2^11) aligned.
 * Each entry is 128 bytes (2^7). We use .align 7 to pad between entries.
 */
.section ".text"
.align 11
.global vector_table
vector_table:

    /* ── Group 1: Current EL with SP_EL0 (unusual; kernel always uses SP_EL1) */
    .align 7;  b  _el1_sp0_sync    /* 0x000 */
    .align 7;  b  _el1_sp0_irq     /* 0x080 */
    .align 7;  b  _el1_sp0_fiq     /* 0x100 */
    .align 7;  b  _el1_sp0_serror  /* 0x180 */

    /* ── Group 2: Current EL with SP_EL1 (normal kernel exceptions) */
    .align 7;  b  _el1_sync        /* 0x200 — kernel fault handler */
    .align 7;  b  _el1_irq         /* 0x280 — hardware interrupt handler */
    .align 7;  b  _el1_fiq         /* 0x300 */
    .align 7;  b  _el1_serror      /* 0x380 */

    /* ── Group 3: Lower EL (AArch64) — exceptions from user-space (Phase 3) */
    .align 7;  b  _el0_sync        /* 0x400 — syscalls via SVC #0 */
    .align 7;  b  _el0_irq         /* 0x480 — IRQ while in user mode */
    .align 7;  b  _el0_fiq         /* 0x500 */
    .align 7;  b  _el0_serror      /* 0x580 */

    /* ── Group 4: Lower EL (AArch32) — not supported */
    .align 7;  b  _el0_aarch32_unsupported   /* 0x600 */
    .align 7;  b  _el0_aarch32_unsupported   /* 0x680 */
    .align 7;  b  _el0_aarch32_unsupported   /* 0x700 */
    .align 7;  b  _el0_aarch32_unsupported   /* 0x780 */

/* ── Exception stub implementations ──────────────────────────────────── */

/*
 * Each stub follows the same pattern:
 *   1. EXCEPTION_ENTRY — save all registers, build trap_frame_t on stack
 *   2. mov x0, sp      — pass pointer to trap_frame_t as first C argument
 *   3. bl  <c_handler> — call C function (x0-x7 = args, x30 = return addr)
 *   4. EXCEPTION_EXIT  — restore registers and eret
 */

/* ── Current EL, SP_EL0 — should never happen with our setup ── */
_el1_sp0_sync:
    EXCEPTION_ENTRY
    mov  x0, sp
    bl   el1_sync_handler
    EXCEPTION_EXIT

_el1_sp0_irq:
    EXCEPTION_ENTRY
    mov  x0, sp
    bl   el1_irq_handler
    EXCEPTION_EXIT

_el1_sp0_fiq:
    /* FIQ not used — treat as SError */
    b    _el1_serror

_el1_sp0_serror:
    EXCEPTION_ENTRY
    mov  x0, sp
    bl   el1_serror_handler
    /* SError is fatal — handler should panic, but add loop for safety */
1:  wfe
    b    1b

/* ── Current EL, SP_EL1 — normal kernel exceptions ── */
_el1_sync:
    EXCEPTION_ENTRY
    mov  x0, sp
    bl   el1_sync_handler      /* handles data aborts, undefined, BRK */
    EXCEPTION_EXIT

_el1_irq:
    EXCEPTION_ENTRY
    mov  x0, sp
    bl   el1_irq_handler       /* dispatches to GIC → timer, UART, etc. */
    EXCEPTION_EXIT

_el1_fiq:
    b    _el1_serror            /* FIQ not used — escalate to SError handler */

_el1_serror:
    EXCEPTION_ENTRY
    mov  x0, sp
    bl   el1_serror_handler    /* SError = async abort — typically fatal */
1:  wfe
    b    1b

/* ── Lower EL (AArch64) — user-space exceptions ── */
_el0_sync:
    EXCEPTION_ENTRY
    mov  x0, sp
    bl   el0_sync_handler      /* Phase 3: SVC #0 = syscall dispatch */
    EXCEPTION_EXIT

_el0_irq:
    EXCEPTION_ENTRY
    mov  x0, sp
    bl   el0_irq_handler       /* Phase 3: IRQ while in user mode */
    EXCEPTION_EXIT

_el0_fiq:
    b    _el0_irq               /* redirect FIQ */

_el0_serror:
    EXCEPTION_ENTRY
    mov  x0, sp
    bl   el1_serror_handler    /* user SError → fatal */
1:  wfe
    b    1b

_el0_aarch32_unsupported:
    /* 32-bit AArch32 not supported — just halt */
1:  wfe
    b    1b
