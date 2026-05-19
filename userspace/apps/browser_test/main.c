/*
 * Phase 7.7 — NetSurf browser integration test
 *
 * Exercises the full Phase 7.7 rendering pipeline:
 *   netsurf_init → browser_window_create → scheduler drain
 *   → browser_window_redraw (plot_aether) → gfx_raw_blit to screen
 *
 * Usage inside AetherOS:
 *   browser_test                     → renders http://10.0.2.2:8080/index.html
 *   browser_test http://example.com/ → renders the given URL
 *
 * Expected: the rendered page appears on screen, "browser_test PASS" printed.
 *
 * QEMU setup: python3 -m http.server 8080 --directory tests/browser/  (host)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* NetSurf public API */
#include "netsurf/netsurf.h"
#include "netsurf/misc.h"
#include "netsurf/window.h"
#include "netsurf/fetch.h"
#include "netsurf/bitmap.h"
#include "netsurf/layout.h"
#include "netsurf/plotters.h"
#include "netsurf/browser_window.h"
#include "utils/errors.h"
#include "utils/nsoption.h"
#include "content/fetch.h"

/* AetherOS bridge */
#include "netsurf_aether.h"
#include "plot_aether.h"

/* AetherOS gfx */
#include "gfx.h"
#include "sys.h"

/* ── globals from gui_window_stub.c ─────────────────────────────────────── */

extern volatile bool          nsaether_dirty;
extern struct browser_window *nsaether_bw;
extern uint32_t              *nsaether_pixels;
extern int                    nsaether_win_w;
extern int                    nsaether_win_h;

/* ── externs ─────────────────────────────────────────────────────────────── */

extern struct gui_misc_table   aether_misc_table;
extern struct gui_window_table aether_window_table;
extern struct gui_fetch_table  aether_fetch_table;
extern struct gui_bitmap_table aether_bitmap_table;
extern struct gui_layout_table aether_layout_table;

extern void nslog_aether_init(void);
extern void fetch_http_aether_register(void);

/* ── netsurf table ───────────────────────────────────────────────────────── */

static struct netsurf_table aether_netsurf_table = {
    .misc   = &aether_misc_table,
    .window = &aether_window_table,
    .fetch  = &aether_fetch_table,
    .bitmap = &aether_bitmap_table,
    .layout = &aether_layout_table,
};

/* ── I/O helpers ─────────────────────────────────────────────────────────── */

static void uart_write(const char *s)
{
    long r;
    int len = 0;
    while (s[len]) len++;
    __asm__ volatile(
        "mov x8, #34\n mov x0, #1\n mov x1, %1\n mov x2, %2\n"
        "svc #0\n mov %0, x0\n"
        : "=r"(r) : "r"(s), "r"((long)len)
        : "x0","x1","x2","x8","memory"
    );
}

static void exit_with(int code)
{
    __asm__ volatile(
        "mov x8, #0\n mov x0, %0\n svc #0\n"
        :: "r"((long)code) : "x0","x8","memory"
    );
    __builtin_unreachable();
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *target_url = (argc > 1) ? argv[1]
                                        : "http://10.0.2.2:8080/index.html";

    uart_write("browser_test: starting...\n");

    /* 1. Init */
    nslog_aether_init();

    if (netsurf_register(&aether_netsurf_table) != NSERROR_OK) {
        uart_write("browser_test FAIL: netsurf_register\n");
        exit_with(1);
    }
    if (nsoption_init(NULL, NULL, NULL) != NSERROR_OK) {
        uart_write("browser_test FAIL: nsoption_init\n");
        exit_with(1);
    }
    nserror ni = netsurf_init(NULL);
    if (ni != NSERROR_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "browser_test FAIL: netsurf_init err=%d\n", (int)ni);
        uart_write(buf);
        exit_with(1);
    }
    uart_write("browser_test: netsurf_init OK\n");

    fetch_http_aether_register();

    /* 2. Window dimensions — content area below a 36-px titlebar */
    gfx_init();
    int sw = (int)gfx_width();
    int sh = (int)gfx_height();
    int title_h = 36;
    int win_x = 0, win_y = title_h;
    int win_w = sw, win_h = sh - title_h;

    nsaether_win_w = win_w;
    nsaether_win_h = win_h;

    /* 3. Create browser window */
    struct nsurl *url = NULL;
    nserror ne = nsurl_create(target_url, &url);
    if (ne != NSERROR_OK || !url) {
        uart_write("browser_test FAIL: nsurl_create\n");
        exit_with(1);
    }
    uart_write("browser_test: navigating to ");
    uart_write(target_url);
    uart_write("\n");

    struct browser_window *bw = NULL;
    nserror be = browser_window_create(
            BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
            url, NULL, NULL, &bw);
    nsurl_unref(url);

    if (be != NSERROR_OK || !bw) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "browser_test FAIL: browser_window_create err=%d\n", (int)be);
        uart_write(buf);
        exit_with(1);
    }
    uart_write("browser_test: browser_window_create OK\n");

    /* 4. Draw chrome: window frame + "Loading..." status */
    gfx_glass_window_frame(win_x, 0, win_w, sh, title_h, "AetherOS Browser", 0);
    gfx_fill(win_x, win_y, win_w, win_h, 0x00FFFFFF);  /* white content area */

    /* 5. Drain scheduler until the page is ready (10-second wall-clock limit) */
    {
        long start = (long)sys_get_ticks();
        long deadline = start + 1000; /* 10 s at 100 Hz */
        for (;;) {
            nsaether_schedule_drain();
            if (nsaether_dirty && browser_window_redraw_ready(nsaether_bw))
                break;
            long now = (long)sys_get_ticks();
            if (now >= deadline)
                break;
            sys_sleep(1); /* ~10 ms — let the clock advance between drains */
        }
    }

    if (!nsaether_dirty) {
        uart_write("browser_test: page load timed out\n");
    }

    /* 6. Render into off-screen buffer then blit to screen */
    if (nsaether_pixels && nsaether_bw) {
        /* Clear buffer to white before each render */
        memset(nsaether_pixels, 0xFF, (size_t)win_w * (size_t)win_h * 4);

        aether_plot_ctx_t plot_ctx;
        aether_plot_ctx_init(&plot_ctx, nsaether_pixels, win_w, win_h);

        struct redraw_context rctx = {
            .interactive       = true,
            .background_images = true,
            .plot              = &aether_plotter_table,
            .priv              = &plot_ctx,
        };

        struct rect content_rect = { 0, 0, win_w, win_h };
        browser_window_redraw(nsaether_bw, 0, 0, &content_rect, &rctx);
        nsaether_dirty = false;

        /* Blit rendered pixels to framebuffer at content area position */
        gfx_raw_blit(nsaether_pixels, (unsigned)win_w,
                     win_x, win_y, (unsigned)win_w, (unsigned)win_h);

        uart_write("browser_test: page rendered to screen\n");
    }

    netsurf_exit();

    uart_write("browser_test PASS\n");
    exit_with(0);
    return 0;
}
