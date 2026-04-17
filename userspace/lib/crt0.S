/*
 * AetherOS — Userspace C Runtime Entry Point
 * File: userspace/lib/crt0.S
 *
 * The kernel calls launch_el0(entry, stack) which eret's to _start with:
 *   PC  = 0x70000000  (_start)
 *   SP  = 0x7FFFF000  (VMM_USER_STACK_TOP)
 *   EL0 exception level, all interrupts unmasked
 *
 * This stub:
 *   1. Zeros the BSS segment (required by the C standard)
 *   2. Calls main()
 *   3. Passes main's return value to sys_exit()
 */

.section .text.boot
.global _start

_start:
    /* ── 1. Zero BSS ───────────────────────────────────────────────── */
    adr  x0, __bss_start
    adr  x1, __bss_end
    cmp  x0, x1
    b.ge .Lbss_done
.Lbss_loop:
    str  xzr, [x0], #8
    cmp  x0, x1
    b.lt .Lbss_loop
.Lbss_done:

    /* ── 2. Call main() ────────────────────────────────────────────── */
    mov  x0, #0          /* argc = 0 */
    mov  x1, #0          /* argv = NULL */
    bl   main

    /* ── 3. sys_exit(return_value) ─────────────────────────────────── */
    /* x0 already holds main's return value */
    mov  x8, #0          /* SYS_EXIT = 0 */
    svc  #0

    /* Should never reach here */
.Lhalt:
    b    .Lhalt
