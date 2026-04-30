/* AArch64 setjmp / longjmp for AetherOS freestanding userspace.
 *
 * jmp_buf layout (unsigned long[14]):
 *   [0..1]  x19, x20
 *   [2..3]  x21, x22
 *   [4..5]  x23, x24
 *   [6..7]  x25, x26
 *   [8..9]  x27, x28
 *   [10..11] x29 (FP), x30 (LR)
 *   [12]    sp
 */

        .text

        .global setjmp
        .type   setjmp, %function
setjmp:
        stp  x19, x20, [x0, #0]
        stp  x21, x22, [x0, #16]
        stp  x23, x24, [x0, #32]
        stp  x25, x26, [x0, #48]
        stp  x27, x28, [x0, #64]
        stp  x29, x30, [x0, #80]
        mov  x2, sp
        str  x2,       [x0, #96]
        mov  x0, #0
        ret

        .global longjmp
        .type   longjmp, %function
longjmp:
        ldp  x19, x20, [x0, #0]
        ldp  x21, x22, [x0, #16]
        ldp  x23, x24, [x0, #32]
        ldp  x25, x26, [x0, #48]
        ldp  x27, x28, [x0, #64]
        ldp  x29, x30, [x0, #80]
        ldr  x2,       [x0, #96]
        mov  sp, x2
        /* return val, but never 0 (longjmp(env, 0) returns 1) */
        cmp  x1, #0
        csinc x0, x1, xzr, ne
        ret
