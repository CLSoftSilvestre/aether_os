/*
 * AetherOS — Software Cursor Overlay
 * File: kernel/drivers/video/cursor.c
 *
 * 16×16 arrow sprite rendered directly into the framebuffer.
 * The region under the sprite is saved and restored on each move so no
 * damage is left on the desktop.
 *
 * Arrow shape (16 rows × 16 bits, MSB = leftmost pixel):
 *   1 = draw cursor pixel  (white body / accent border)
 *   0 = transparent (background shows through)
 */

#include "drivers/video/cursor.h"
#include "drivers/video/fb.h"
#include "aether/printk.h"
#include "aether/types.h"

#define CURSOR_W  16
#define CURSOR_H  16

/* Two-layer bitmap: outer border (black) and inner fill (white) */
static const u16 cursor_border[CURSOR_H] = {
    0x4000, 0xE000, 0xF000, 0xF800,
    0xFC00, 0xFE00, 0xFF00, 0xFF80,
    0xFFC0, 0xFFE0, 0xFFC0, 0xFE00,
    0xEF00, 0x4F80, 0x0780, 0x0300,
};

static const u16 cursor_fill[CURSOR_H] = {
    0x0000, 0x4000, 0x6000, 0x7000,
    0x7800, 0x7C00, 0x7E00, 0x7F00,
    0x7F80, 0x7FC0, 0x7C00, 0x6400,
    0x4600, 0x0300, 0x0300, 0x0000,
};

/* Lumina-themed cursor colors */
#define CURSOR_BORDER  0x000000u   /* black outline      */
#define CURSOR_FILL    0xFFFFFFu   /* white fill         */

/* Background save buffer: 16×16 pixels × 4 bytes */
static u32 bg_save[CURSOR_H][CURSOR_W];

static unsigned int cur_x;
static unsigned int cur_y;
static int          cur_visible;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static void save_bg(unsigned int x, unsigned int y)
{
    for (unsigned int row = 0; row < CURSOR_H; row++) {
        for (unsigned int col = 0; col < CURSOR_W; col++) {
            bg_save[row][col] = fb_get_pixel(x + col, y + row);
        }
    }
}

static void restore_bg(unsigned int x, unsigned int y)
{
    for (unsigned int row = 0; row < CURSOR_H; row++) {
        for (unsigned int col = 0; col < CURSOR_W; col++) {
            fb_put_pixel(x + col, y + row, bg_save[row][col]);
        }
    }
}

static void draw_sprite(unsigned int x, unsigned int y)
{
    for (unsigned int row = 0; row < CURSOR_H; row++) {
        for (unsigned int col = 0; col < CURSOR_W; col++) {
            u16 mask = (u16)(1u << (15 - col));
            if (cursor_border[row] & mask) {
                u32 color = (cursor_fill[row] & mask) ? CURSOR_FILL : CURSOR_BORDER;
                fb_put_pixel(x + col, y + row, color);
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void cursor_init(void)
{
    cur_x       = fb_width  ? fb_width  / 2u : 640u;
    cur_y       = fb_height ? fb_height / 2u : 360u;
    cur_visible = 0;
}

void cursor_show(void)
{
    if (cur_visible) return;
    save_bg(cur_x, cur_y);
    draw_sprite(cur_x, cur_y);
    cur_visible = 1;
}

void cursor_hide(void)
{
    if (!cur_visible) return;
    restore_bg(cur_x, cur_y);
    cur_visible = 0;
}

void cursor_move(unsigned int x, unsigned int y)
{
    static int dbg = 0;
    if (dbg++ < 10)
        kinfo("[cursor_move] x=%u y=%u vis=%d\n", x, y, cur_visible);

    /* Clamp to screen */
    unsigned sw = fb_width  ? fb_width  : 1280u;
    unsigned sh = fb_height ? fb_height :  720u;
    if (x + CURSOR_W > sw) x = sw - CURSOR_W;
    if (y + CURSOR_H > sh) y = sh - CURSOR_H;

    if (x == cur_x && y == cur_y) return;

    if (cur_visible) restore_bg(cur_x, cur_y);

    cur_x = x;
    cur_y = y;

    if (cur_visible) {
        save_bg(cur_x, cur_y);
        draw_sprite(cur_x, cur_y);
    }
}

void cursor_get_pos(unsigned int *x, unsigned int *y)
{
    *x = cur_x;
    *y = cur_y;
}
