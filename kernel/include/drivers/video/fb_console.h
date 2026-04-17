#ifndef DRIVERS_VIDEO_FB_CONSOLE_H
#define DRIVERS_VIDEO_FB_CONSOLE_H

/*
 * AetherOS — Framebuffer Text Console
 *
 * A scrolling text console rendered on the framebuffer.
 * Call fb_console_init() after ramfb_init().
 * Hook fb_console_putc() into the printk output path.
 */

void fb_console_init(void);
void fb_console_putc(char c);
void fb_console_puts(const char *s);

#endif /* DRIVERS_VIDEO_FB_CONSOLE_H */
