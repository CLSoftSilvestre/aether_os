/*
 * ARM Generic Timer Driver — AetherOS
 * File: kernel/drivers/timer/arm_timer.c
 *
 * The ARM Generic Timer is a per-CPU 64-bit counter that increments at a
 * fixed frequency (CNTFRQ_EL0). We use it to generate a periodic interrupt
 * at TIMER_HZ (100 Hz = 10 ms per tick) by programming CNTP_TVAL_EL0.
 *
 * Every time the timer fires:
 *   1. el1_irq_handler sees IRQ ID 30 from the GIC
 *   2. Calls timer_irq_handler()
 *   3. We reload the countdown and increment the tick counter
 *
 * The tick counter is the basis for all future timekeeping:
 *   - Scheduler time slices (Phase 2)
 *   - sleep() / nanosleep() (Phase 3)
 *   - System clock (Phase 3+)
 *
 * System register access in AArch64 uses mrs/msr.
 * These are like x86 RDMSR/WRMSR but for ARM co-processor registers.
 */

#include "drivers/timer/arm_timer.h"
#include "drivers/irq/gic_v2.h"
#include "aether/printk.h"

/* Tick counter — incremented by timer_irq_handler() on every timer interrupt */
static volatile u64 g_ticks = 0;

/* Timer interval in counter ticks (set during init) */
static u64 g_interval = 0;

/* ── System register accessors ──────────────────────────────────────────── */
/*
 * We wrap mrs/msr in inline functions to keep the calling code readable.
 * "mrs x, reg" = Move from System Register to General register (like RDMSR)
 * "msr reg, x" = Move from General register to System Register (like WRMSR)
 */

static inline u64 read_cntfrq(void)
{
    u64 val;
    __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(val));
    return val;
}

static inline u64 read_cntpct(void)
{
    u64 val;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(val));
    return val;
}

static inline void write_cntp_tval(u64 val)
{
    __asm__ volatile("msr CNTP_TVAL_EL0, %0" :: "r"(val));
}

static inline void write_cntp_ctl(u32 val)
{
    __asm__ volatile("msr CNTP_CTL_EL0, %0" :: "r"((u64)val));
}

static inline u32 read_cntp_ctl(void)
{
    u64 val;
    __asm__ volatile("mrs %0, CNTP_CTL_EL0" : "=r"(val));
    return (u32)val;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void timer_init(void)
{
    u64 freq = read_cntfrq();
    if (freq == 0) {
        kwarn("timer: CNTFRQ_EL0 is 0 — defaulting to 62.5 MHz\n");
        freq = 62500000;
    }

    g_interval = freq / TIMER_HZ;

    kinfo("Timer: freq=%lu Hz, interval=%lu ticks (%d Hz)\n",
          (unsigned long)freq, (unsigned long)g_interval, TIMER_HZ);

    /* Enable IRQ 30 (PPI — Non-Secure EL1 Physical Timer) in the GIC */
    gic_enable_irq(TIMER_IRQ_ID);

    /* Program the first countdown */
    write_cntp_tval(g_interval);

    /*
     * CNTP_CTL_EL0:
     *   bit 0 (ENABLE) = 1 — start the timer
     *   bit 1 (IMASK)  = 0 — do NOT mask the IRQ (let it reach the GIC)
     */
    write_cntp_ctl(CNTP_CTL_ENABLE);

    /* isb ensures the timer control write takes effect before we return */
    __asm__ volatile("isb" ::: "memory");

    kinfo("Timer: started at %d Hz\n", TIMER_HZ);
}

/*
 * timer_irq_handler — called from el1_irq_handler when IRQ ID == 30.
 *
 * Must re-arm the timer by rewriting CNTP_TVAL_EL0, otherwise only
 * one interrupt fires. (The hardware clears TVAL to 0 when it fires.)
 */
void timer_irq_handler(void)
{
    g_ticks++;

    /* Reload the countdown for the next tick */
    write_cntp_tval(g_interval);

    /* nothing — heartbeat removed; use sys_get_ticks() from user space */
}

u64 timer_get_ticks(void)
{
    return g_ticks;
}

u64 timer_get_freq(void)
{
    return read_cntfrq();
}
