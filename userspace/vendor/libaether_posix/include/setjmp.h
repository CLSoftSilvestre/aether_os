#ifndef _POSIX_SETJMP_H
#define _POSIX_SETJMP_H

/* AArch64: x19-x30 (12), sp (1), d8-d15 (8 × 8-byte pairs = 8 longs each)
 * Total: 12 GP + 1 SP + 8 FP = 21 × 8 = 168 bytes → round up to 32 longs */
typedef unsigned long jmp_buf[32];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* POSIX sigsetjmp/siglongjmp: no signal mask support, maps to setjmp/longjmp */
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(env, savesigs) setjmp(env)
#define siglongjmp(env, val)     longjmp(env, val)

#endif /* _POSIX_SETJMP_H */
