/*
 * AetherOS — Display Power Management (Phase 6.2.3)
 * File: kernel/drivers/power/dpms.c
 */

#include "drivers/power/dpms.h"
#include "drivers/video/fb.h"
#include "drivers/video/cursor.h"
#include "drivers/timer/arm_timer.h"
#include "aether/printk.h"
#include "aether/types.h"

static u64  g_last_activity = 0;
static bool g_blanked       = false;

void dpms_init(void)
{
    g_last_activity = timer_get_ticks();
    kinfo("[dpms] init: blank after %u s of inactivity\n",
          DPMS_BLANK_TICKS / TIMER_HZ);
}

void dpms_activity(void)
{
    g_last_activity = timer_get_ticks();
    if (g_blanked)
        dpms_force_wake();
}

bool dpms_is_blanked(void) { return g_blanked; }

void dpms_force_blank(void)
{
    if (g_blanked) return;
    g_blanked = true;
    cursor_hide();
    fb_fill_rect(0, 0, fb_width, fb_height, FB_BLACK);
    kinfo("[dpms] display blanked\n");
}

void dpms_force_wake(void)
{
    if (!g_blanked) return;
    g_blanked       = false;
    g_last_activity = timer_get_ticks();
    cursor_show();
    kinfo("[dpms] display wake\n");
    /*
     * Userspace redraws naturally: the input event that triggered this wake
     * propagates through the event queue and will cause the focused app to
     * repaint on its next event-loop iteration.
     */
}

void dpms_tick(void)
{
    if (g_blanked) return;
    u64 now = timer_get_ticks();
    if ((now - g_last_activity) >= DPMS_BLANK_TICKS)
        dpms_force_blank();
}
