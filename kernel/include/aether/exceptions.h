#ifndef AETHER_EXCEPTIONS_H
#define AETHER_EXCEPTIONS_H

#include "aether/types.h"

/*
 * AetherOS exception handling — AArch64
 *
 * When the CPU encounters an exception (IRQ, fault, syscall) it:
 *   1. Saves ELR_EL1 (interrupted PC) and SPSR_EL1 (interrupted state)
 *   2. Jumps to the vector table entry (VBAR_EL1 + offset)
 *   3. Our handler saves all general registers and calls a C function
 *   4. The C function handles the exception and returns
 *   5. We restore all registers and execute `eret` to resume
 *
 * The trap_frame_t struct mirrors exactly what exceptions.S pushes
 * onto the stack — the layout MUST match or register corruption occurs.
 */

/*
 * Trap frame — snapshot of CPU state at the time of exception.
 * Saved by the EXCEPTION_ENTRY macro in exceptions.S.
 *
 * Stack layout (sp points to x[0] after EXCEPTION_ENTRY):
 *   sp +   0: x[0]  ... x[29]   (240 bytes, 15 stp pairs)
 *   sp + 240: x[30] (LR)
 *   sp + 248: elr   (interrupted PC — "where we came from")
 *   sp + 256: spsr  (interrupted PSTATE — flags, EL, etc.)
 *   sp + 264: esr   (Exception Syndrome Register — "why we faulted")
 */
typedef struct {
    u64 x[30];    /* x0–x29                          offset   0 */
    u64 x30;      /* link register (return address)   offset 240 */
    u64 elr;      /* ELR_EL1: interrupted PC          offset 248 */
    u64 spsr;     /* SPSR_EL1: interrupted CPU state  offset 256 */
    u64 esr;      /* ESR_EL1: fault syndrome          offset 264 */
} __attribute__((packed)) trap_frame_t;

/*
 * ESR_EL1 field extraction.
 *
 * ESR layout:
 *   bits [31:26] = EC  (Exception Class  — what kind of exception)
 *   bit  [25]    = IL  (Instruction Length — 16 or 32 bit instruction)
 *   bits [24:0]  = ISS (Instruction Specific Syndrome — fault details)
 */
#define ESR_EC(esr)   (((esr) >> 26) & 0x3F)
#define ESR_ISS(esr)  ((esr) & 0x1FFFFFF)

/* Exception Class values */
#define EC_UNKNOWN    0x00   /* Unknown / uncategorised */
#define EC_WFI        0x01   /* WFI or WFE instruction */
#define EC_SVC64      0x15   /* SVC from AArch64 (system call) */
#define EC_IABORT_LOW 0x20   /* Instruction Abort from lower EL */
#define EC_IABORT_CUR 0x21   /* Instruction Abort from current EL */
#define EC_PC_ALIGN   0x22   /* PC alignment fault */
#define EC_DABORT_LOW 0x24   /* Data Abort from lower EL */
#define EC_DABORT_CUR 0x25   /* Data Abort from current EL */
#define EC_SP_ALIGN   0x26   /* SP alignment fault */
#define EC_BRK        0x3C   /* BRK instruction (software breakpoint) */

/* Install vector table into VBAR_EL1 and enable exception handling */
void exceptions_init(void);

/* C handlers — called from exceptions.S with trap_frame_t* in x0 */
void el1_sync_handler(trap_frame_t *frame);
void el1_irq_handler(trap_frame_t *frame);
void el1_serror_handler(trap_frame_t *frame);
void el0_sync_handler(trap_frame_t *frame);   /* Phase 3: syscalls */
void el0_irq_handler(trap_frame_t *frame);

#endif /* AETHER_EXCEPTIONS_H */
