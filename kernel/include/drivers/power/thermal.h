#ifndef DRIVERS_POWER_THERMAL_H
#define DRIVERS_POWER_THERMAL_H
/*
 * AetherOS — Thermal Monitoring & Throttling (Phase 6.2.4)
 * File: kernel/include/drivers/power/thermal.h
 *
 * Reads the BCM2711 SoC temperature via the VideoCore mailbox every second.
 * Two thresholds:
 *   WARN_MC     — log a warning but keep running at full speed
 *   THROTTLE_MC — call cpufreq_thermal_cap(true) to pin clock at minimum
 *
 * Temperature cools back below THROTTLE_MC - HYSTERESIS_MC before unthrottling
 * to avoid oscillating on/off at the boundary.
 *
 * On QEMU (no mailbox): returns THERMAL_FAKE_MC (45 000 mc = 45 °C) always.
 */

#include "aether/types.h"

#define THERMAL_WARN_MC      75000u   /* 75 °C */
#define THERMAL_THROTTLE_MC  80000u   /* 80 °C — start throttling */
#define THERMAL_HYST_MC       5000u   /* 5 °C hysteresis before unthrottle */
#define THERMAL_FAKE_MC      45000u   /* simulated QEMU temperature */

void thermal_init(void);
int  thermal_get_mc(void);         /* current temp in millidegrees C */
bool thermal_is_throttling(void);
void thermal_tick(void);           /* call every 1 Hz from timer ISR */

#endif /* DRIVERS_POWER_THERMAL_H */
