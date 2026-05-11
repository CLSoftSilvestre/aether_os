#ifndef DRIVERS_POWER_DPMS_H
#define DRIVERS_POWER_DPMS_H
/*
 * AetherOS — Display Power Management (Phase 6.2.3)
 * File: kernel/include/drivers/power/dpms.h
 *
 * Software DPMS: after DPMS_BLANK_TICKS of input inactivity the framebuffer
 * is filled black and the software cursor is hidden.  The display is "woken"
 * on any keyboard or mouse activity by the first call to dpms_activity().
 *
 * On Pi 4 hardware with HDMI connected, a future phase can invoke the
 * VideoCore mailbox SET_DISPLAY_POWER_STATE tag (0x00040001) in addition
 * to the software blank.  For QEMU the software blackout is the only effect.
 *
 * After a wake, userspace redraws naturally: the next input event that
 * caused the wake propagates through the event queue and triggers a repaint
 * in the focused application.
 */

#include "aether/types.h"

/* 5 minutes of inactivity at 100 Hz */
#define DPMS_BLANK_TICKS  (5u * 60u * 100u)

void dpms_init(void);

/* Call on every keyboard or mouse event (from the timer ISR / poll path) */
void dpms_activity(void);

/* Called every second (1 Hz) from the power management tick */
void dpms_tick(void);

bool dpms_is_blanked(void);
void dpms_force_blank(void);
void dpms_force_wake(void);

#endif /* DRIVERS_POWER_DPMS_H */
