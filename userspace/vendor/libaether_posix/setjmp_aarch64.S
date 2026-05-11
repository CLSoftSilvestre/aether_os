/* AArch64 setjmp / longjmp for libaether_posix.
 *
 * Saves all callee-saved GP registers (x19-x30), SP, and callee-saved
 * FP registers (d8-d15). libpng requires FP register save correctness.
 *
 * jmp_buf layout (long[32] = 256 bytes):
 *   offset   0 : x19
 *   offset   8 : x20
 *   ...
 *   offset  80 : x29 (FP)
 *   offset  88 : x30 (LR)
 *   offset  96 : sp
 *   offset 104 : d8
 *   ...
 *   offset 152 : d15
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
        stp  d8,  d9,  [x0, #104]
        stp  d10, d11, [x0, #120]
        stp  d12, d13, [x0, #136]
        stp  d14, d15, [x0, #152]
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
        ldp  d8,  d9,  [x0, #104]
        ldp  d10, d11, [x0, #120]
        ldp  d12, d13, [x0, #136]
        ldp  d14, d15, [x0, #152]
        cmp  x1, #0
        csinc x0, x1, xzr, ne   /* longjmp(env,0) must return 1 */
        ret
