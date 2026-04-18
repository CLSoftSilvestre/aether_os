/*
 * AetherOS — StatusBar
 * File: userspace/apps/statusbar/main.c
 *
 * Phase 4.4: Background sidebar daemon.
 * Draws the System Info panel (uptime, memory bar, platform) and
 * refreshes it every second (100 ticks at 100 Hz).
 * Runs forever until the OS shuts down.
 */

#include <gfx.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>

/* ── Layout constants (must match init) ─────────────────────────────────── */

#define TOPBAR_H   36
#define ACCENT_H    2
#define BOTBAR_Y  744

#define FONT_W  8
#define FONT_H  8

/* Terminal window geometry — needed to align the sidebar */
#define TERM_COLS  80
#define WIN_W     (TERM_COLS * FONT_W + 16)   /* 656 */
#define TITLE_H    28
#define WIN_H     (TITLE_H + 8 + 70 * FONT_H + 8)
#define WIN_X      8
#define WIN_Y     (TOPBAR_H + ACCENT_H + \
                   (BOTBAR_Y - TOPBAR_H - ACCENT_H - WIN_H) / 2)

/* Sidebar */
#define SIDE_X    (WIN_X + WIN_W + 8)    /* 672 */
#define SIDE_W    (1024 - SIDE_X - 8)    /* 344 */
#define SIDE_Y     WIN_Y
#define SIDE_TH    24
#define SIDE_H     280

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void fmt_uptime(char *buf, long ticks)
{
    long s = ticks / 100, m = s / 60; s %= 60;
    long h = m / 60;                  m %= 60;
    snprintf(buf, 16, "%02ld:%02ld:%02ld", h, m, s);
}

/* ── Sidebar drawing ─────────────────────────────────────────────────────── */

static void draw_sidebar(long ticks)
{
    /* Shadow */
    gfx_fill(SIDE_X + 3, SIDE_Y + 3, SIDE_W, SIDE_H, GFX_RGB(8, 8, 14));

    /* Body */
    gfx_fill(SIDE_X, SIDE_Y, SIDE_W, SIDE_H, C_WIN_BG);

    /* Title bar */
    gfx_fill(SIDE_X, SIDE_Y, SIDE_W, SIDE_TH, C_TITLEBAR);
    gfx_text_center(SIDE_X, SIDE_W, SIDE_Y + 6,
                    "System Info", C_TEXT, C_TITLEBAR);
    gfx_hline(SIDE_X, SIDE_Y + SIDE_TH, SIDE_W, C_ACCENT);
    gfx_rect(SIDE_X, SIDE_Y, SIDE_W, SIDE_H, C_SEP);

    int y = SIDE_Y + SIDE_TH + 12;

    /* Uptime */
    gfx_text(SIDE_X + 10, y, "Uptime", C_TEXT_DIM, C_WIN_BG);
    y += FONT_H + 2;
    char tbuf[16];
    fmt_uptime(tbuf, ticks);
    gfx_text(SIDE_X + 10, y, tbuf, C_ACCENT, C_WIN_BG);
    y += FONT_H + 14;

    /* Memory */
    gfx_hline(SIDE_X + 10, y, SIDE_W - 20, C_SEP);
    y += 6;
    gfx_text(SIDE_X + 10, y, "Memory", C_TEXT_DIM, C_WIN_BG);
    y += FONT_H + 4;

    long v = sys_pmm_stats();
    unsigned long free_p  = (unsigned long)((unsigned long long)v >> 32);
    unsigned long total_p = (unsigned long)((unsigned long long)v & 0xFFFFFFFFUL);
    unsigned long free_mb  = free_p  * 4 / 1024;
    unsigned long total_mb = total_p * 4 / 1024;

    char mbuf[32];
    snprintf(mbuf, sizeof(mbuf), "%lu MB free / %lu MB", free_mb, total_mb);
    gfx_text(SIDE_X + 10, y, mbuf, C_TEXT, C_WIN_BG);
    y += FONT_H + 4;

    unsigned bar_w = (unsigned)(SIDE_W - 20);
    unsigned used_w = (unsigned)(bar_w - (bar_w * free_p / total_p));
    gfx_fill(SIDE_X + 10, y, (int)used_w, 8, C_ACCENT);
    gfx_fill(SIDE_X + 10 + (int)used_w, y,
             (int)(bar_w - used_w), 8, GFX_RGB(40, 40, 60));
    y += 8 + 14;

    /* Platform */
    gfx_hline(SIDE_X + 10, y, SIDE_W - 20, C_SEP);
    y += 6;
    gfx_text(SIDE_X + 10, y, "Platform", C_TEXT_DIM, C_WIN_BG);
    y += FONT_H + 4;
    gfx_text(SIDE_X + 10, y, "QEMU virt", C_TEXT, C_WIN_BG);
    y += FONT_H + 2;
    gfx_text(SIDE_X + 10, y, "Cortex-A76 / AArch64", C_TEXT, C_WIN_BG);
    y += FONT_H + 2;
    gfx_text(SIDE_X + 10, y, "AetherOS v0.0.5", C_TEXT, C_WIN_BG);
    y += FONT_H + 14;

    /* Process count hint */
    gfx_hline(SIDE_X + 10, y, SIDE_W - 20, C_SEP);
    y += 6;
    gfx_text(SIDE_X + 10, y, "Phase 4.4", C_TEXT_DIM, C_WIN_BG);
    y += FONT_H + 2;
    gfx_text(SIDE_X + 10, y, "Multi-process desktop", C_ACCENT2, C_WIN_BG);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    for (;;) {
        draw_sidebar(gfx_ticks());
        sys_sleep(100);   /* refresh every second */
    }

    return 0;
}
