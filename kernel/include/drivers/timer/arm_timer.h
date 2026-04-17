#ifndef ARM_TIMER_H
#define ARM_TIMER_H

#include "aether/types.h"

/*
 * ARM Generic Timer driver — AetherOS
 *
 * The ARM Generic Timer is a per-CPU countdown timer built into
 * every Cortex-A core. It is accessed via system registers (not MMIO).
 *
 * Think of it as a 64-bit free-running counter that ticks at a fixed
 * frequency (CNTFRQ_EL0, typically 62.5 MHz on QEMU). You program a
 * deadline, and when the counter reaches it, an IRQ fires.
 *
 * We use the Non-Secure Physical Timer (EL1 physical timer):
 *   CNTFRQ_EL0  — counter frequency in Hz (read-only, set by firmware)
 *   CNTPCT_EL0  — current 64-bit counter value
 *   CNTP_TVAL_EL0 — countdown: write N → IRQ fires after N ticks
 *   CNTP_CTL_EL0  — control: bit0=enable, bit1=IRQ mask, bit2=fired status
 *
 * IRQ: PPI 30 (Non-Secure EL1 Physical Timer) — always assigned to GIC IRQ 30.
 *
 * Target rate: 100 Hz (10 ms per tick) — standard for kernel scheduling.
 */

#define TIMER_IRQ_ID   30      /* GIC PPI 30 — Non-Secure EL1 Physical Timer */
#define TIMER_HZ       100     /* Timer frequency: ticks per second */

/* CNTP_CTL_EL0 bits */
#define CNTP_CTL_ENABLE   (1u << 0)   /* Enable the timer */
#define CNTP_CTL_IMASK    (1u << 1)   /* Mask the IRQ (1=masked, 0=unmasked) */
#define CNTP_CTL_ISTATUS  (1u << 2)   /* IRQ status (read-only, 1=fired) */

/* Public API */
void timer_init(void);
void timer_irq_handler(void);    /* call from el1_irq_handler when IRQ==30 */
u64  timer_get_ticks(void);      /* number of timer interrupts since boot */
u64  timer_get_freq(void);       /* counter frequency in Hz */

#endif /* ARM_TIMER_H */
