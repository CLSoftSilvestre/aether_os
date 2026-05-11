/*
 * AetherOS — Thermal Monitoring & Throttling (Phase 6.2.4)
 * File: kernel/drivers/power/thermal.c
 */

#include "drivers/power/thermal.h"
#include "drivers/power/cpufreq.h"
#include "drivers/gpu/mailbox.h"
#include "aether/printk.h"
#include "aether/types.h"

static int  g_temp_mc    = (int)THERMAL_FAKE_MC;
static bool g_throttling = false;
static bool g_hw_present = false;

void thermal_init(void)
{
    if (mailbox_present()) {
        g_hw_present = true;
        u32 t = mailbox_get_temperature(MBOX_TEMP_SOC);
        g_temp_mc = (int)t;
        u32 max_t = mailbox_get_max_temperature(MBOX_TEMP_SOC);
        kinfo("[thermal] init: %d.%03d °C  (max %u.%03u °C)\n",
              g_temp_mc / 1000, g_temp_mc % 1000,
              max_t / 1000u, max_t % 1000u);
    } else {
        kinfo("[thermal] QEMU mode — simulated %u °C\n",
              THERMAL_FAKE_MC / 1000u);
    }
}

int  thermal_get_mc(void)        { return g_temp_mc; }
bool thermal_is_throttling(void) { return g_throttling; }

void thermal_tick(void)
{
    if (g_hw_present)
        g_temp_mc = (int)mailbox_get_temperature(MBOX_TEMP_SOC);
    /* else keep simulated value */

    bool should_throttle;
    if (!g_throttling)
        should_throttle = ((u32)g_temp_mc >= THERMAL_THROTTLE_MC);
    else
        /* hysteresis: stay throttled until temp drops enough */
        should_throttle = ((u32)g_temp_mc >= (THERMAL_THROTTLE_MC - THERMAL_HYST_MC));

    if (should_throttle != g_throttling) {
        g_throttling = should_throttle;
        cpufreq_thermal_cap(should_throttle);
        if (should_throttle)
            kwarn("[thermal] throttling: %d.%03d °C ≥ %u °C\n",
                  g_temp_mc / 1000, g_temp_mc % 1000,
                  THERMAL_THROTTLE_MC / 1000u);
        else
            kinfo("[thermal] unthrottled: %d.%03d °C\n",
                  g_temp_mc / 1000, g_temp_mc % 1000);
        return;
    }

    if (!g_throttling && (u32)g_temp_mc >= THERMAL_WARN_MC)
        kwarn("[thermal] high temp: %d.%03d °C\n",
              g_temp_mc / 1000, g_temp_mc % 1000);
}
