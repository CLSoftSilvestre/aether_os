/*
 * AetherOS — Desktop Manager (PID 1)
 * File: userspace/apps/init/main.c
 *
 * Phase 4.4: init is now a lightweight desktop manager.
 * Responsibilities:
 *   1. Claim the framebuffer
 *   2. Draw the static desktop chrome (background, top bar, bottom bar)
 *   3. Spawn statusbar (background sidebar daemon)
 *   4. Spawn aether_term (interactive terminal window)
 *   5. Refresh the top bar clock and bottom bar memory every second
 *
 * Desktop layout (1024×768):
 *   [0]   Top bar    1024×36  — branding + HH:MM:SS uptime
 *   [36]  Accent     1024×2   — purple separator
 *   [38]  Main area  1024×706 — aether_term + statusbar occupy this
 *   [744] Bot bar    1024×24  — status + free memory
 */

#include <gfx.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>
#include <input.h>

/* ── Layout constants ────────────────────────────────────────────────────── */

#define TOPBAR_Y    0
#define TOPBAR_H   36
#define ACCENT_Y   36
#define ACCENT_H    2
#define BOTBAR_Y  744
#define BOTBAR_H   24
#define FONT_W      8
#define FONT_H      8

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void fmt_uptime(char *buf, long ticks)
{
    long s = ticks / 100, m = s / 60; s %= 60;
    long h = m / 60;                  m %= 60;
    snprintf(buf, 16, "%02ld:%02ld:%02ld", h, m, s);
}

/* ── Chrome drawing ──────────────────────────────────────────────────────── */

static void draw_desktop(void)
{
    gfx_fill(0, 0, 1024, 768, C_DESKTOP);
}

static void draw_top_bar(long ticks)
{
    gfx_fill(0, TOPBAR_Y, 1024, TOPBAR_H, C_PANEL);

    gfx_text(14, TOPBAR_Y + 10, "AetherOS", C_TEXT, C_PANEL);
    gfx_text(14 + 8 * FONT_W + 8, TOPBAR_Y + 10, "v0.0.5", C_TEXT_DIM, C_PANEL);

    gfx_text_center(0, 1024, TOPBAR_Y + 10,
                    "Phase 4.5  --  Lumina Desktop", C_TEXT_DIM, C_PANEL);

    char ubuf[20];
    char tbuf[16];
    fmt_uptime(tbuf, ticks);
    snprintf(ubuf, sizeof(ubuf), "up %s", tbuf);
    int len = (int)strlen(ubuf);
    gfx_text(1024 - len * FONT_W - 14, TOPBAR_Y + 10,
             ubuf, C_TEXT_DIM, C_PANEL);

    gfx_fill(0, ACCENT_Y, 1024, ACCENT_H, C_ACCENT);
}

static void draw_bot_bar(long ticks)
{
    gfx_fill(0, BOTBAR_Y, 1024, BOTBAR_H, C_PANEL);
    gfx_hline(0, BOTBAR_Y, 1024, C_SEP);

    gfx_text(14, BOTBAR_Y + 6,
             "AetherOS 0.0.5  |  QEMU virt  |  Cortex-A76  |  1024x768",
             C_TEXT_DIM, C_PANEL);

    long v = sys_pmm_stats();
    unsigned long free_pages = (unsigned long)((unsigned long long)v >> 32);
    unsigned long free_mb    = free_pages * 4 / 1024;
    char mbuf[24];
    snprintf(mbuf, sizeof(mbuf), "Free: %lu MB", free_mb);
    int mlen = (int)strlen(mbuf);
    gfx_text(1024 - mlen * FONT_W - 14, BOTBAR_Y + 6,
             mbuf, C_TEXT_DIM, C_PANEL);

    (void)ticks;
}

/* Refresh only the live uptime portion of the top bar */
static void refresh_top_bar(long ticks)
{
    char ubuf[20];
    char tbuf[16];
    fmt_uptime(tbuf, ticks);
    snprintf(ubuf, sizeof(ubuf), "up %s", tbuf);
    int len = (int)strlen(ubuf);
    int x = 1024 - len * FONT_W - 14;
    gfx_fill(x - 2, TOPBAR_Y, 1024 - (x - 2), TOPBAR_H, C_PANEL);
    gfx_text(x, TOPBAR_Y + 10, ubuf, C_TEXT_DIM, C_PANEL);
}

/* Refresh only the free-memory portion of the bottom bar */
static void refresh_bot_bar(void)
{
    long v = sys_pmm_stats();
    unsigned long free_pages = (unsigned long)((unsigned long long)v >> 32);
    unsigned long free_mb    = free_pages * 4 / 1024;
    char mbuf[24];
    snprintf(mbuf, sizeof(mbuf), "Free: %lu MB", free_mb);
    int mlen = (int)strlen(mbuf);
    int x = 1024 - mlen * FONT_W - 14;
    gfx_fill(x - 2, BOTBAR_Y, 1024 - (x - 2), BOTBAR_H, C_PANEL);
    gfx_text(x, BOTBAR_Y + 6, mbuf, C_TEXT_DIM, C_PANEL);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    sys_fb_claim();
    gfx_init();

    /* Draw static desktop chrome */
    draw_desktop();
    draw_top_bar(gfx_ticks());
    draw_bot_bar(gfx_ticks());

    /* Launch background processes — no waitpid, they run concurrently */
    sys_spawn("/statusbar");
    sys_spawn("/aether_term");

    /* Show the mouse cursor once apps are up */
    sys_cursor_show(1);

    /* Refresh bars every second; poll mouse on each tick */
    for (;;) {
        long ticks = gfx_ticks();
        refresh_top_bar(ticks);
        refresh_bot_bar();

        /* Drain all pending mouse events and move cursor */
        unsigned long long me;
        while ((me = sys_mouse_poll()) != 0) {
            mouse_event_t ev = mouse_event_unpack(me);
            sys_cursor_move(ev.x, ev.y);
            /* Future: hit-test ev.buttons against window regions */
        }

        sys_sleep(100);
    }

    return 0;
}
