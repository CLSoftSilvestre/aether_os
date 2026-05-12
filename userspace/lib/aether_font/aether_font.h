/*
 * aether_font.h — Phase 7.2 FreeType wrapper for AetherOS
 *
 * Provides a simple API for loading TTF/OTF fonts and rendering UTF-8 text
 * into a 32-bit ARGB pixel buffer.  Backed by FreeType 2.13.x.
 *
 * Typical usage:
 *   aether_font_init();
 *   aether_font_t *f;
 *   aether_font_load("/fonts/NotoSans-Regular.ttf", &f);
 *   aether_font_draw(f, "Hello!", 18, 0xFFFFFF, fb, stride, w, h, x, y);
 *   aether_font_free(f);
 */

#ifndef AETHER_FONT_H
#define AETHER_FONT_H

#include <stdint.h>

typedef struct aether_font aether_font_t;

/*
 * Initialize the FreeType library.  Call once before any other function.
 * Returns 0 on success, -1 on failure.
 */
int aether_font_init(void);

/*
 * Load a font from a path on the AetherOS filesystem.
 * *out is set to an opaque handle; free it with aether_font_free().
 * Returns 0 on success, -1 on failure.
 */
int aether_font_load(const char *path, aether_font_t **out);

/*
 * Release a font handle.
 */
void aether_font_free(aether_font_t *font);

/*
 * Render UTF-8 text into a 32-bit (0x00RRGGBB) pixel buffer.
 *
 *   font      — font handle from aether_font_load()
 *   text      — NUL-terminated UTF-8 string
 *   px_size   — font size in pixels (e.g. 12, 18, 24)
 *   color     — foreground color as 0x00RRGGBB
 *   buf       — destination pixel buffer
 *   stride    — buffer row stride in pixels (usually == buf_w)
 *   buf_w/h   — buffer dimensions (used for clipping)
 *   x, y      — baseline position (y is the baseline, not the top)
 *
 * Returns the total pixel advance (text width rendered), or -1 on error.
 */
int aether_font_draw(aether_font_t *font,
                     const char *text, int px_size, uint32_t color,
                     uint32_t *buf, int stride, int buf_w, int buf_h,
                     int x, int y);

/*
 * Measure the pixel width of text without rendering.
 * Returns pixel advance, or -1 on error.
 */
int aether_font_measure_width(aether_font_t *font,
                               const char *text, int px_size);

/*
 * Return the ascender in pixels at px_size (distance from baseline to top
 * of tallest glyph).  Used to position the baseline in a render buffer.
 */
int aether_font_get_ascent(aether_font_t *font, int px_size);

/*
 * Return the full line height in pixels at px_size (ascender + |descender|).
 * Use as the render-buffer row height.
 */
int aether_font_get_height(aether_font_t *font, int px_size);

#endif /* AETHER_FONT_H */
