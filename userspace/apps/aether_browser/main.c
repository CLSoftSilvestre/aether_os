/*
 * Phase 7.8 — aether_browser (compositor WM, libwidget chrome)
 *
 * Window chrome is drawn by on_reposition (gfx_glass_window_frame).
 * init handles close / minimize / focus / drag — no WM_FLAG_NO_CHROME.
 * Widget tree covers the content area below the title bar:
 *
 *   [0 .. TOOLBAR_H-1]  toolbar panel (back / fwd / reload / address bar)
 *   [TOOLBAR_H]         1-px separator (drawn by toolbar draw_fn)
 *   [TOOLBAR_H+1 ..]    viewport (NetSurf pixel blit)
 *   [.. win_h-TITLE_H-STATUS_H .. win_h-TITLE_H-1]  status bar
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
#include "netsurf/keypress.h"
#include "netsurf/mouse.h"
#include "utils/errors.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "content/fetch.h"
#include "desktop/browser_history.h"

/* AetherOS bridge */
#include "netsurf_aether.h"
#include "plot_aether.h"

/* AetherOS system */
#include "gfx.h"
#include "gpu.h"
#include "sys.h"
#include "input.h"
#include "widget.h"

/* ── externs from NetSurf bridge ─────────────────────────────────────────── */

extern volatile bool          nsaether_dirty;
extern volatile bool          nsaether_loading;
extern struct browser_window *nsaether_bw;
extern uint32_t              *nsaether_pixels;
extern int                    nsaether_win_w;
extern int                    nsaether_win_h;
extern char                   nsaether_status[256];
extern char                   nsaether_url[512];

extern struct gui_misc_table   aether_misc_table;
extern struct gui_window_table aether_window_table;
extern struct gui_fetch_table  aether_fetch_table;
extern struct gui_bitmap_table aether_bitmap_table;
extern struct gui_layout_table aether_layout_table;

extern void nslog_aether_init(void);
extern void fetch_http_aether_register(void);
extern void nsaether_schedule_drain(void);
extern void js_timers_tick(void);

/* ── Shell layout constants (must match topbar / dock) ───────────────────── */

#define TOPBAR_H      36
#define ACCENT_H       2
#define DOCK_H        56
#define DOCK_BB_STRIP 40

/* ── Window chrome constants ─────────────────────────────────────────────── */

#define TITLE_H       28
#define TOOLBAR_H     36
#define SEPARATOR_H    1
#define STATUS_H      20

/* Y offset from content-area top to viewport (toolbar + separator) */
#define VP_OFF  (TOOLBAR_H + SEPARATOR_H)   /* 37 */

/* Toolbar button geometry (content-area-relative) */
#define BTN_Y         7
#define BTN_H        22
#define BTN_W        30
#define BTN_BACK_X    8
#define BTN_FWD_X    42
#define BTN_RLD_X    76

/* Address bar (content-area-relative) */
#define ADDR_X      112
#define ADDR_Y        7
#define ADDR_H       22

/* ── NetSurf table ───────────────────────────────────────────────────────── */

static struct netsurf_table g_ns_table = {
    .misc   = &aether_misc_table,
    .window = &aether_window_table,
    .fetch  = &aether_fetch_table,
    .bitmap = &aether_bitmap_table,
    .layout = &aether_layout_table,
};

/* ── Window geometry ─────────────────────────────────────────────────────── */

static int  g_win_x;
static int  g_win_y;
static int  g_win_w;
static int  g_win_h;
static int  g_viewport_h;   /* pixel rows available for NetSurf */
static int  g_addr_w;       /* address bar pixel width */
static long g_win_id = -1;

/* Viewport scroll position — defined in gui_window_stub.c, also updated
 * here on key press and reset to 0 on navigation. */
extern int g_scroll_y;

/* ── Widget tree ─────────────────────────────────────────────────────────── */

static widget_t g_root;
static widget_t g_toolbar;
static widget_t g_btn_back;
static widget_t g_btn_fwd;
static widget_t g_btn_reload;
static widget_t g_addr_input;
static widget_t g_viewport;
static widget_t g_status;

/* ── widget_run context (global so callbacks can set running=0) ──────────── */

static widget_ctx_t g_ctx;

/* ── UART helper ─────────────────────────────────────────────────────────── */

static void uart(const char *s)
{
    long r;
    int len = 0;
    while (s[len]) len++;
    __asm__ volatile(
        "mov x8, #34\n mov x0, #1\n mov x1, %1\n mov x2, %2\n"
        "svc #0\n mov %0, x0\n"
        : "=r"(r) : "r"(s), "r"((long)len) : "x0","x1","x2","x8","memory");
}

/* ── NetSurf viewport render ─────────────────────────────────────────────── */

static void render_viewport(void)
{
    if (!nsaether_pixels || !nsaether_bw) return;

    static int rv_cnt = 0;
    if (rv_cnt++ < 5) {
        char dbg[32];
        snprintf(dbg, sizeof(dbg), "render_viewport: #%d\n", rv_cnt);
        uart(dbg);
    }

    memset(nsaether_pixels, 0xFF,
           (size_t)g_win_w * (size_t)g_viewport_h * 4);

    aether_plot_ctx_t plot_ctx;
    aether_plot_ctx_init(&plot_ctx, nsaether_pixels, g_win_w, g_viewport_h);

    struct redraw_context rctx = {
        .interactive       = true,
        .background_images = true,
        .plot              = &aether_plotter_table,
        .priv              = &plot_ctx,
    };

    struct rect cr = { 0, 0, g_win_w, g_viewport_h };
    browser_window_redraw(nsaether_bw, 0, -g_scroll_y, &cr, &rctx);
}

/* ── Navigation helpers ──────────────────────────────────────────────────── */

static void navigate_to(const char *url_str)
{
    if (!nsaether_bw || !url_str || !url_str[0]) return;

    struct nsurl *nav_url = NULL;
    if (nsurl_create(url_str, &nav_url) != NSERROR_OK || !nav_url) {
        uart("aether_browser: nsurl_create failed\n");
        return;
    }
    g_scroll_y = 0;
    browser_window_navigate(nsaether_bw, nav_url,
                            NULL, BW_NAVIGATE_HISTORY,
                            NULL, NULL, NULL);
    nsurl_unref(nav_url);
}

static void sync_url_to_widget(void)
{
    if (!nsaether_bw) return;
    struct nsurl *url = NULL;
    if (browser_window_get_url(nsaether_bw, false, &url) == NSERROR_OK && url) {
        const char *s = nsurl_access(url);
        if (s) textinput_set_text(&g_addr_input, s);
        nsurl_unref(url);
    }
}

/* ── Custom widget draw functions ────────────────────────────────────────── */

static void toolbar_draw(widget_t *w, int ax, int ay)
{
    (void)w;
    /* Background + separator line 1px below toolbar */
    gfx_fill((unsigned)ax, (unsigned)ay,
              (unsigned)g_win_w, (unsigned)TOOLBAR_H, C_PANEL);
    gfx_hline((unsigned)ax, (unsigned)(ay + TOOLBAR_H),
               (unsigned)g_win_w, C_SEP);
}

static void viewport_draw(widget_t *w, int ax, int ay)
{
    (void)w;
    if (nsaether_pixels)
        gfx_raw_blit(nsaether_pixels, (unsigned)g_win_w,
                     ax, ay,
                     (unsigned)g_win_w, (unsigned)g_viewport_h);
    else
        gfx_fill((unsigned)ax, (unsigned)ay,
                 (unsigned)g_win_w, (unsigned)g_viewport_h, 0xFFFFFFFFu);
}

static int viewport_event(widget_t *w, const widget_event_t *ev)
{
    (void)w;
    if (!nsaether_bw) return 0;

    /* Screen-absolute → viewport-relative coordinates */
    int vx = ev->mx - g_win_x;
    int vy = ev->my - (g_win_y + TITLE_H + VP_OFF);

    switch (ev->type) {
    case WEV_MOUSE_DOWN:
        widget_set_focused(&g_viewport);
        browser_window_mouse_click(nsaether_bw, BROWSER_MOUSE_PRESS_1, vx, vy);
        return 1;
    case WEV_MOUSE_UP:
        {
            char dbg[48];
            snprintf(dbg, sizeof(dbg), "click: vx=%d vy=%d\n", vx, vy);
            uart(dbg);
        }
        browser_window_mouse_click(nsaether_bw, BROWSER_MOUSE_CLICK_1, vx, vy);
        return 1;
    case WEV_MOUSE_MOVE:
        browser_window_mouse_click(nsaether_bw, BROWSER_MOUSE_HOVER, vx, vy);
        return 0;    /* non-consuming: let hover redraw proceed normally */
    case WEV_KEY_DOWN:
        if (ev->modifiers & MOD_CTRL) {
            switch (ev->keycode) {
            case KEY_L:
                widget_set_focused(&g_addr_input);
                return 1;
            case KEY_R:
                browser_window_reload(nsaether_bw, false);
                return 1;
            case KEY_LBRACKET:
                if (browser_window_back_available(nsaether_bw))
                    browser_window_history_back(nsaether_bw, false);
                return 1;
            case KEY_RBRACKET:
                if (browser_window_forward_available(nsaether_bw))
                    browser_window_history_forward(nsaether_bw, false);
                return 1;
            default: break;
            }
        }
        {
            switch (ev->keycode) {
            case KEY_UP:
                if (g_scroll_y > 0) {
                    g_scroll_y -= 40;
                    if (g_scroll_y < 0) g_scroll_y = 0;
                    nsaether_dirty = true;
                }
                return 1;
            case KEY_DOWN:
                g_scroll_y += 40;
                nsaether_dirty = true;
                return 1;
            case KEY_PGUP:
                g_scroll_y -= g_viewport_h;
                if (g_scroll_y < 0) g_scroll_y = 0;
                nsaether_dirty = true;
                return 1;
            case KEY_PGDN:
                g_scroll_y += g_viewport_h;
                nsaether_dirty = true;
                return 1;
            case KEY_HOME:
                g_scroll_y = 0;
                nsaether_dirty = true;
                return 1;
            case KEY_END:
                g_scroll_y += 9999;
                nsaether_dirty = true;
                return 1;
            default: break;
            }
            int ch = (int)(unsigned char)ev->keycode;
            if (ch >= 32 && ch < 127)
                browser_window_key_press(nsaether_bw, (uint32_t)ch);
        }
        return 0;
    default:
        return 0;
    }
}

static void status_draw(widget_t *w, int ax, int ay)
{
    (void)w;
    gfx_hline((unsigned)ax, (unsigned)ay, (unsigned)g_win_w, C_SEP);
    gfx_fill((unsigned)ax, (unsigned)(ay + 1),
              (unsigned)g_win_w, (unsigned)(STATUS_H - 1), C_PANEL);
    const char *txt = nsaether_status[0] ? nsaether_status
                                         : (nsaether_loading ? "Loading..." : "Done");
    int ty = ay + 1 + ((STATUS_H - 1) - (int)gfx_font_height()) / 2;
    gfx_text_transparent(8, (unsigned)ty, txt, C_TEXT_DIM);
}

/* ── Button callbacks ────────────────────────────────────────────────────── */

static void on_back(widget_t *w)
{
    (void)w;
    if (nsaether_bw && browser_window_back_available(nsaether_bw))
        browser_window_history_back(nsaether_bw, false);
}

static void on_fwd(widget_t *w)
{
    (void)w;
    if (nsaether_bw && browser_window_forward_available(nsaether_bw))
        browser_window_history_forward(nsaether_bw, false);
}

static void on_reload(widget_t *w)
{
    (void)w;
    if (nsaether_bw) browser_window_reload(nsaether_bw, false);
}

static void on_addr_submit(widget_t *w)
{
    navigate_to(textinput_get_text(w));
    widget_set_focused(&g_viewport);
}

/* ── Per-frame hook: NetSurf scheduler + viewport sync ───────────────────── */

static void browser_per_frame(void *ud)
{
    (void)ud;

    static bool first_pf = true;
    if (first_pf) { first_pf = false; uart("browser_per_frame: first\n"); }

    nsaether_schedule_drain();
    js_timers_tick();

    if (nsaether_dirty && nsaether_bw) {
        static int bpf_cnt = 0;
        if (bpf_cnt < 5) {
            bpf_cnt++;
            int rdy = browser_window_redraw_ready(nsaether_bw) ? 1 : 0;
            char dbg[32];
            snprintf(dbg, sizeof(dbg), "bpf: dirty rdy=%d\n", rdy);
            uart(dbg);
        }
    }
    if (nsaether_dirty && nsaether_bw &&
            browser_window_redraw_ready(nsaether_bw)) {
        render_viewport();
        sys_sched_yield();          /* compositor refreshes cursor here */
        nsaether_dirty = false;
        if (widget_get_focused() != &g_addr_input)
            sync_url_to_widget();
        widget_invalidate(&g_viewport);
        widget_invalidate(&g_toolbar);
        widget_invalidate(&g_status);
    }

    /* Throbber animation + one-shot load-complete update */
    static int  s_was_loading = 0;
    static long s_last_tick   = 0;
    if (nsaether_loading) {
        long now = gfx_ticks();
        if (now - s_last_tick >= 10) {
            s_last_tick = now;
            g_btn_reload.data.button.text[0] = '.';
            g_btn_reload.data.button.text[1] = '\0';
            widget_invalidate(&g_btn_reload);
            widget_invalidate(&g_status);
            sys_sched_yield();
        }
    } else if (s_was_loading) {
        /* Loading just finished: update nav buttons + reload label */
        g_btn_reload.data.button.text[0] = 'R';
        g_btn_reload.data.button.text[1] = '\0';
        if (nsaether_bw) {
            g_btn_back.state =
                browser_window_back_available(nsaether_bw) ? WS_NORMAL : WS_DISABLED;
            g_btn_fwd.state =
                browser_window_forward_available(nsaether_bw) ? WS_NORMAL : WS_DISABLED;
        }
        widget_invalidate(&g_btn_back);
        widget_invalidate(&g_btn_fwd);
        widget_invalidate(&g_btn_reload);
        widget_invalidate(&g_status);
    }
    s_was_loading = (int)nsaether_loading;
}

/* ── Window chrome (called by widget_run on init + drag) ─────────────────── */

static void draw_chrome(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, g_win_w, g_win_h,
                           TITLE_H, "AetherOS Browser", 0);
}

static void on_reposition(void *ud) { (void)ud; draw_chrome(); }

/* ── Build widget tree ───────────────────────────────────────────────────── */

static void build_ui(void)
{
    int content_h = g_win_h - TITLE_H;
    int status_y  = content_h - STATUS_H;

    /* Root: transparent container covering the content area */
    widget_init(&g_root, WIDGET_PANEL, 0, 0, g_win_w, content_h);
    /* draw_fn = NULL: don't overwrite the glass window border */

    /* Toolbar panel with custom draw (background + separator) */
    widget_init(&g_toolbar, WIDGET_PANEL, 0, 0, g_win_w, TOOLBAR_H + SEPARATOR_H);
    g_toolbar.draw_fn = toolbar_draw;
    widget_add_child(&g_root, &g_toolbar);

    /* Navigation buttons */
    widget_init_button(&g_btn_back,   BTN_BACK_X, BTN_Y, BTN_W, BTN_H, "<",  on_back);
    widget_init_button(&g_btn_fwd,    BTN_FWD_X,  BTN_Y, BTN_W, BTN_H, ">",  on_fwd);
    widget_init_button(&g_btn_reload, BTN_RLD_X,  BTN_Y, BTN_W, BTN_H, "R",  on_reload);
    g_btn_back.state   = WS_DISABLED;
    g_btn_fwd.state    = WS_DISABLED;
    widget_add_child(&g_toolbar, &g_btn_back);
    widget_add_child(&g_toolbar, &g_btn_fwd);
    widget_add_child(&g_toolbar, &g_btn_reload);

    /* Address bar */
    widget_init_textinput(&g_addr_input, ADDR_X, ADDR_Y, g_addr_w, ADDR_H,
                          NULL, on_addr_submit);
    widget_add_child(&g_toolbar, &g_addr_input);

    /* Viewport: custom draw (blit NetSurf pixels) + event forwarding */
    widget_init(&g_viewport, WIDGET_PANEL,
                0, VP_OFF, g_win_w, g_viewport_h);
    g_viewport.draw_fn  = viewport_draw;
    g_viewport.event_fn = viewport_event;
    g_viewport.focusable = 1;
    widget_add_child(&g_root, &g_viewport);

    /* Status bar: custom draw (separator + panel + text) */
    widget_init(&g_status, WIDGET_PANEL, 0, status_y, g_win_w, STATUS_H);
    g_status.draw_fn = status_draw;
    widget_add_child(&g_root, &g_status);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *start_url = (argc > 1) ? argv[1]
                                       : "http://10.0.2.2:8080/index.html";

    uart("aether_browser: starting\n");

    /* 1. Init NetSurf */
    nslog_aether_init();
    if (netsurf_register(&g_ns_table) != NSERROR_OK) {
        uart("aether_browser FAIL: netsurf_register\n");
        return 1;
    }
    if (nsoption_init(NULL, NULL, NULL) != NSERROR_OK) {
        uart("aether_browser FAIL: nsoption_init\n");
        return 1;
    }
    nserror ni = netsurf_init(NULL);
    if (ni != NSERROR_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "aether_browser FAIL: netsurf_init err=%d\n", (int)ni);
        uart(buf);
        return 1;
    }
    fetch_http_aether_register();

    /* Enable JavaScript at runtime — desktop/options.h compiles in false;
       our lib/netsurf_aether/options.h is never seen by nsoption.c */
    nsoption_set_bool(enable_javascript, true);

    /* 2. Screen dimensions and window geometry */
    gfx_init();
    int scr_w = (int)gfx_width();
    int scr_h = (int)gfx_height();

    g_win_x      = 0;
    g_win_y      = TOPBAR_H + ACCENT_H;            /* 38 px */
    g_win_w      = scr_w;
    g_win_h      = scr_h - TOPBAR_H - ACCENT_H - DOCK_H - DOCK_BB_STRIP;  /* 586 px */
    g_viewport_h = g_win_h - TITLE_H - VP_OFF - STATUS_H;
    g_addr_w     = g_win_w - ADDR_X - 10;

    {
        char dbg[80];
        snprintf(dbg, sizeof(dbg),
                 "aether_browser: scr=%dx%d win=%dx%d vp_h=%d\n",
                 scr_w, scr_h, g_win_w, g_win_h, g_viewport_h);
        uart(dbg);
    }

    nsaether_win_w = g_win_w;
    nsaether_win_h = g_viewport_h;

    /* 3. Register WM window (no WM_FLAG_NO_CHROME: init handles
     *    close / minimize / focus / drag) */
    g_win_id = sys_wm_register(g_win_x, g_win_y, g_win_w, g_win_h,
                               "AetherOS Browser");
    if (g_win_id >= 0) {
        /* z=1: above desktop (z=0); init's raise_to_front raises further on click */
        sys_wm_set_zindex(g_win_id, 1);
    }

    /* 4. Draw initial chrome before widget_run allocates the BO */
    draw_chrome();

    /* 5. Build widget tree */
    build_ui();

    /* 6. Create NetSurf browser window + start navigation */
    struct nsurl *start_nsurl = NULL;
    if (nsurl_create(start_url, &start_nsurl) != NSERROR_OK || !start_nsurl) {
        uart("aether_browser FAIL: nsurl_create\n");
        netsurf_exit();
        return 1;
    }
    struct browser_window *bw = NULL;
    nserror be = browser_window_create(
            BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
            start_nsurl, NULL, NULL, &bw);
    nsurl_unref(start_nsurl);
    if (be != NSERROR_OK || !bw) {
        uart("aether_browser FAIL: browser_window_create\n");
        netsurf_exit();
        return 1;
    }

    /* Pre-fill address bar with the start URL */
    textinput_set_text(&g_addr_input, start_url);

    /* Default focus: viewport (allows keyboard scroll without clicking) */
    widget_set_focused(&g_viewport);

    uart("aether_browser: starting widget_run\n");

    /* 7. Run widget event loop */
    g_ctx.win_x         = &g_win_x;
    g_ctx.win_y         = &g_win_y;
    g_ctx.content_dx    = 0;
    g_ctx.content_dy    = TITLE_H;
    g_ctx.win_id        = (int)g_win_id;
    g_ctx.win_w         = g_win_w;
    g_ctx.win_h         = g_win_h;
    g_ctx.on_reposition = on_reposition;
    g_ctx.per_frame_fn  = browser_per_frame;
    g_ctx.userdata      = NULL;
    g_ctx.running       = 1;

    widget_run(&g_root, &g_ctx);

    /* 8. Cleanup */
    netsurf_exit();
    sys_wm_request_close(g_win_id);
    return 0;
}
