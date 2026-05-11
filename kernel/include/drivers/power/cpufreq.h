#ifndef DRIVERS_POWER_CPUFREQ_H
#define DRIVERS_POWER_CPUFREQ_H
/*
 * AetherOS — CPU Frequency Scaling (Phase 6.2.1)
 * File: kernel/include/drivers/power/cpufreq.h
 *
 * Three governor modes:
 *   PERFORMANCE — pin clock at max_hz; use when latency matters
 *   ONDEMAND    — raise to max when load > 70 %, drop to min when < 20 %
 *   POWERSAVE   — pin clock at min_hz; use for idle/thermal conditions
 *
 * On BCM2711 (Pi 4):  clock changes go through the VideoCore mailbox.
 * On QEMU -M virt:    no mailbox → simulated 600–1500 MHz; governor
 *                     bookkeeping still runs so userspace sees realistic values.
 */

#include "aether/types.h"

#define CPUFREQ_GOV_PERFORMANCE  0
#define CPUFREQ_GOV_ONDEMAND     1
#define CPUFREQ_GOV_POWERSAVE    2

/* Simulated clock limits when no mailbox hardware is present */
#define CPUFREQ_FAKE_MIN_HZ   600000000u   /*  600 MHz */
#define CPUFREQ_FAKE_MAX_HZ  1500000000u   /* 1500 MHz */

/* Ondemand thresholds (percent load over 1-second window) */
#define CPUFREQ_BOOST_LOAD_PCT   70u   /* above this → switch to max */
#define CPUFREQ_IDLE_LOAD_PCT    20u   /* below this → switch to min */

void cpufreq_init(void);
void cpufreq_set_governor(int gov);
int  cpufreq_get_governor(void);
u32  cpufreq_get_current_hz(void);
u32  cpufreq_get_min_hz(void);
u32  cpufreq_get_max_hz(void);

/*
 * cpufreq_sample — called from timer_irq_handler every TIMER_HZ ticks.
 * idle_ticks: number of ticks the idle task was running in the last window.
 * total_ticks: total ticks in the window (should equal TIMER_HZ).
 */
void cpufreq_sample(u32 idle_ticks, u32 total_ticks);

/* Called by thermal driver to override governor and force minimum clock */
void cpufreq_thermal_cap(bool throttle);

#endif /* DRIVERS_POWER_CPUFREQ_H */
