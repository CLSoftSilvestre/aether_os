/*
 * AetherOS — Kernel Entry Point
 * File: kernel/core/main.c
 *
 * kernel_main() is called from boot.S after:
 *   - CPU is in EL1 (kernel mode), SP_EL1 active
 *   - Stack is set up at __stack_top
 *   - BSS is zeroed
 *
 * Milestone 1.2 initialisation sequence:
 *   printk → exceptions → GIC → timer → idle (waiting for timer IRQs)
 */

#include "aether/printk.h"
#include "aether/exceptions.h"
#include "drivers/char/uart_pl011.h"
#include "drivers/irq/gic_v2.h"
#include "drivers/timer/arm_timer.h"
#include "aether/types.h"

extern u8 __bss_start[];
extern u8 __bss_end[];
extern u8 __kernel_end[];
extern u8 __stack_top[];

__attribute__((noreturn))
void kernel_main(void)
{
    /* ── Stage 1: UART (no printk yet, output directly) ─────────────── */
    uart_init();
    uart_puts("\r\n");
    uart_puts("╔══════════════════════════════════════╗\r\n");
    uart_puts("║         AetherOS v0.0.1              ║\r\n");
    uart_puts("║   Milestone 1.2 — Kernel Core        ║\r\n");
    uart_puts("╚══════════════════════════════════════╝\r\n");
    uart_puts("\r\n");

    /* ── Stage 2: printk (UART is up, structured logging from here) ── */
    kinfo("UART initialised\n");
    kinfo("Kernel loaded at 0x40000000\n");
    kinfo("BSS:        %p – %p (%lu bytes)\n",
          (void *)__bss_start, (void *)__bss_end,
          (unsigned long)(__bss_end - __bss_start));
    kinfo("Kernel end: %p\n", (void *)__kernel_end);
    kinfo("Stack top:  %p\n", (void *)__stack_top);

    /* ── Stage 3: Exception vector table ────────────────────────────── */
    /*
     * Install VBAR_EL1 so the CPU knows where to jump for every exception.
     * Without this, any IRQ or fault will jump to address 0x0 and hang.
     *
     * Must be done BEFORE enabling any interrupts.
     */
    exceptions_init();

    /* ── Stage 4: GIC — interrupt controller ────────────────────────── */
    /*
     * The GIC routes peripheral interrupts (timer, UART, etc.) to CPUs.
     * Must be initialised before enabling individual device IRQs.
     */
    gic_init();

    /* ── Stage 5: ARM Generic Timer ─────────────────────────────────── */
    /*
     * Sets up a 100 Hz periodic interrupt (IRQ 30).
     * After timer_init(), the GIC will start delivering IRQs every 10 ms.
     */
    timer_init();

    /* ── Stage 6: Enable IRQs ────────────────────────────────────────── */
    /*
     * Up to this point, IRQs have been masked in PSTATE (the DAIF bits).
     * DAIF = Debug, Asynchronous abort, IRQ, FIQ — all masked at boot.
     *
     * msr daifclr, #2  clears the 'I' (IRQ mask) bit → IRQs are now live.
     *
     * In x86 terms: this is the STI (Set Interrupt Flag) instruction.
     *
     * After this line, every 10 ms the CPU will:
     *   1. Finish the current instruction
     *   2. Save PC and PSTATE to ELR_EL1 / SPSR_EL1
     *   3. Jump to vector_table + 0x280 (EL1 IRQ handler)
     *   4. Our _el1_irq stub saves all registers
     *   5. Calls el1_irq_handler() → gic_acknowledge() → timer_irq_handler()
     *   6. Returns, restores registers, eret back to the instruction after wfi
     */
    kinfo("Enabling IRQs...\n");
    __asm__ volatile("msr daifclr, #2" ::: "memory");
    kinfo("IRQs enabled — timer running at %d Hz\n", TIMER_HZ);
    kinfo("\n");
    kinfo("Entering idle loop. Watch for timer ticks below.\n");
    kinfo("─────────────────────────────────────────────\n");

    /* ── Idle loop ───────────────────────────────────────────────────── */
    /*
     * wfi = Wait For Interrupt — the CPU sleeps until an IRQ fires.
     * Each timer tick wakes the CPU, runs the handler, then returns here.
     *
     * This is Phase 1's "scheduler" — one task, the idle task.
     * The real scheduler arrives in Milestone 1.2 (round-robin, Phase 2).
     */
    for (;;) {
        __asm__ volatile("wfi");
    }
}
