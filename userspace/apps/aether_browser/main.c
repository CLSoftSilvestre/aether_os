/*
 * Phase 7.8 — aether_browser MVP
 *
 * Full browser application integrating:
 *   - Lumina glassmorphism window chrome
 *   - Toolbar: Back / Forward / Reload buttons + address bar
 *   - Viewport: NetSurf → aether_plotter_table → gfx_raw_blit
 *   - Status bar: NetSurf status messages
 *   - Event loop: WM events → toolbar hit-test + NetSurf input
 *
 * Test inside AetherOS:
 *   aether_browser                        → http://10.0.2.2:8080/index.html
 *   aether_browser http://example.com/    → navigate to given URL
 *
 * QEMU: python3 -m http.server 8080 --directory tests/browser/ (on host Mac)
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
#include "sys.h"
#include "input.h"

/* ── externs from bridge ─────────────────────────────────────────────────── */

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

/* ── Layout constants ────────────────────────────────────────────────────── */

#define WIN_W       1024
#define WIN_H        700
#define TITLE_H       28
#define TOOLBAR_H     36
#define SEPARATOR_H    1
#define STATUS_H      20

/* VIEWPORT_Y is where the content area begins (below title + toolbar + sep) */
#define VIEWPORT_Y  (TITLE_H + TOOLBAR_H + SEPARATOR_H)
#define VIEWPORT_W  WIN_W
#define VIEWPORT_H  (WIN_H - VIEWPORT_Y - STATUS_H)   /* 615 */

/* Initial window position: below topbar (36px) + accent (2px) + margin */
#define WIN_X_INIT   80
#define WIN_Y_INIT   50

/* Toolbar button geometry (toolbar-relative coords) */
#define BTN_Y         7
#define BTN_H        22
#define BTN_W        30

#define BTN_BACK_X    8
#define BTN_FWD_X    42
#define BTN_RLD_X    76

/* Address bar (toolbar-relative) */
#define ADDR_X      112
#define ADDR_Y        7
#define ADDR_H       22
#define ADDR_W      (WIN_W - ADDR_X - 10)   /* 902 */

/* Close button (window-relative, drawn by gfx_glass_window_frame at wx+10) */
#define CLOSE_REL_X  10
#define CLOSE_REL_Y   8
#define CLOSE_SIZE   12

/* ── NetSurf table ───────────────────────────────────────────────────────── */

static struct netsurf_table g_ns_table = {
    .misc   = &aether_misc_table,
    .window = &aether_window_table,
    .fetch  = &aether_fetch_table,
    .bitmap = &aether_bitmap_table,
    .layout = &aether_layout_table,
};

/* ── Window state ────────────────────────────────────────────────────────── */

static int  g_win_x = WIN_X_INIT;
static int  g_win_y = WIN_Y_INIT;
static long g_win_id = -1;
static int  g_running = 1;

/* ── Address bar state ───────────────────────────────────────────────────── */

static char g_url[512];
static int  g_url_len;
static int  g_url_cursor;
static int  g_url_focused;

/* ── Mouse tracking ──────────────────────────────────────────────────────── */

static unsigned g_prev_btns = 0;
static int      g_hover_close = 0;   /* 1 = cursor over close button */

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

/* ── Drawing helpers ─────────────────────────────────────────────────────── */

static void draw_btn(int rel_x, int rel_y, const char *label, int active)
{
    int ax = g_win_x + rel_x;
    int ay = g_win_y + TITLE_H + rel_y;
    unsigned bg = active ? GFX_RGB(52, 48, 90) : GFX_RGB(34, 32, 58);
    unsigned bd = active ? C_ACCENT : C_SEP;
    gfx_fill_rounded((unsigned)ax, (unsigned)ay,
                     (unsigned)BTN_W, (unsigned)BTN_H, GFX_WIDGET_R, bg);
    gfx_rect_rounded((unsigned)ax, (unsigned)ay,
                     (unsigned)BTN_W, (unsigned)BTN_H, GFX_WIDGET_R, bd);
    /* Center label in button */
    int lw = gfx_text_width(label);
    int tx = ax + (BTN_W - lw) / 2;
    int ty = ay + (BTN_H - gfx_font_height()) / 2;
    gfx_text_transparent((unsigned)tx, (unsigned)ty, label,
                         active ? C_TEXT : C_TEXT_DIM);
}

static void draw_toolbar(void)
{
    int ty = g_win_y + TITLE_H;

    /* Toolbar background */
    gfx_fill((unsigned)g_win_x, (unsigned)ty,
              (unsigned)WIN_W, (unsigned)TOOLBAR_H, C_PANEL);

    /* Separator line below toolbar */
    gfx_hline((unsigned)g_win_x,
               (unsigned)(ty + TOOLBAR_H),
               (unsigned)WIN_W, C_SEP);

    /* Nav buttons */
    int back_on  = nsaether_bw && browser_window_back_available(nsaether_bw);
    int fwd_on   = nsaether_bw && browser_window_forward_available(nsaether_bw);
    int rld_on   = nsaether_bw && browser_window_reload_available(nsaether_bw);
    draw_btn(BTN_BACK_X, BTN_Y, "<", back_on);
    draw_btn(BTN_FWD_X,  BTN_Y, ">", fwd_on);
    draw_btn(BTN_RLD_X,  BTN_Y, nsaether_loading ? "." : "R", rld_on);

    /* Address bar box */
    int ax = g_win_x + ADDR_X;
    int ay = ty + ADDR_Y;
    unsigned addr_bg = g_url_focused ? GFX_RGB(18, 18, 30) : GFX_RGB(12, 12, 22);
    unsigned addr_bd = g_url_focused ? C_ACCENT : C_SEP;
    gfx_fill_rounded((unsigned)ax, (unsigned)ay,
                     (unsigned)ADDR_W, (unsigned)ADDR_H, GFX_INPUT_R, addr_bg);
    gfx_rect_rounded((unsigned)ax, (unsigned)ay,
                     (unsigned)ADDR_W, (unsigned)ADDR_H, GFX_INPUT_R, addr_bd);

    /* URL text (clip to ADDR_W - 12px padding) */
    int text_x = ax + 6;
    int text_y = ay + (ADDR_H - gfx_font_height()) / 2;
    gfx_text((unsigned)text_x, (unsigned)text_y, g_url, C_TEXT, addr_bg);

    /* Cursor when focused */
    if (g_url_focused) {
        int cx    = gfx_text_prefix_width(g_url, g_url_cursor);
        int cur_x = text_x + cx;
        int cur_y = ay + 4;
        int cur_h = ADDR_H - 8;
        gfx_vline((unsigned)cur_x, (unsigned)cur_y, (unsigned)cur_h, C_TEXT);
    }
}

static void draw_status(void)
{
    int sy = g_win_y + WIN_H - STATUS_H;
    gfx_fill((unsigned)g_win_x, (unsigned)sy,
              (unsigned)WIN_W, (unsigned)STATUS_H, C_PANEL);
    gfx_hline((unsigned)g_win_x, (unsigned)sy, (unsigned)WIN_W, C_SEP);

    const char *txt = nsaether_status[0] ? nsaether_status
                                         : (nsaether_loading ? "Loading..." : "Done");
    int ty = sy + (STATUS_H - gfx_font_height()) / 2;
    gfx_text_transparent((unsigned)(g_win_x + 8), (unsigned)ty, txt, C_TEXT_DIM);
}

static void draw_chrome(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H,
                            TITLE_H, "AetherOS Browser", g_hover_close);
    draw_toolbar();
    draw_status();
}

static void draw_viewport(void)
{
    if (!nsaether_pixels || !nsaether_bw) return;

    memset(nsaether_pixels, 0xFF,
           (size_t)VIEWPORT_W * (size_t)VIEWPORT_H * 4);

    aether_plot_ctx_t plot_ctx;
    aether_plot_ctx_init(&plot_ctx, nsaether_pixels, VIEWPORT_W, VIEWPORT_H);

    struct redraw_context rctx = {
        .interactive       = true,
        .background_images = true,
        .plot              = &aether_plotter_table,
        .priv              = &plot_ctx,
    };

    struct rect content_rect = { 0, 0, VIEWPORT_W, VIEWPORT_H };
    browser_window_redraw(nsaether_bw, 0, 0, &content_rect, &rctx);

    gfx_raw_blit(nsaether_pixels, (unsigned)VIEWPORT_W,
                 g_win_x, g_win_y + VIEWPORT_Y,
                 (unsigned)VIEWPORT_W, (unsigned)VIEWPORT_H);
}

/* ── Navigation ──────────────────────────────────────────────────────────── */

static void navigate_to(const char *url_str)
{
    if (!nsaether_bw || !url_str || !url_str[0]) return;

    struct nsurl *nsurl = NULL;
    if (nsurl_create(url_str, &nsurl) != NSERROR_OK || !nsurl) {
        uart("aether_browser: nsurl_create failed\n");
        return;
    }
    browser_window_navigate(nsaether_bw, nsurl,
                            NULL, BW_NAVIGATE_HISTORY,
                            NULL, NULL, NULL);
    nsurl_unref(nsurl);

    /* Optimistic status update — NetSurf will overwrite via set_status */
    snprintf(nsaether_status, sizeof(nsaether_status), "Loading...");
    draw_toolbar();
    draw_status();
}

static void sync_url_from_ns(void)
{
    /* Copy NetSurf's current URL into our address bar buffer */
    if (!nsaether_bw) return;
    struct nsurl *url = NULL;
    if (browser_window_get_url(nsaether_bw, false, &url) == NSERROR_OK && url) {
        const char *s = nsurl_access(url);
        if (s) {
            size_t i = 0;
            while (s[i] && i < sizeof(g_url) - 1) { g_url[i] = s[i]; i++; }
            g_url[i] = '\0';
            g_url_len = (int)i;
            g_url_cursor = g_url_len;
        }
        nsurl_unref(url);
    }
}

/* ── Address bar text input ──────────────────────────────────────────────── */

static int keycode_to_char(const key_event_t *ke)
{
    static const char base[] = {
        /* A–Z → a–z (0x01..0x1A) */
        'a','b','c','d','e','f','g','h','i','j','k','l','m',
        'n','o','p','q','r','s','t','u','v','w','x','y','z',
        /* KEY_0..9 → '0'..'9' */
        '0','1','2','3','4','5','6','7','8','9',
    };
    static const char shifted[] = {
        'A','B','C','D','E','F','G','H','I','J','K','L','M',
        'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
        ')','!','@','#','$','%','^','&','*','(',
    };

    keycode_t k = ke->keycode;
    int shift = (ke->modifiers & MOD_SHIFT) || (ke->modifiers & MOD_CAPS);

    if (k >= KEY_A && k <= KEY_9) {
        return shift ? (unsigned char)shifted[k - KEY_A]
                     : (unsigned char)base[k - KEY_A];
    }
    if (k == KEY_SPACE)       return ' ';
    if (k == KEY_DOT)         return shift ? '>' : '.';
    if (k == KEY_SLASH)       return shift ? '?' : '/';
    if (k == KEY_SEMICOLON)   return shift ? ':' : ';';
    if (k == KEY_MINUS)       return shift ? '_' : '-';
    if (k == KEY_EQUALS)      return shift ? '+' : '=';
    return 0;
}

static void handle_addr_key(const key_event_t *ke)
{
    if (!ke->is_press) return;

    switch (ke->keycode) {

    case KEY_ENTER:
        navigate_to(g_url);
        g_url_focused = 0;
        break;

    case KEY_ESC:
        /* Restore URL from current page */
        sync_url_from_ns();
        g_url_focused = 0;
        break;

    case KEY_BACKSPACE:
        if (g_url_cursor > 0) {
            memmove(g_url + g_url_cursor - 1,
                    g_url + g_url_cursor,
                    (size_t)(g_url_len - g_url_cursor) + 1);
            g_url_cursor--;
            g_url_len--;
        }
        break;

    case KEY_DELETE:
        if (g_url_cursor < g_url_len) {
            memmove(g_url + g_url_cursor,
                    g_url + g_url_cursor + 1,
                    (size_t)(g_url_len - g_url_cursor));
            g_url_len--;
            g_url[g_url_len] = '\0';
        }
        break;

    case KEY_LEFT:
        if (g_url_cursor > 0) g_url_cursor--;
        break;
    case KEY_RIGHT:
        if (g_url_cursor < g_url_len) g_url_cursor++;
        break;
    case KEY_HOME:
        g_url_cursor = 0;
        break;
    case KEY_END:
        g_url_cursor = g_url_len;
        break;

    default: {
        int ch = keycode_to_char(ke);
        if (ch && g_url_len < (int)(sizeof(g_url) - 1)) {
            memmove(g_url + g_url_cursor + 1,
                    g_url + g_url_cursor,
                    (size_t)(g_url_len - g_url_cursor) + 1);
            g_url[g_url_cursor++] = (char)ch;
            g_url_len++;
        }
        break;
    }
    } /* switch */

    draw_toolbar();
}

/* ── Viewport keyboard (scroll) ──────────────────────────────────────────── */

static void handle_viewport_key(const key_event_t *ke)
{
    if (!ke->is_press || !nsaether_bw) return;

    int cx = VIEWPORT_W / 2;
    int cy = VIEWPORT_H / 2;
    int step = 48;

    /* Map AetherOS keycodes to NetSurf scroll */
    switch (ke->keycode) {
    case KEY_UP:
        browser_window_scroll_at_point(nsaether_bw, cx, cy, 0, -step);
        break;
    case KEY_DOWN:
        browser_window_scroll_at_point(nsaether_bw, cx, cy, 0,  step);
        break;
    case KEY_LEFT:
        browser_window_scroll_at_point(nsaether_bw, cx, cy, -step, 0);
        break;
    case KEY_RIGHT:
        browser_window_scroll_at_point(nsaether_bw, cx, cy,  step, 0);
        break;
    case KEY_PGUP:
        browser_window_scroll_at_point(nsaether_bw, cx, cy, 0, -VIEWPORT_H);
        break;
    case KEY_PGDN:
        browser_window_scroll_at_point(nsaether_bw, cx, cy, 0,  VIEWPORT_H);
        break;
    default: {
        /* Pass printable characters into NetSurf (for form fields, etc.) */
        int ch = keycode_to_char(ke);
        if (ch) browser_window_key_press(nsaether_bw, (uint32_t)ch);
        break;
    }
    }
}

/* ── Hit-test helpers ────────────────────────────────────────────────────── */

static int in_rect(int px, int py, int rx, int ry, int rw, int rh)
{
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

/* ── Mouse handler ───────────────────────────────────────────────────────── */

static void handle_mouse(mouse_event_t me)
{
    int mx = (int)me.x, my = (int)me.y;
    unsigned btns = me.buttons;
    int click = (btns & 1) && !(g_prev_btns & 1);   /* left rising edge */
    int release = !(btns & 1) && (g_prev_btns & 1); /* left falling edge */
    g_prev_btns = btns;

    /* Close button hover */
    int new_hover = in_rect(mx, my,
                            g_win_x + CLOSE_REL_X, g_win_y + CLOSE_REL_Y,
                            CLOSE_SIZE, CLOSE_SIZE);
    if (new_hover != g_hover_close) {
        g_hover_close = new_hover;
        /* Redraw title bar with updated hover state */
        gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H,
                                TITLE_H, "AetherOS Browser", g_hover_close);
    }

    /* Close button click */
    if (click && g_hover_close) {
        g_running = 0;
        return;
    }

    /* Toolbar area */
    int ty = g_win_y + TITLE_H;
    if (my >= ty && my < ty + TOOLBAR_H) {
        if (!click) return;
        int rx = mx - g_win_x;
        int ry = my - ty;

        if (in_rect(rx, ry, BTN_BACK_X, BTN_Y, BTN_W, BTN_H)) {
            if (nsaether_bw && browser_window_back_available(nsaether_bw))
                browser_window_history_back(nsaether_bw, false);
        } else if (in_rect(rx, ry, BTN_FWD_X, BTN_Y, BTN_W, BTN_H)) {
            if (nsaether_bw && browser_window_forward_available(nsaether_bw))
                browser_window_history_forward(nsaether_bw, false);
        } else if (in_rect(rx, ry, BTN_RLD_X, BTN_Y, BTN_W, BTN_H)) {
            if (nsaether_bw) browser_window_reload(nsaether_bw, false);
        } else if (in_rect(rx, ry, ADDR_X, ADDR_Y, ADDR_W, ADDR_H)) {
            g_url_focused = 1;
            g_url_cursor  = g_url_len;
            draw_toolbar();
        }
        return;
    }

    /* Viewport area */
    int vp_top    = g_win_y + VIEWPORT_Y;
    int vp_bottom = g_win_y + WIN_H - STATUS_H;
    if (my >= vp_top && my < vp_bottom && nsaether_bw) {
        int vx = mx - g_win_x;
        int vy = my - vp_top;

        if (click) {
            g_url_focused = 0;
            /* PRESS on button down */
            browser_window_mouse_click(nsaether_bw,
                                       BROWSER_MOUSE_PRESS_1, vx, vy);
        } else if (release) {
            /* CLICK on button up (completes the click) */
            browser_window_mouse_click(nsaether_bw,
                                       BROWSER_MOUSE_CLICK_1, vx, vy);
        } else {
            /* Hover — NetSurf updates status with link URL */
            browser_window_mouse_click(nsaether_bw,
                                       BROWSER_MOUSE_HOVER, vx, vy);
        }
    }
}

/* ── Keyboard handler ────────────────────────────────────────────────────── */

static void handle_key(const key_event_t *ke)
{
    if (!ke->is_press) return;

    /* Global shortcuts */
    if (ke->modifiers & MOD_CTRL) {
        switch (ke->keycode) {
        case KEY_L:
            /* Focus address bar and select all */
            g_url_focused = 1;
            g_url_cursor  = g_url_len;
            draw_toolbar();
            return;
        case KEY_R:
            if (nsaether_bw) browser_window_reload(nsaether_bw, false);
            return;
        case KEY_LBRACKET:  /* Ctrl+[ = back */
            if (nsaether_bw && browser_window_back_available(nsaether_bw))
                browser_window_history_back(nsaether_bw, false);
            return;
        case KEY_RBRACKET:  /* Ctrl+] = forward */
            if (nsaether_bw && browser_window_forward_available(nsaether_bw))
                browser_window_history_forward(nsaether_bw, false);
            return;
        default: break;
        }
    }

    if (g_url_focused) {
        handle_addr_key(ke);
    } else {
        handle_viewport_key(ke);
    }
}

/* ── WM event handler ────────────────────────────────────────────────────── */

static void handle_wm_event(unsigned long long ev)
{
    if (ev == 0) return;

    /* Window dragged to new position */
    if (wm_event_is_redraw(ev)) {
        g_win_x = wm_event_redraw_x(ev);
        g_win_y = wm_event_redraw_y(ev);
        draw_chrome();
        draw_viewport();
        return;
    }

    /* Mouse forwarded by compositor */
    if (wm_event_is_mouse(ev)) {
        mouse_event_t me = wm_event_mouse_unpack(ev);
        handle_mouse(me);
        return;
    }

    /* Compositor WM events */
    unsigned type = (unsigned)(ev >> 32) & 0xFFu;
    switch (type) {
    case WM_EV_CLOSE_REQUEST:
        g_running = 0;
        break;
    default:
        break;
    }
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
        snprintf(buf, sizeof(buf), "aether_browser FAIL: netsurf_init err=%d\n", (int)ni);
        uart(buf);
        return 1;
    }
    fetch_http_aether_register();

    /* 2. Framebuffer + window */
    gfx_init();

    int sw = (int)gfx_width();
    int sh = (int)gfx_height();

    /* Center window on screen */
    g_win_x = (sw - WIN_W) / 2;
    if (g_win_x < 0) g_win_x = 0;
    g_win_y = WIN_Y_INIT;
    if (g_win_y + WIN_H > sh) g_win_y = sh - WIN_H;
    if (g_win_y < 0) g_win_y = 0;

    /* Configure NetSurf viewport size */
    nsaether_win_w = VIEWPORT_W;
    nsaether_win_h = VIEWPORT_H;

    /* Register WM window */
    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "AetherOS Browser");

    /* 3. Pre-load initial URL into address bar */
    size_t ui = 0;
    while (start_url[ui] && ui < sizeof(g_url) - 1) {
        g_url[ui] = start_url[ui]; ui++;
    }
    g_url[ui] = '\0';
    g_url_len = (int)ui;
    g_url_cursor = g_url_len;

    /* 4. Draw initial chrome */
    draw_chrome();
    gfx_fill((unsigned)g_win_x, (unsigned)(g_win_y + VIEWPORT_Y),
              (unsigned)VIEWPORT_W, (unsigned)VIEWPORT_H, 0x00FFFFFF);

    /* 5. Create browser window + navigate */
    struct nsurl *nsurl = NULL;
    if (nsurl_create(start_url, &nsurl) != NSERROR_OK || !nsurl) {
        uart("aether_browser FAIL: nsurl_create\n");
        netsurf_exit();
        return 1;
    }

    struct browser_window *bw = NULL;
    nserror be = browser_window_create(
            BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
            nsurl, NULL, NULL, &bw);
    nsurl_unref(nsurl);

    if (be != NSERROR_OK || !bw) {
        uart("aether_browser FAIL: browser_window_create\n");
        netsurf_exit();
        return 1;
    }

    uart("aether_browser: event loop started\n");

    /* 6. Event loop */
    long last_toolbar_tick = 0;

    while (g_running) {

        /* Drain NetSurf cooperative scheduler */
        nsaether_schedule_drain();

        /* Redraw viewport when content changed */
        if (nsaether_dirty && nsaether_bw && browser_window_redraw_ready(nsaether_bw)) {
            draw_viewport();
            nsaether_dirty = false;

            /* Sync address bar from NetSurf (URL may have changed on navigate) */
            if (!g_url_focused) {
                sync_url_from_ns();
                draw_toolbar();
            }
            draw_status();
        }

        /* Poll WM events (reposition, mouse, close) */
        unsigned long long ev;
        while ((ev = sys_wm_event_poll()) != 0)
            handle_wm_event(ev);

        /* Poll keyboard */
        unsigned long long kev;
        while ((kev = sys_key_poll()) != 0) {
            key_event_t ke = key_event_unpack(kev);
            handle_key(&ke);
        }

        /* Periodic toolbar refresh (throbber animation, button state) */
        long now = gfx_ticks();
        if (now - last_toolbar_tick >= 10) {   /* ~100 ms */
            last_toolbar_tick = now;
            if (nsaether_loading || !g_url_focused) {
                draw_toolbar();
                draw_status();
            }
        }

        sys_sched_yield();
    }

    /* 7. Cleanup */
    netsurf_exit();
    sys_wm_request_close(g_win_id);
    return 0;
}
