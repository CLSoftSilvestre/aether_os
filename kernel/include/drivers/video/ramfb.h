#ifndef DRIVERS_VIDEO_RAMFB_H
#define DRIVERS_VIDEO_RAMFB_H

/*
 * QEMU ramfb — simple framebuffer backed by guest RAM.
 *
 * ramfb_init() allocates a contiguous physical buffer from the PMM,
 * configures the ramfb via fw_cfg, and populates:
 *   fb_base, fb_width, fb_height, fb_stride
 *
 * ramfb_reconfigure(w, h) switches to a new resolution.
 * Must be called after fat32_mount() and before process_spawn().
 */

#include "aether/types.h"

void ramfb_init(void);

/* Returns 0 on success, -1 if resolution is not in the supported preset list. */
int  ramfb_reconfigure(u32 w, u32 h);

#endif /* DRIVERS_VIDEO_RAMFB_H */
