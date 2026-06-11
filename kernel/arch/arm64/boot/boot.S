/*
 * AetherOS — AArch64 Boot Entry Point
 * File: kernel/arch/arm64/boot/boot.S
 *
 * This is the very first code that runs. QEMU loads our kernel8.img into
 * RAM at 0x40000000 and jumps here.
 *
 * === AArch64 for someone who knows 8086/8085 ===
 *
 *  8086 concept       →  AArch64 equivalent
 *  ─────────────────────────────────────────────
 *  AX, BX, CX, DX    →  x0–x30  (31 general-purpose 64-bit registers)
 *  SP                 →  sp      (stack pointer, same role)
 *  IP                 →  pc      (program counter, not directly writable)
 *  JMP label          →  b  label
 *  CALL label         →  bl label  (branch with link — saves return addr in x30)
 *  RET                →  ret       (jumps to address in x30)
 *  INT 21h            →  svc #0    (supervisor call = syscall)
 *  MOV AX, [mem]      →  ldr x0, [x1]   (x1 holds address)
 *  MOV [mem], AX      →  str x0, [x1]
 *
 * Exception Levels (privilege rings):
 *   EL3 = highest privilege (secure firmware, like SMM in x86)
 *   EL2 = hypervisor (like VMX root mode)
 *   EL1 = OS kernel (like Ring 0)
 *   EL0 = user applications (like Ring 3)
 *
 * QEMU "-M virt -kernel" typically starts us at EL1 already, but we
 * handle EL2 and EL3 entry just in case.
 */

.section ".text.boot"    /* placed first in image by linker script */
.global _start

/* ─── Entry point ──────────────────────────────────────────────────────── */
_start:
    /*
     * Read the current Exception Level.
     * CurrentEL is a system register; bits [3:2] hold the EL number.
     * mrs = "Move to Register from System register" (like MOV AX, CR0 in x86)
     * lsr = Logical Shift Right (like SHR in x86)
     */
    mrs x0, CurrentEL
    lsr x0, x0, #2         /* x0 = EL number (1, 2, or 3) */

    cmp x0, #3
    b.eq _el3_to_el1        /* came in at EL3 — drop to EL1 via EL2 */
    cmp x0, #2
    b.eq _el2_to_el1        /* came in at EL2 — drop to EL1 */
    /* else: already at EL1, continue */
    b _el1_start


/* ─── EL3 → EL2 → EL1 ─────────────────────────────────────────────────── */
_el3_to_el1:
    /*
     * SCR_EL3 — Secure Configuration Register
     * Bit 10 (RW)  = 1  → EL2 is AArch64 (not 32-bit)
     * Bit  0 (NS)  = 1  → Non-secure world
     */
    mov x0, #(1 << 10)
    orr x0, x0, #(1 << 0)
    msr SCR_EL3, x0

    /*
     * SPSR_EL3 — Saved Program Status Register for EL3
     * This is the CPU state we will "restore" when we eret.
     * 0x3c9 = EL2h (use SP_EL2) with DAIF (all interrupts) masked.
     */
    mov x0, #0x3c9
    msr SPSR_EL3, x0

    /* ELR_EL3 — Exception Link Register: where eret will jump */
    adr x0, _el2_to_el1
    msr ELR_EL3, x0

    /*
     * eret — Exception Return.
     * Restores SPSR_EL3 into PSTATE, jumps to ELR_EL3.
     * Like IRET in x86 — pops flags and IP, but across privilege levels.
     */
    eret

_el2_to_el1:
    /*
     * HCR_EL2 — Hypervisor Configuration Register
     * Bit 31 (RW) = 1 → EL1 is AArch64 (not 32-bit)
     */
    mov x0, #(1 << 31)
    msr HCR_EL2, x0

    /* Drop to EL1 with interrupts still masked */
    mov x0, #0x3c5          /* EL1h + DAIF masked */
    msr SPSR_EL2, x0

    adr x0, _el1_start
    msr ELR_EL2, x0
    eret


/* ─── EL1 initialisation ───────────────────────────────────────────────── */
_el1_start:
    /*
     * Multi-core: only CPU 0 initialises the system.
     * The other cores spin here until we wake them later (Phase 2+).
     *
     * MPIDR_EL1 = Multiprocessor Affinity Register
     * Bits [7:0] = CPU ID (Aff0 field).
     *
     * cbnz = "Compare and Branch if Not Zero" (like JNEZ / JNE in x86)
     */
    mrs x0, MPIDR_EL1
    and x0, x0, #0xFF       /* keep only CPU ID bits */
    cbnz x0, _secondary_start  /* CPUs 1-3: SMP secondary entry */

    /*
     * Switch to SP_EL1 (dedicated kernel stack pointer).
     *
     * AArch64 has two stack pointers per EL:
     *   SP_EL0  — "user" stack pointer (SPSel=0)
     *   SP_EL1  — "kernel" stack pointer (SPSel=1, the "h" in EL1h)
     *
     * QEMU may enter EL1 in "t" mode (SPSel=0, using SP_EL0).
     * We explicitly switch to SP_EL1 before touching the stack.
     * After this instruction, "sp" refers to SP_EL1.
     */
    msr SPSel, #1

    /*
     * Set up the stack pointer (now SP_EL1).
     * __stack_top is defined in linker.ld — the high end of our 64KB stack.
     * ldr x0, =symbol  loads the absolute address (like MOV AX, OFFSET var)
     * Stack grows DOWNWARD: first push → sp -= 8, then write.
     */
    ldr x0, =__stack_top
    mov sp, x0

    /*
     * Zero the BSS section.
     * BSS = uninitialised global variables. The C standard says they must
     * start as zero. Without an OS to zero them, we do it ourselves.
     *
     * __bss_start and __bss_end are symbols from linker.ld.
     *
     * xzr = the "zero register" — always reads as 0 (no x86 equivalent,
     *        but think of it as MOV [addr], 0 without needing a scratch reg)
     */
    ldr x0, =__bss_start
    ldr x1, =__bss_end
    sub x1, x1, x0          /* x1 = byte count to zero */

_zero_bss:
    cbz x1, _bss_done       /* cbz = jump if x1 == 0 (like JCXZ) */
    str xzr, [x0], #8       /* store 0 at [x0], then x0 += 8 (post-index) */
    sub x1, x1, #8
    b   _zero_bss
_bss_done:

    /*
     * Enable FP/SIMD access from EL0.
     * CPACR_EL1.FPEN [21:20] = 0b11  → no trapping at EL0 or EL1.
     * Reset value is 0b00 (trap everything); the kernel uses
     * -mgeneral-regs-only so it never touches FP registers itself,
     * but userspace processes need them for any double-precision math.
     * EC=0x07 ("Trapped SIMD/FP access") is the fault if we skip this.
     */
    mov x0, #(3 << 20)
    msr CPACR_EL1, x0
    isb

    /*
     * Jump to the C kernel entry point.
     * bl = Branch with Link = CALL in 8086.
     * Return address is stored in x30 (link register).
     * kernel_main is defined in kernel/core/main.c
     */
    bl kernel_main

    /*
     * kernel_main should never return. If it does, fall through to idle.
     */

/* ─── Core 0 fallback idle (kernel_main should never return) ────────────── */
_cpu_idle:
    wfe
    b _cpu_idle

/* ─── Secondary core entry point ────────────────────────────────────────── */
/*
 * _secondary_start — entered by cores 1-3 via PSCI CPU_ON.
 *
 * PSCI delivers secondary cores at EL2 or EL1 depending on firmware.
 * We handle both cases, mirroring the EL2→EL1 drop done for core 0.
 *
 * Steps:
 *   1. Drop from EL2 to EL1 if needed (same as primary boot path).
 *   2. Enable FP/SIMD at EL1 (CPACR_EL1.FPEN = 0b11).
 *   3. Switch to SP_EL1.
 *   4. Read core_id from MPIDR_EL1[7:0].
 *   5. Load the per-core stack top from g_secondary_stack_tops[core_id].
 *   6. Call secondary_main(core_id) in C.
 *
 * Exposed as secondary_boot_entry so smp.c can pass its address to PSCI.
 */
.global secondary_boot_entry
secondary_boot_entry:
_secondary_start:
    /* ── 1. EL check — drop to EL1 if we arrived at EL2 ── */
    mrs x0, CurrentEL
    lsr x0, x0, #2         /* x0 = EL number */
    cmp x0, #2
    b.eq _secondary_el2_drop
    b   _secondary_el1      /* already at EL1 */

_secondary_el2_drop:
    /* Configure HCR_EL2 to make EL1 run as AArch64 */
    mov x0, #(1 << 31)     /* RW bit: EL1 is AArch64 */
    msr HCR_EL2, x0

    /* Drop to EL1h with all interrupts masked */
    mov x0, #0x3c5          /* EL1h (SPSel=1) + DAIF masked */
    msr SPSR_EL2, x0
    adr x0, _secondary_el1
    msr ELR_EL2, x0
    eret

_secondary_el1:
    /* ── 2. Enable FP/SIMD — required for user-space float operations ── */
    mov x0, #(3 << 20)     /* CPACR_EL1.FPEN = 0b11 */
    msr CPACR_EL1, x0
    isb

    /* ── 3. Switch to SP_EL1 (dedicated kernel stack pointer) ── */
    msr SPSel, #1

    /* ── 4. Get our core ID from MPIDR_EL1 [7:0] ── */
    mrs x0, MPIDR_EL1
    and x0, x0, #0xFF       /* x0 = core_id (1, 2, or 3) */

    /*
     * ── 5. Load per-core stack top ──
     * g_secondary_stack_tops is a u64[4] filled by smp_init() before the
     * PSCI calls.  g_secondary_stack_tops[core_id] is the stack top address.
     * ldr x1, =symbol loads the symbol's address (like MOV AX, OFFSET var).
     * [x1, x0, lsl #3] = *(x1 + core_id * 8) — one 64-bit entry per core.
     */
    ldr x1, =g_secondary_stack_tops
    ldr x2, [x1, x0, lsl #3]  /* x2 = g_secondary_stack_tops[core_id] */
    mov sp, x2                 /* sp cannot be a direct ldr destination   */

    /*
     * ── 6. Call secondary_main(core_id) ──
     * x0 still holds core_id — AArch64 calling convention passes first
     * argument in x0 (equivalent to pushing it on the stack in x86).
     */
    bl secondary_main

    /* secondary_main() never returns — park the core for safety */
1:  wfi
    b   1b
