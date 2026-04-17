#ifndef DRIVERS_VIDEO_FONT_H
#define DRIVERS_VIDEO_FONT_H

/*
 * Embedded 8×8 bitmap font (public-domain VGA font, 256 glyphs).
 *
 * Each glyph is 8 bytes; bit 7 of each byte is the leftmost pixel.
 */

#include "aether/types.h"

#define FONT_W  8
#define FONT_H  8

/* Draw one character at pixel position (x, y) on the framebuffer. */
void font_draw_char(u32 x, u32 y, unsigned char ch, u32 fg, u32 bg);

#endif /* DRIVERS_VIDEO_FONT_H */
