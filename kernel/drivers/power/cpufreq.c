/*
 * AetherOS — CPU Frequency Scaling Driver (Phase 6.2.1)
 * File: kernel/drivers/power/cpufreq.c
 *
 * Ondemand governor logic:
 *   The timer ISR counts idle vs total ticks over a 1-second window and
 *   calls cpufreq_sample().  We compute load_pct and raise or lower the
 *   ARM CPU clock via the VideoCore mailbox (BCM2711) or track it in
 *   simulation (QEMU).
 *
 *   Hysteresis prevents thrashing: only switch freq when the new target
 *   differs from the current setting.
 */

#include "drivers/power/cpufreq.h"
#include "drivers/gpu/mailbox.h"
#include "aether/printk.h"
#include "aether/types.h"

static int  g_governor   = CPUFREQ_GOV_ONDEMAND;
static u32  g_min_hz     = CPUFREQ_FAKE_MIN_HZ;
static u32  g_max_hz     = CPUFREQ_FAKE_MAX_HZ;
static u32  g_current_hz = CPUFREQ_FAKE_MAX_HZ;
static bool g_hw_present = false;
static bool g_throttled  = false;

/* ── Internal helpers ──────────────────────────────────────────────────── */

static void apply_freq(u32 hz)
{
    if (hz == g_current_hz) return;
    if (g_hw_present)
        mailbox_set_clock_rate(MBOX_CLK_ARM, hz);
    g_current_hz = hz;
    kinfo("[cpufreq] ARM clock → %u MHz\n", hz / 1000000u);
}

/* ── Public API ────────────────────────────────────────────────────────── */

void cpufreq_init(void)
{
    u32 min_hz = mailbox_get_min_clock_rate(MBOX_CLK_ARM);
    u32 max_hz = mailbox_get_max_clock_rate(MBOX_CLK_ARM);
    u32 cur_hz = mailbox_get_clock_rate(MBOX_CLK_ARM);

    if (max_hz && min_hz) {
        g_hw_present = true;
        g_min_hz     = min_hz;
        g_max_hz     = max_hz;
        g_current_hz = cur_hz ? cur_hz : max_hz;
        kinfo("[cpufreq] Pi 4 hardware: min=%u MHz  max=%u MHz  cur=%u MHz\n",
              g_min_hz / 1000000u, g_max_hz / 1000000u, g_current_hz / 1000000u);
    } else {
        /* QEMU: simulate a 600–1500 MHz range; bookkeeping still works */
        kinfo("[cpufreq] QEMU mode — simulated %u MHz (ondemand active)\n",
              g_max_hz / 1000000u);
    }
}

void cpufreq_set_governor(int gov)
{
    g_governor = gov;
    if (gov == CPUFREQ_GOV_PERFORMANCE)
        apply_freq(g_max_hz);
    else if (gov == CPUFREQ_GOV_POWERSAVE)
        apply_freq(g_min_hz);
    /* ONDEMAND: next sample() call will decide */
}

int  cpufreq_get_governor(void)    { return g_governor; }
u32  cpufreq_get_current_hz(void)  { return g_current_hz; }
u32  cpufreq_get_min_hz(void)      { return g_min_hz; }
u32  cpufreq_get_max_hz(void)      { return g_max_hz; }

void cpufreq_thermal_cap(bool throttle)
{
    g_throttled = throttle;
    if (throttle)
        apply_freq(g_min_hz);
    /* On unthrottle: let the governor pick up naturally next sample */
}

void cpufreq_sample(u32 idle_ticks, u32 total_ticks)
{
    if (g_governor != CPUFREQ_GOV_ONDEMAND) return;
    if (g_throttled) return;
    if (!total_ticks) return;

    u32 load_pct = ((total_ticks - idle_ticks) * 100u) / total_ticks;

    if (load_pct >= CPUFREQ_BOOST_LOAD_PCT)
        apply_freq(g_max_hz);
    else if (load_pct < CPUFREQ_IDLE_LOAD_PCT)
        apply_freq(g_min_hz);
    /* between thresholds: hold current frequency */
}
