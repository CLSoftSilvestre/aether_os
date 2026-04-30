#ifndef _AETHER_SETJMP_H
#define _AETHER_SETJMP_H

/* AArch64 setjmp/longjmp — saves x19-x30 + sp (13 regs × 8 = 104 bytes).
   14 slots for 16-byte alignment. */
typedef unsigned long jmp_buf[14];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif /* _AETHER_SETJMP_H */
