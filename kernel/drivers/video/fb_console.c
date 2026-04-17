/*
 * AetherOS — Framebuffer Text Console
 * File: kernel/drivers/video/fb_console.c
 *
 * Scrolling text console rendered on the framebuffer.
 * Character grid: 1024/8 × 768/8 = 128 columns × 96 rows.
 *
 * Call fb_console_init() after ramfb_init().
 * Hook fb_console_putc() into the printk output path for dual UART+FB output.
 *
 * Colors:
 *   Background: near-black #121218 (matches ramfb clear color)
 *   Foreground: soft white  #D8D8E8
 *   Kernel log prefix colors applied by log level (coming in 4.1)
 */

#include "drivers/video/fb_console.h"
#include "drivers/video/fb.h"
#include "drivers/video/font.h"
#include "aether/types.h"

/* ── Console geometry ────────────────────────────────────────────────── */

#define CON_COLS  128   /* fb_width  / FONT_W */
#define CON_ROWS   96   /* fb_height / FONT_H */

#define CON_FG  FB_RGB(216, 216, 232)   /* #D8D8E8 — soft white         */
#define CON_BG  FB_RGB( 18,  18,  24)   /* #121218 — near-black         */

/* ── Cursor position ─────────────────────────────────────────────────── */

static u32 cur_col;
static u32 cur_row;
static int con_ready;
static int con_claimed;   /* set by fb_console_claim() — user process took over */

/* ── Scroll one line up ──────────────────────────────────────────────── */

static void scroll_up(void)
{
    if (!fb_base) return;

    u32 pitch    = fb_stride / 4;
    u32 row_px   = FONT_H * pitch;        /* pixels in one text row */
    u32 copy_px  = (CON_ROWS - 1) * row_px; /* everything except last row */

    /* Move rows 1..(CON_ROWS-1) up by one text row */
    volatile u32 *dst = fb_base;
    volatile u32 *src = fb_base + row_px;
    for (u32 i = 0; i < copy_px; i++)
        dst[i] = src[i];

    /* Clear the last row */
    fb_fill_rect(0, (CON_ROWS - 1) * FONT_H, fb_width, FONT_H, CON_BG);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void fb_console_init(void)
{
    if (!fb_base) return;
    cur_col = 0;
    cur_row = 0;
    con_ready = 1;
    con_claimed = 0;
}

void fb_console_claim(void)
{
    con_claimed = 1;
}

void fb_console_putc(char c)
{
    if (!con_ready || !fb_base || con_claimed) return;

    if (c == '\r') {
        cur_col = 0;
        return;
    }

    if (c == '\n') {
        cur_col = 0;
        cur_row++;
        if (cur_row >= CON_ROWS) {
            scroll_up();
            cur_row = CON_ROWS - 1;
        }
        return;
    }

    if (c == '\t') {
        /* Expand tab to next 8-column boundary */
        u32 next = (cur_col + 8u) & ~7u;
        while (cur_col < next)
            fb_console_putc(' ');
        return;
    }

    /* Printable character */
    font_draw_char(cur_col * FONT_W, cur_row * FONT_H,
                   (unsigned char)c, CON_FG, CON_BG);
    cur_col++;
    if (cur_col >= CON_COLS) {
        cur_col = 0;
        cur_row++;
        if (cur_row >= CON_ROWS) {
            scroll_up();
            cur_row = CON_ROWS - 1;
        }
    }
}

void fb_console_puts(const char *s)
{
    while (*s) fb_console_putc(*s++);
}
