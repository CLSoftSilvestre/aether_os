#ifndef DRIVERS_VIDEO_RAMFB_H
#define DRIVERS_VIDEO_RAMFB_H

/*
 * QEMU ramfb — simple framebuffer backed by guest RAM.
 *
 * Requires QEMU launched with:  -device ramfb -vga none
 *
 * ramfb_init() allocates a contiguous physical buffer from the PMM,
 * configures the ramfb via fw_cfg, and populates:
 *   fb_base, fb_width, fb_height, fb_stride
 */

void ramfb_init(void);

#endif /* DRIVERS_VIDEO_RAMFB_H */
