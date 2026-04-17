/*
 * AetherOS — Kernel Entry Point
 * File: kernel/core/main.c
 *
 * Phase 2 initialisation sequence:
 *   UART → printk → exceptions → PMM → kmalloc → GIC → timer
 *   → scheduler → tasks → enable IRQs → idle loop
 */

#include "aether/printk.h"
#include "aether/exceptions.h"
#include "aether/mm.h"
#include "aether/kmalloc.h"
#include "aether/scheduler.h"
#include "drivers/char/uart_pl011.h"
#include "drivers/irq/gic_v2.h"
#include "drivers/timer/arm_timer.h"
#include "aether/types.h"

extern u8 __stack_top[];

/* ── Kernel tasks ────────────────────────────────────────────────────────── */

/*
 * task_monitor — a kernel thread that wakes every second and prints a
 * system status report. Shows the scheduler is alive and context switching
 * between the idle task and this task.
 */
static void task_monitor(void)
{
    u64 iteration = 0;

    kinfo("[monitor] started on PID %lu\n",
          (unsigned long)task_current_pid());

    while (1) {
        iteration++;
        kinfo("[monitor] iteration %lu | ticks=%lu | free pages=%lu\n",
              (unsigned long)iteration,
              (unsigned long)timer_get_ticks(),
              (unsigned long)pmm_free_pages());

        if (iteration == 1) {
            /* Print task list on first run to show the scheduler state */
            scheduler_print_tasks();
        }

        /* Sleep for 2 seconds (200 ticks at 100Hz) then yield */
        task_sleep(200);
    }
}

/*
 * task_counter — a kernel thread that counts slowly and yields between
 * each step, demonstrating fine-grained cooperative scheduling.
 */
static void task_counter(void)
{
    kinfo("[counter] started on PID %lu\n",
          (unsigned long)task_current_pid());

    for (u32 i = 1; i <= 10; i++) {
        kinfo("[counter] count = %lu\n", (unsigned long)i);
        task_sleep(50);   /* sleep 500ms between counts */
    }

    kinfo("[counter] done — exiting\n");
    task_exit();
}

/* ── Kernel entry ────────────────────────────────────────────────────────── */

__attribute__((noreturn))
void kernel_main(void)
{
    /* ── 1. UART — first, so we can see output immediately ─────────── */
    uart_init();
    uart_puts("\r\n");
    uart_puts("╔══════════════════════════════════════╗\r\n");
    uart_puts("║         AetherOS v0.0.2              ║\r\n");
    uart_puts("║        Phase 2 — Memory + Tasks      ║\r\n");
    uart_puts("╚══════════════════════════════════════╝\r\n");
    uart_puts("\r\n");

    /* ── 2. Exception vector table ──────────────────────────────────── */
    /*
     * Must come before anything that could trigger an exception
     * (including printk, which does MMIO writes to the UART).
     * In practice it works without this, but it is the correct order.
     */
    exceptions_init();

    /* ── 3. Physical Memory Manager ─────────────────────────────────── */
    /*
     * Discovers free RAM and builds the page bitmap.
     * Everything after this can allocate physical pages.
     */
    pmm_init();
    pmm_print_stats();

    /* ── 4. Kernel heap ──────────────────────────────────────────────── */
    /*
     * Sets up the first-fit free-list allocator on top of the PMM.
     * After this, kmalloc()/kfree() are available.
     */
    kmalloc_init();

    /* ── 5. GIC + Timer ─────────────────────────────────────────────── */
    gic_init();
    timer_init();

    /* ── 6. Scheduler ───────────────────────────────────────────────── */
    /*
     * Register the current execution context (kernel_main) as the idle
     * task. It will run whenever no other task is ready.
     */
    scheduler_init();
    scheduler_add_idle();

    /* Create two kernel threads */
    task_create(task_monitor, "monitor");
    task_create(task_counter, "counter");

    /* ── 7. Enable IRQs ─────────────────────────────────────────────── */
    /*
     * From this point forward, the timer fires every 10ms.
     * The timer handler increments g_ticks, which task_sleep() uses
     * to decide when to wake sleeping tasks.
     *
     * IRQ unmasking must happen AFTER the GIC and vector table are ready.
     */
    kinfo("Enabling IRQs — entering idle loop\n");
    kinfo("────────────────────────────────────────────\n");
    __asm__ volatile("msr daifclr, #2" ::: "memory");

    /* ── 8. Idle loop ───────────────────────────────────────────────── */
    /*
     * The idle task: sleep until an IRQ wakes us, then yield to let
     * any newly-ready tasks run.
     *
     * wfi (Wait For Interrupt) is the ARM equivalent of x86 HLT —
     * it halts the CPU core in a low-power state until an interrupt fires.
     *
     * After wfi returns (a timer IRQ fired):
     *   1. The IRQ handler incremented g_ticks
     *   2. task_yield() checks if any sleeping task's wake_tick has passed
     *   3. If yes, that task's state → READY and we switch to it
     *   4. The task runs, eventually sleeps again, and we return here
     */
    while (1) {
        __asm__ volatile("wfi");
        task_yield();
    }
}
