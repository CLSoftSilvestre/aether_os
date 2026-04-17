#ifndef GIC_V2_H
#define GIC_V2_H

#include "aether/types.h"

/*
 * ARM GICv2 — Generic Interrupt Controller version 2
 *
 * The GIC is the ARM equivalent of the x86 PIC (8259) / APIC — it routes
 * hardware interrupts from peripherals to CPUs.
 *
 * Architecture:
 *   Distributor (GICD) — global; decides which CPU gets each interrupt
 *   CPU Interface (GICC) — per-CPU; CPU reads this to get its pending IRQ
 *
 * QEMU virt machine GICv2 base addresses:
 *   GICD: 0x08000000
 *   GICC: 0x08010000
 *
 * Interrupt ID ranges:
 *   0–15:   SGI  — Software Generated Interrupts (inter-CPU messages)
 *   16–31:  PPI  — Private Peripheral Interrupts (per-CPU, e.g. timer)
 *   32–1019:SPI  — Shared Peripheral Interrupts (devices, e.g. UART)
 */

/* Base addresses on QEMU virt */
#define GICD_BASE  0x08000000UL
#define GICC_BASE  0x08010000UL

/* ── Distributor registers ───────────────────────────────────────────── */
#define GICD_CTLR        0x000   /* Distributor Control Register */
#define GICD_TYPER       0x004   /* Interrupt Controller Type   */
#define GICD_ISENABLER   0x100   /* Interrupt Set-Enable (array, 1 bit/IRQ)  */
#define GICD_ICENABLER   0x180   /* Interrupt Clear-Enable                   */
#define GICD_IPRIORITYR  0x400   /* Interrupt Priority (array, 1 byte/IRQ)   */
#define GICD_ITARGETSR   0x800   /* Interrupt Processor Targets (1 byte/IRQ) */
#define GICD_ICFGR       0xC00   /* Interrupt Configuration (2 bits/IRQ)     */

/* ── CPU Interface registers ────────────────────────────────────────── */
#define GICC_CTLR  0x000   /* CPU Interface Control Register */
#define GICC_PMR   0x004   /* Interrupt Priority Mask (lower = stricter) */
#define GICC_BPR   0x008   /* Binary Point Register */
#define GICC_IAR   0x00C   /* Interrupt Acknowledge — read to claim IRQ */
#define GICC_EOIR  0x010   /* End Of Interrupt — write to release IRQ */

/* Special IAR value meaning "no pending interrupt" */
#define GIC_SPURIOUS_IRQ  1023

/* Public API */
void gic_init(void);
void gic_enable_irq(u32 irq_id);
void gic_disable_irq(u32 irq_id);
u32  gic_acknowledge(void);         /* read IAR — returns IRQ id */
void gic_end_of_interrupt(u32 irq); /* write EOIR */

#endif /* GIC_V2_H */
