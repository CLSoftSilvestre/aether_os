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
#include "drivers/input/virtio_input.h"
#include "drivers/usb/ohci.h"
#include "drivers/power/cpufreq.h"
#include "drivers/power/thermal.h"
#include "drivers/power/dpms.h"
#include "aether/net.h"
#include "aether/printk.h"
#include "aether/scheduler.h"
#include "aether/smp.h"

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
 * timer_secondary_init — enable the per-CPU timer on a secondary core.
 *
 * The ARM Generic Timer (CNTP) is per-CPU: each core has its own
 * CNTP_TVAL_EL0 and CNTP_CTL_EL0.  GICv2 PPI 30 is also banked per CPU,
 * so gic_enable_irq(30) from here affects only the calling core.
 *
 * The timer fires at the same TIMER_HZ rate as core 0, waking the secondary
 * idle loop from WFI so it can call task_yield() and pick up new work.
 * The full timer_irq_handler() tick accounting runs only on core 0.
 */
void timer_secondary_init(void)
{
    u64 freq = read_cntfrq();
    if (!freq) freq = 62500000ULL;
    u64 interval = freq / TIMER_HZ;

    /* Enable PPI 30 for this core only (GICv2 banked register) */
    gic_enable_irq(TIMER_IRQ_ID);

    /* Program first countdown and enable */
    write_cntp_tval(interval);
    write_cntp_ctl(CNTP_CTL_ENABLE);
    __asm__ volatile("isb" ::: "memory");
}

/*
 * timer_irq_handler — called from el1_irq_handler when IRQ ID == 30.
 *
 * Must re-arm the timer by rewriting CNTP_TVAL_EL0, otherwise only
 * one interrupt fires. (The hardware clears TVAL to 0 when it fires.)
 */
void timer_irq_handler(void)
{
    /* Re-arm this core's timer regardless of which core we're on */
    write_cntp_tval(g_interval);

    /*
     * Only core 0 does global tick accounting and peripheral polling.
     * Secondary cores just keep their timer armed so WFI wakes the idle loop,
     * allowing task_yield() to distribute runnable tasks to all cores.
     */
    if (cpu_id() != 0)
        return;

    g_ticks++;

    virtio_input_poll();

    /* USB: record input activity for DPMS and autosuspend */
    if (usb_hid_get_activity()) {
        dpms_activity();
        usb_autosuspend_activity();
    }

    usb_hid_poll();
    net_rx_poll();

    /* 1 Hz power management tick */
    if ((g_ticks % TIMER_HZ) == 0) {
        /* Feed the ondemand governor a true 4-core busy figure derived from
         * the scheduler's CNTPCT run-time accounting (load%, out of 100). */
        u32 load = scheduler_cpu_load();
        cpufreq_sample(100u - load, 100u);

        thermal_tick();
        dpms_tick();
        usb_autosuspend_tick();
    }
}

u64 timer_get_ticks(void)
{
    return g_ticks;
}

u64 timer_get_freq(void)
{
    return read_cntfrq();
}

/*
 * timer_seed_from_cntpct — called once, just before enabling IRQs.
 * Seeds g_ticks with the number of 100 Hz ticks elapsed since the
 * physical counter started (i.e. since QEMU launched), so that the
 * uptime visible to userspace counts from power-on, not from the
 * moment IRQs were enabled (which is after all boot work finishes).
 */
void timer_seed_from_cntpct(void)
{
    u64 freq = read_cntfrq();
    if (!freq) freq = 62500000ULL;
    u64 cntpct;
    __asm__ volatile("mrs %0, CNTPCT_EL0" : "=r"(cntpct));
    g_ticks = cntpct * (u64)TIMER_HZ / freq;
}
