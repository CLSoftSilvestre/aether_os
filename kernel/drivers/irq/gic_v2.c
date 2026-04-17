/*
 * ARM GICv2 Driver — AetherOS
 * File: kernel/drivers/irq/gic_v2.c
 *
 * The GIC routes hardware interrupts to CPUs — analogous to the 8259 PIC
 * in x86, but for ARM systems. Every interrupt must be:
 *   1. Enabled in the Distributor (GICD)
 *   2. Acknowledged by the CPU Interface (GICC_IAR) when it fires
 *   3. Signalled complete via GICC_EOIR after handling
 *
 * If you forget step 3, the GIC thinks the IRQ is still in-service and
 * will not deliver another one of the same ID — a common IRQ bug.
 */

#include "drivers/irq/gic_v2.h"
#include "aether/printk.h"

/* ── MMIO helpers ───────────────────────────────────────────────────────── */

static inline u32 gicd_read(u32 offset)
{
    return MMIO_READ32(GICD_BASE + offset);
}

static inline void gicd_write(u32 offset, u32 value)
{
    MMIO_WRITE32(GICD_BASE + offset, value);
}

static inline u32 gicc_read(u32 offset)
{
    return MMIO_READ32(GICC_BASE + offset);
}

static inline void gicc_write(u32 offset, u32 value)
{
    MMIO_WRITE32(GICC_BASE + offset, value);
}

/* ── Distributor init ───────────────────────────────────────────────────── */

void gic_init(void)
{
    /* Step 1 — disable distributor while we configure it */
    gicd_write(GICD_CTLR, 0);

    /*
     * GICD_TYPER bits [4:0] = ITLinesNumber.
     * Total interrupts = (ITLinesNumber + 1) × 32.
     * We iterate over all supported interrupt lines.
     */
    u32 typer    = gicd_read(GICD_TYPER);
    u32 num_regs = (typer & 0x1F) + 1;   /* number of 32-bit ISENABLER regs */

    /*
     * Disable all interrupts in the distributor.
     * ICENABLER[n]: writing 1 to bit N disables interrupt N.
     * Each register covers 32 interrupts.
     */
    for (u32 i = 0; i < num_regs; i++)
        gicd_write(GICD_ICENABLER + i * 4, 0xFFFFFFFF);

    /*
     * Set all interrupt priorities to 0xA0 (medium — lower number = higher
     * priority in GIC). IPRIORITYR is a byte array, one byte per interrupt.
     * We write 4 bytes at a time.
     */
    u32 num_irqs = num_regs * 32;
    for (u32 i = 0; i < num_irqs / 4; i++)
        gicd_write(GICD_IPRIORITYR + i * 4, 0xA0A0A0A0);

    /*
     * Route all SPI interrupts (ID >= 32) to CPU 0.
     * ITARGETSR is a byte array; each byte is a CPU bitmask (bit 0 = CPU 0).
     * PPIs (16-31) are banked per-CPU — their ITARGETSR entries are read-only.
     */
    for (u32 i = 8; i < num_irqs / 4; i++)   /* start at SPI 0 (offset 32/4=8) */
        gicd_write(GICD_ITARGETSR + i * 4, 0x01010101);

    /*
     * Configure all interrupts as level-sensitive (not edge-triggered).
     * ICFGR: 2 bits per interrupt, bit 1 = edge (1) or level (0).
     */
    for (u32 i = 0; i < num_irqs / 16; i++)
        gicd_write(GICD_ICFGR + i * 4, 0);

    /* Data Synchronization Barrier — ensure all writes reach the GIC */
    __asm__ volatile("dsb sy" ::: "memory");

    /* Step 2 — enable distributor */
    gicd_write(GICD_CTLR, 1);

    /* ── CPU Interface ── */

    /*
     * GICC_PMR — Priority Mask Register.
     * Any interrupt with priority numerically LOWER than this value gets
     * delivered. 0xFF = accept all priorities.
     * (Lower priority number = higher priority in GIC, counter-intuitive!)
     */
    gicc_write(GICC_PMR, 0xFF);

    /* Binary Point Register: no grouping (all priority bits matter) */
    gicc_write(GICC_BPR, 0);

    /* Enable the CPU interface */
    gicc_write(GICC_CTLR, 1);

    kinfo("GICv2 initialised — %lu IRQs supported\n", (unsigned long)num_irqs);
}

/* ── Per-interrupt control ──────────────────────────────────────────────── */

/*
 * gic_enable_irq — enable delivery of one interrupt to this CPU.
 *
 * ISENABLER is a bit array. IRQ n is controlled by:
 *   register index = n / 32
 *   bit position   = n % 32
 *
 * Writing 1 enables; writing 0 has no effect (use ICENABLER to disable).
 */
void gic_enable_irq(u32 irq_id)
{
    u32 reg = irq_id / 32;
    u32 bit = irq_id % 32;
    gicd_write(GICD_ISENABLER + reg * 4, 1u << bit);
}

void gic_disable_irq(u32 irq_id)
{
    u32 reg = irq_id / 32;
    u32 bit = irq_id % 32;
    gicd_write(GICD_ICENABLER + reg * 4, 1u << bit);
}

/* ── IRQ lifecycle (called from exception handler) ──────────────────────── */

/*
 * gic_acknowledge — read IAR to claim the highest-priority pending IRQ.
 *
 * This MUST be called at the start of the IRQ handler.
 * Returns the IRQ ID (or GIC_SPURIOUS_IRQ = 1023 if no real IRQ pending).
 *
 * After calling this, the interrupt is "active" in the GIC — no more
 * interrupts of this ID will be delivered until gic_end_of_interrupt().
 */
u32 gic_acknowledge(void)
{
    return gicc_read(GICC_IAR) & 0x3FF;   /* bits [9:0] = interrupt ID */
}

/*
 * gic_end_of_interrupt — signal that handling is complete.
 *
 * MUST be called after handling every IRQ, or the GIC will not deliver
 * another interrupt of the same ID. (Same concept as EOI in x86 PIC.)
 */
void gic_end_of_interrupt(u32 irq)
{
    gicc_write(GICC_EOIR, irq);
}
