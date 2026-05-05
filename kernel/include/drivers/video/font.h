#ifndef DRIVERS_VIDEO_FONT_H
#define DRIVERS_VIDEO_FONT_H

/*
 * Lumina Mono — 8×16 bitmap font for AetherOS.
 *
 * Base glyphs: CP437 8×8 (public domain). Rendered as 8×16 by the
 * draw function using a 2× vertical stretch with 1-row top padding
 * and 1-row bottom padding, giving 14 px effective character height
 * and 2 px inter-line breathing room.
 *
 * Bit order: bit 7 of each byte is the leftmost pixel (col 0).
 */

#include "aether/types.h"

#define FONT_W   8
#define FONT_H  16

/* Draw one character at pixel position (x, y) on the framebuffer. */
void font_draw_char(u32 x, u32 y, unsigned char ch, u32 fg, u32 bg);

#endif /* DRIVERS_VIDEO_FONT_H */
