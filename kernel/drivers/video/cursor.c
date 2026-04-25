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
    0xC000, 0xF000, 0xF800, 0xFC00,
    0xFE00, 0xFF00, 0xFF80, 0xFFC0,
    0xFFE0, 0xFFC0, 0xEF80, 0xC7C0,
    0x03C0, 0x01E0, 0x00E0, 0x0060,
};

static const u16 cursor_fill[CURSOR_H] = {
    0x8000, 0xC000, 0xE000, 0xF000,
    0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFF00, 0xC700, 0x8380,
    0x0180, 0x00C0, 0x0040, 0x0000,
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
    cur_x       = 512;
    cur_y       = 384;
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
    if (x + CURSOR_W > 1024) x = 1024 - CURSOR_W;
    if (y + CURSOR_H >  768) y =  768 - CURSOR_H;

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
