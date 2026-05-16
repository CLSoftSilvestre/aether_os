/*
 * Phase 7.7 — gui_window_table: AetherOS implementation
 *
 * Each browser window owns an off-screen XRGB8888 pixel buffer sized to
 * the content area (default 1280×720).  When NetSurf marks the window
 * dirty via aether_window_invalidate(), it sets nsaether_dirty = true.
 * The browser application checks that flag each event-loop tick, calls
 * browser_window_redraw() with aether_plotter_table, then blits the buffer
 * to the screen framebuffer with gfx_raw_blit().
 *
 * For Phase 7.7, one window is supported at a time.  The global
 * nsaether_bw / nsaether_dirty / nsaether_pixels / nsaether_win_w/h
 * variables provide the app with everything it needs without an
 * additional API header.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/window.h"
#include "netsurf/plotters.h"
#include "netsurf/mouse.h"
#include "utils/nsurl.h"
#include "netsurf_aether.h"

/* ── globals accessed by the browser app ────────────────────────────────── */

volatile bool          nsaether_dirty   = false;
volatile bool          nsaether_loading = false;
struct browser_window *nsaether_bw      = NULL;
uint32_t              *nsaether_pixels  = NULL;
int                    nsaether_win_w   = 1280;
int                    nsaether_win_h   =  720;
char                   nsaether_status[256] = {0};
char                   nsaether_url[512]    = {0};

/* ── window struct ───────────────────────────────────────────────────────── */

struct gui_window {
    struct browser_window *bw;
    uint32_t              *pixels;
    int                    width;
    int                    height;
};

/* ── window lifecycle ────────────────────────────────────────────────────── */

static struct gui_window *aether_window_create(struct browser_window *bw,
                                               struct gui_window     *existing,
                                               gui_window_create_flags flags)
{
    (void)existing; (void)flags;

    struct gui_window *gw = malloc(sizeof(struct gui_window));
    if (!gw) return NULL;

    int w = nsaether_win_w;
    int h = nsaether_win_h;

    gw->pixels = malloc((size_t)w * (size_t)h * 4);
    if (!gw->pixels) { free(gw); return NULL; }

    /* Clear to white — browser CSS will paint over this */
    memset(gw->pixels, 0xFF, (size_t)w * (size_t)h * 4);

    gw->bw     = bw;
    gw->width  = w;
    gw->height = h;

    /* Expose globals so the app can reach bw and pixels */
    nsaether_bw     = bw;
    nsaether_pixels = gw->pixels;

    return gw;
}

static void aether_window_destroy(struct gui_window *gw)
{
    if (!gw) return;
    if (nsaether_bw     == gw->bw)     nsaether_bw     = NULL;
    if (nsaether_pixels == gw->pixels) nsaether_pixels = NULL;
    free(gw->pixels);
    free(gw);
}

static void gw_dbg(const char *s)
{
    long r;
    int len = 0;
    while (s[len]) len++;
    __asm__ volatile(
        "mov x8, #34\n mov x0, #1\n mov x1, %1\n mov x2, %2\n"
        "svc #0\n mov %0, x0\n"
        : "=r"(r) : "r"(s), "r"((long)len) : "x0","x1","x2","x8","memory"
    );
}

static nserror aether_window_invalidate(struct gui_window *gw,
                                         const struct rect *rect)
{
    (void)gw; (void)rect;
    gw_dbg("aether_window_invalidate: dirty flag set\n");
    nsaether_dirty = true;
    return NSERROR_OK;
}

static bool aether_window_get_scroll(struct gui_window *gw,
                                      int *sx, int *sy)
{
    (void)gw;
    if (sx) *sx = 0;
    if (sy) *sy = 0;
    return true;
}

static nserror aether_window_set_scroll(struct gui_window *gw,
                                         const struct rect *rect)
{
    (void)gw; (void)rect;
    return NSERROR_OK;
}

static nserror aether_window_get_dimensions(struct gui_window *gw,
                                             int *width, int *height)
{
    if (width)  *width  = gw ? gw->width  : nsaether_win_w;
    if (height) *height = gw ? gw->height : nsaether_win_h;
    return NSERROR_OK;
}

static nserror aether_window_event(struct gui_window    *gw,
                                    enum gui_window_event e)
{
    (void)gw;
    switch (e) {
    case GW_EVENT_START_THROBBER:
        nsaether_loading = true;
        break;
    case GW_EVENT_STOP_THROBBER:
        nsaether_loading = false;
        break;
    default:
        break;
    }
    return NSERROR_OK;
}

static void aether_window_set_title(struct gui_window *gw, const char *title)
{
    (void)gw; (void)title;
    /* Title bar is drawn by the browser app from the URL — no-op for MVP */
}

static nserror aether_window_set_url(struct gui_window *gw, struct nsurl *url)
{
    (void)gw;
    if (!url) return NSERROR_OK;
    const char *s = nsurl_access(url);
    if (s) {
        size_t n = 0;
        while (s[n] && n < sizeof(nsaether_url) - 1) n++;
        size_t i;
        for (i = 0; i < n; i++) nsaether_url[i] = s[i];
        nsaether_url[i] = '\0';
    }
    return NSERROR_OK;
}

static void aether_window_set_status(struct gui_window *gw, const char *text)
{
    (void)gw;
    if (!text) { nsaether_status[0] = '\0'; return; }
    size_t i = 0;
    while (text[i] && i < sizeof(nsaether_status) - 1) {
        nsaether_status[i] = text[i]; i++;
    }
    nsaether_status[i] = '\0';
}

/* ── exported table ──────────────────────────────────────────────────────── */

struct gui_window_table aether_window_table = {
    .create         = aether_window_create,
    .destroy        = aether_window_destroy,
    .invalidate     = aether_window_invalidate,
    .get_scroll     = aether_window_get_scroll,
    .set_scroll     = aether_window_set_scroll,
    .get_dimensions = aether_window_get_dimensions,
    .event          = aether_window_event,
    .set_title      = aether_window_set_title,
    .set_url        = aether_window_set_url,
    .set_status     = aether_window_set_status,
};
