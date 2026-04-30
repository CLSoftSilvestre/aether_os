/*
 * AetherOS — Userspace C Runtime Entry Point
 * File: userspace/lib/crt0.S
 *
 * The kernel calls launch_el0(entry, stack) which eret's to _start with:
 *   PC  = 0x70000000  (_start)
 *   SP  = 0x7FFFF000  (VMM_USER_STACK_TOP)
 *   EL0 exception level, all interrupts unmasked
 *
 * For processes spawned with SYS_SPAWN_ARGS, the kernel sets:
 *   x0 = argc, x1 = user-VA of argv[] pointer array
 * For plain SYS_SPAWN, x0=0 and x1=0 (no args).
 *
 * This stub:
 *   1. Zeros the BSS segment (required by the C standard)
 *   2. Preserves x0 (argc) and x1 (argv) across BSS zero
 *   3. Calls main(argc, argv)
 *   4. Passes main's return value to sys_exit()
 */

.section .text.boot
.global _start

_start:
    /* ── 1. Preserve argc (x0) and argv (x1) across BSS zero ─────── */
    mov  x19, x0         /* callee-saved: argc */
    mov  x20, x1         /* callee-saved: argv */

    /* ── 2. Zero BSS ───────────────────────────────────────────────── */
    /* Use adrp+add (4 GB range) — binary may be >1 MB with Lua VM */
    adrp x0, __bss_start
    add  x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add  x1, x1, :lo12:__bss_end
    cmp  x0, x1
    b.ge .Lbss_done
.Lbss_loop:
    str  xzr, [x0], #8
    cmp  x0, x1
    b.lt .Lbss_loop
.Lbss_done:

    /* ── 3. Call main(argc, argv) ──────────────────────────────────── */
    mov  x0, x19         /* argc */
    mov  x1, x20         /* argv */
    bl   main

    /* ── 4. sys_exit(return_value) ─────────────────────────────────── */
    /* x0 already holds main's return value */
    mov  x8, #0          /* SYS_EXIT = 0 */
    svc  #0

    /* Should never reach here */
.Lhalt:
    b    .Lhalt
