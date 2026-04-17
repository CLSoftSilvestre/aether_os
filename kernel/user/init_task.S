/*
 * AetherOS — First User-Space Program (init task)
 * File: kernel/user/init_task.S
 *
 * This is the very first program that runs at EL0 (user mode).
 * It is embedded directly in the kernel binary for Phase 3 MVP.
 * Phase 3.1 will load it from an ELF file on the initrd instead.
 *
 * What this program does:
 *   1. Calls sys_write(1, message, len) to print to the UART
 *   2. Calls sys_exit(0) to terminate cleanly
 *
 * Syscall ABI (AetherOS, matches Linux AArch64):
 *   x8  = syscall number
 *   x0  = arg0, x1 = arg1, x2 = arg2
 *   svc #0  — invoke kernel
 *   x0  = return value after syscall
 *
 * ═══════════════════════════════════════════════════════════
 * Why assembly instead of C?
 * ═══════════════════════════════════════════════════════════
 * The user-space C runtime (crt0, libaether) doesn't exist yet.
 * A C function requires:
 *   - A stack frame set up by crt0 (_start)
 *   - argc/argv passed correctly
 *   - A heap (malloc/free)
 *   - Standard library functions (printf, etc.)
 *
 * Assembly lets us make syscalls directly without any runtime support.
 * This is exactly how the Linux kernel tests its own EL0 entry point
 * before the C library is available.
 *
 * ═══════════════════════════════════════════════════════════
 * EL0 restrictions
 * ═══════════════════════════════════════════════════════════
 * Running at EL0, this code CANNOT:
 *   - Access device memory (MMIO at 0x0–0x3FFFFFFF) — AP=EL1_RW
 *   - Execute privileged instructions (mrs/msr with system regs)
 *   - Modify page tables or SCTLR_EL1
 *
 * It CAN:
 *   - Access normal RAM (0x40000000–0x7FFFFFFF) — AP=BOTH_RW
 *   - Call SVC #0 to request kernel services
 *   - Read/write its own stack and data
 */

.section ".text"
.global user_init_start
.global user_init_end

/*
 * user_init_start — entry point of the user process.
 * The kernel jumps here via `eret` after setting ELR_EL1 = user_init_start.
 */
user_init_start:

    /* ── syscall: sys_write(fd=1, message, length) ── */
    /*
     * adr x1, user_msg  computes the address of the string label
     * as a PC-relative offset. This works regardless of where the
     * binary is loaded (position-independent within ±1MB).
     *
     * Equivalent 8086: MOV DX, OFFSET message
     */
    mov  x8, #34            /* SYS_WRITE = 34                          */
    mov  x0, #1             /* fd = 1 (stdout)                         */
    adr  x1, user_msg       /* x1 = address of message string          */
    mov  x2, #32            /* length = 32 bytes (see string below)    */
    svc  #0                 /* invoke kernel — returns bytes written   */

    /* ── syscall: sys_exit(0) ── */
    mov  x8, #0             /* SYS_EXIT = 0                            */
    mov  x0, #0             /* exit code = 0 (success)                 */
    svc  #0                 /* invoke kernel — does not return         */

    /* Safety: should never reach here */
    b    .

/* ── Data ─────────────────────────────────────────────────────────── */
/*
 * This string lives in the .text section immediately after the code.
 * The CPU can read it from EL0 because the RAM block descriptor has
 * AP=BOTH_RW (EL0 read/write access to all of 0x40000000–0x7FFFFFFF).
 *
 * Must be exactly 32 bytes (matches x2=32 in sys_write call above).
 */
user_msg:
    .ascii "Hello from EL0! (AetherOS init)\n"

user_init_end:
    /* Marker so the kernel knows the size of the user image */
