/*
 * Phase 7.8 / Iteration 4 — aether_browser (compositor WM, libwidget chrome, tabs)
 *
 * Window chrome is drawn by on_reposition (gfx_glass_window_frame).
 * init handles close / minimize / focus / drag — no WM_FLAG_NO_CHROME.
 * Widget tree covers the content area below the title bar:
 *
 *   [0 .. TABBAR_H-1]           tab bar (tab pills + new-tab +)
 *   [TABBAR_H .. TABBAR_H+TOOLBAR_H-1]  toolbar (< > R [address])
 *   [TABBAR_H+TOOLBAR_H]        1-px separator
 *   [VP_OFF ..]                 viewport (NetSurf pixel blit)
 *   [.. win_h-TITLE_H-STATUS_H .. win_h-TITLE_H-1]  status bar
 *
 * Tabs (Iteration 4):
 *   Up to MAX_TABS tabs, each with its own browser_window* and pixel buffer.
 *   nsaether_bw / nsaether_pixels are swapped to the active tab on switch.
 *   Ctrl+T = new tab, Ctrl+W = close tab, Ctrl+1..9 = switch.
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

/* Iteration 5: persistence (bookmarks + history + cookies + downloads) */
#include "persistence.h"
#include "cookies_aether.h"
#include "downloads_aether.h"

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
extern void js_handle_mouse_click(int x, int y);

#ifdef AETHER_TLS_ENABLED
extern void tls_global_init(void);
#endif

/* ── Shell layout constants (must match topbar / dock) ───────────────────── */

#define TOPBAR_H      36
#define ACCENT_H       2
#define DOCK_H        56
#define DOCK_BB_STRIP 40

/* ── Window chrome constants ─────────────────────────────────────────────── */

#define TITLE_H       28
#define TABBAR_H      28    /* Iteration 4: tab bar height */
#define TOOLBAR_H     36
#define SEPARATOR_H    1
#define STATUS_H      20

/* Y offset from content-area top to viewport (tab bar + toolbar + separator) */
#define VP_OFF  (TABBAR_H + TOOLBAR_H + SEPARATOR_H)   /* 65 */

/* Toolbar button geometry (parent-relative: child of g_toolbar) */
#define BTN_Y         7
#define BTN_H        22
#define BTN_W        30
#define BTN_BACK_X    8
#define BTN_FWD_X    42
#define BTN_RLD_X    76

/* Address bar (parent-relative: child of g_toolbar) */
#define ADDR_X      112
#define ADDR_Y        7
#define ADDR_H       22

/* Bookmark star — drawn manually in toolbar_draw, right of address bar */
#define BTN_STAR_W   28   /* pixel width of the star hit area */

/* ── Tab constants ───────────────────────────────────────────────────────── */

#define MAX_TABS      8
#define TAB_W_MAX   160
#define TAB_W_MIN    60
#define TAB_CLOSE_W  18    /* hit area for "×" at right edge of each pill */
#define TAB_NEW_W    28    /* hit area for "+" new-tab button */

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

/* Viewport scroll position — defined in gui_window_stub.c, reset on navigate. */
extern int g_scroll_y;

/* ── Tab state (Iteration 4) ─────────────────────────────────────────────── */

typedef struct {
    struct browser_window *bw;
    uint32_t              *pixels;   /* saved from nsaether_pixels at create time */
    char                   title[64];
} tab_t;

static tab_t g_tabs[MAX_TABS];
static int   g_tab_count  = 0;
static int   g_active_tab = 0;

/* ── Widget tree ─────────────────────────────────────────────────────────── */

static widget_t g_root;
static widget_t g_tabbar;
static widget_t g_toolbar;
static widget_t g_btn_back;
static widget_t g_btn_fwd;
static widget_t g_btn_reload;
static widget_t g_addr_input;
static widget_t g_viewport;
static widget_t g_status;

/* ── widget_run context (global so callbacks can set running=0) ──────────── */

static widget_ctx_t g_ctx;

/* ── Download status message (I5.4) ─────────────────────────────────────── */

static char g_dl_msg[128]  = "";
static int  g_dl_msg_ticks = 0;   /* frames remaining to show message */

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

/* ── Tab helpers ─────────────────────────────────────────────────────────── */

static void tab_title_from_url(const char *url, char *out, int maxlen)
{
    if (!url || !out || maxlen <= 0) return;
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://",  7) == 0) p += 7;
    if (strncmp(p, "www.", 4) == 0) p += 4;
    int i = 0;
    while (p[i] && i < maxlen - 1) { out[i] = p[i]; i++; }
    out[i] = '\0';
}

/* Draw text clipped to max_w pixels (in-place truncation). */
static void draw_text_clipped(int x, int y, const char *s,
                               unsigned color, int max_w)
{
    if (!s || !s[0] || max_w <= 0) return;
    int slen = 0;
    while (s[slen]) slen++;
    int n = slen;
    while (n > 0 && gfx_text_prefix_width(s, n) > max_w) n--;
    if (n <= 0) return;
    char buf[65];
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) buf[i] = s[i];
    buf[n] = '\0';
    gfx_text_transparent((unsigned)x, (unsigned)y, buf, color);
}

/* Compute the pixel width of each tab pill given current g_win_w and count. */
static int tab_pill_width(void)
{
    if (g_tab_count <= 0) return TAB_W_MAX;
    int avail = g_win_w - TAB_NEW_W;
    int w = avail / g_tab_count;
    if (w > TAB_W_MAX) w = TAB_W_MAX;
    if (w < TAB_W_MIN) w = TAB_W_MIN;
    return w;
}

static void tab_switch(int idx)
{
    if (idx < 0 || idx >= g_tab_count) return;
    g_active_tab    = idx;
    nsaether_bw     = g_tabs[idx].bw;
    nsaether_pixels = g_tabs[idx].pixels;
    g_scroll_y      = 0;
    nsaether_dirty  = true;

    /* Sync address bar to this tab's URL */
    struct nsurl *url = NULL;
    if (nsaether_bw &&
        browser_window_get_url(nsaether_bw, false, &url) == NSERROR_OK && url) {
        const char *s = nsurl_access(url);
        if (s) textinput_set_text(&g_addr_input, s);
        nsurl_unref(url);
    }

    widget_invalidate(&g_tabbar);
    widget_invalidate(&g_toolbar);
    widget_invalidate(&g_viewport);
    widget_invalidate(&g_status);
}

static int tab_new(const char *url_str)
{
    if (g_tab_count >= MAX_TABS) return -1;

    struct nsurl *nav_url = NULL;
    if (nsurl_create(url_str, &nav_url) != NSERROR_OK || !nav_url) return -1;

    struct browser_window *bw = NULL;
    nserror e = browser_window_create(
            BW_CREATE_HISTORY | BW_CREATE_FOREGROUND,
            nav_url, NULL, NULL, &bw);
    nsurl_unref(nav_url);
    if (e != NSERROR_OK || !bw) return -1;

    int idx = g_tab_count++;
    g_tabs[idx].bw     = bw;
    g_tabs[idx].pixels = nsaether_pixels;   /* set by aether_window_create callback */
    tab_title_from_url(url_str, g_tabs[idx].title, sizeof(g_tabs[idx].title));

    tab_switch(idx);
    textinput_set_text(&g_addr_input, url_str);
    return idx;
}

static void tab_close(int idx)
{
    if (idx < 0 || idx >= g_tab_count) return;

    if (g_tab_count <= 1) {
        /* Last tab — exit browser */
        g_ctx.running = 0;
        return;
    }

    struct browser_window *doomed = g_tabs[idx].bw;

    /* Remove from array */
    for (int i = idx; i < g_tab_count - 1; i++)
        g_tabs[i] = g_tabs[i + 1];
    g_tab_count--;

    if (g_active_tab >= g_tab_count) g_active_tab = g_tab_count - 1;
    else if (g_active_tab > idx)     g_active_tab--;

    tab_switch(g_active_tab);

    /* Destroy the removed tab's BW (aether_window_destroy won't null globals
     * because nsaether_bw was already swapped to the new active tab). */
    browser_window_destroy(doomed);
}

/* ── NetSurf viewport render ─────────────────────────────────────────────── */

static void render_viewport(void)
{
    if (!nsaether_pixels || !nsaether_bw) return;

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
        if (s) {
            textinput_set_text(&g_addr_input, s);
            tab_title_from_url(s, g_tabs[g_active_tab].title,
                               sizeof(g_tabs[g_active_tab].title));
            widget_invalidate(&g_tabbar);
            widget_invalidate(&g_toolbar);   /* refresh star bookmark indicator */
        }
        nsurl_unref(url);
    }
}

/* ── Custom widget draw functions ────────────────────────────────────────── */

static void tabbar_draw(widget_t *w, int ax, int ay)
{
    (void)w;
    int tab_w = tab_pill_width();

    /* Background */
    gfx_fill((unsigned)ax, (unsigned)ay,
              (unsigned)g_win_w, (unsigned)TABBAR_H, C_WIN_BG);

    for (int i = 0; i < g_tab_count; i++) {
        int tx = ax + i * tab_w;
        bool active = (i == g_active_tab);

        /* Pill background */
        unsigned int bg = active ? C_PANEL : C_WIN_BG;
        gfx_fill((unsigned)tx, (unsigned)ay,
                 (unsigned)tab_w, (unsigned)TABBAR_H, bg);

        /* Right-edge divider for inactive tabs */
        if (!active)
            gfx_vline((unsigned)(tx + tab_w - 1),
                      (unsigned)(ay + 4),
                      (unsigned)(TABBAR_H - 8), C_SEP);

        /* Tab title (clipped) */
        const char *title = g_tabs[i].title[0] ? g_tabs[i].title : "New Tab";
        int text_x = tx + 6;
        int text_y = ay + (TABBAR_H - (int)gfx_font_height()) / 2;
        int text_max_w = tab_w - TAB_CLOSE_W - 10;
        unsigned int tc = active ? C_TEXT : C_TEXT_DIM;
        draw_text_clipped(text_x, text_y, title, tc, text_max_w);

        /* Close button "x" */
        int cx = tx + tab_w - TAB_CLOSE_W + 2;
        gfx_text_transparent((unsigned)cx, (unsigned)text_y, "x",
                             active ? C_TEXT_DIM : C_SEP);
    }

    /* "+" new-tab button */
    {
        int nx = ax + g_tab_count * tab_w;
        int ny = ay + (TABBAR_H - (int)gfx_font_height()) / 2;
        gfx_text_transparent((unsigned)(nx + 7), (unsigned)ny, "+", C_TEXT_DIM);
    }

    /* Bottom separator — active tab's pill merges visually with toolbar */
    gfx_hline((unsigned)ax, (unsigned)(ay + TABBAR_H - 1),
               (unsigned)g_win_w, C_SEP);
}

static int tabbar_event(widget_t *w, const widget_event_t *ev)
{
    (void)w;
    if (ev->type != WEV_MOUSE_DOWN) return 0;

    int mx = ev->mx - g_win_x;   /* content-area-relative x */
    int my = ev->my - (g_win_y + TITLE_H);  /* content-area-relative y */
    if (my < 0 || my >= TABBAR_H) return 0;

    int tab_w = tab_pill_width();

    /* "+" new-tab button */
    int new_btn_x = g_tab_count * tab_w;
    if (mx >= new_btn_x && mx < new_btn_x + TAB_NEW_W) {
        tab_new("http://info.cern.ch");
        return 1;
    }

    /* Tab pills */
    for (int i = 0; i < g_tab_count; i++) {
        int tx = i * tab_w;
        if (mx < tx || mx >= tx + tab_w) continue;
        int close_x = tx + tab_w - TAB_CLOSE_W;
        if (mx >= close_x)
            tab_close(i);
        else
            tab_switch(i);
        return 1;
    }
    return 0;
}

/* ── Bookmark helpers ────────────────────────────────────────────────────── */

static int current_url_bookmarked(void)
{
    if (!nsaether_bw) return 0;
    struct nsurl *url = NULL;
    if (browser_window_get_url(nsaether_bw, false, &url) != NSERROR_OK || !url)
        return 0;
    int idx = bmarks_find(nsurl_access(url));
    nsurl_unref(url);
    return idx >= 0 ? 1 : 0;
}

static void star_toggle(void)
{
    if (!nsaether_bw) return;
    struct nsurl *url = NULL;
    if (browser_window_get_url(nsaether_bw, false, &url) != NSERROR_OK || !url)
        return;
    const char *s = nsurl_access(url);
    int idx = bmarks_find(s);
    if (idx >= 0)
        bmarks_remove(idx);
    else
        bmarks_add(s, g_tabs[g_active_tab].title);
    nsurl_unref(url);
    widget_invalidate(&g_toolbar);
}

static void toolbar_draw(widget_t *w, int ax, int ay)
{
    (void)w;
    gfx_fill((unsigned)ax, (unsigned)ay,
              (unsigned)g_win_w, (unsigned)TOOLBAR_H, C_PANEL);
    gfx_hline((unsigned)ax, (unsigned)(ay + TOOLBAR_H),
               (unsigned)g_win_w, C_SEP);

    /* Bookmark star — right of address bar */
    int star_ax = ax + g_win_w - BTN_STAR_W - 4;
    int star_ay = ay + BTN_Y + ((BTN_H - (int)gfx_font_height()) / 2);
    unsigned star_col = current_url_bookmarked() ? C_YELLOW : C_TEXT_DIM;
    gfx_text_center_transparent((unsigned)star_ax, (unsigned)BTN_STAR_W,
                                 (unsigned)star_ay, "*", star_col);
}

static int toolbar_event(widget_t *w, const widget_event_t *ev)
{
    (void)w;
    if (ev->type != WEV_MOUSE_DOWN) return 0;
    /* Content-area-relative coordinates */
    int mx = ev->mx - g_win_x;
    int my = ev->my - (g_win_y + TITLE_H + TABBAR_H);
    if (my < 0 || my >= TOOLBAR_H) return 0;
    /* Hit-test star button */
    int star_x = g_win_w - BTN_STAR_W - 4;
    if (mx >= star_x && mx < star_x + BTN_STAR_W) {
        star_toggle();
        return 1;
    }
    return 0;
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

/* Convert AetherOS keycode_t + modifiers to a NetSurf NS_KEY_* / Unicode codepoint.
 * Returns 0 for keys that should not be forwarded (modifiers, function keys, etc.). */
static uint32_t keycode_to_ns(keycode_t kc, unsigned int mods)
{
    int shift = (mods & MOD_SHIFT) ? 1 : 0;
    int caps  = (mods & MOD_CAPS)  ? 1 : 0;
    int upper = shift ^ caps;

    /* Letters */
    if (kc >= KEY_A && kc <= KEY_Z)
        return (uint32_t)((upper ? 'A' : 'a') + (kc - KEY_A));

    /* Digits and their shift symbols */
    if (kc >= KEY_0 && kc <= KEY_9) {
        static const char shift_nums[] = ")!@#$%^&*(";
        return shift ? (uint32_t)(unsigned char)shift_nums[kc - KEY_0]
                     : (uint32_t)('0' + (kc - KEY_0));
    }

    /* NetSurf special keys */
    switch (kc) {
    case KEY_ENTER:      return (uint32_t)NS_KEY_CR;
    case KEY_BACKSPACE:  return (uint32_t)NS_KEY_DELETE_LEFT;
    case KEY_ESC:        return (uint32_t)NS_KEY_ESCAPE;
    case KEY_SPACE:      return ' ';
    case KEY_LEFT:       return (uint32_t)NS_KEY_LEFT;
    case KEY_RIGHT:      return (uint32_t)NS_KEY_RIGHT;
    case KEY_UP:         return (uint32_t)NS_KEY_UP;
    case KEY_DOWN:       return (uint32_t)NS_KEY_DOWN;
    case KEY_HOME:       return (uint32_t)NS_KEY_LINE_START;
    case KEY_END:        return (uint32_t)NS_KEY_LINE_END;
    case KEY_DELETE:     return (uint32_t)NS_KEY_DELETE_RIGHT;
    case KEY_PGUP:       return (uint32_t)NS_KEY_PAGE_UP;
    case KEY_PGDN:       return (uint32_t)NS_KEY_PAGE_DOWN;
    /* Punctuation */
    case KEY_MINUS:      return shift ? '_' : '-';
    case KEY_EQUALS:     return shift ? '+' : '=';
    case KEY_LBRACKET:   return shift ? '{' : '[';
    case KEY_RBRACKET:   return shift ? '}' : ']';
    case KEY_BACKSLASH:  return shift ? '|' : '\\';
    case KEY_SEMICOLON:  return shift ? ':' : ';';
    case KEY_APOSTROPHE: return shift ? '"' : '\'';
    case KEY_COMMA:      return shift ? '<' : ',';
    case KEY_DOT:        return shift ? '>' : '.';
    case KEY_SLASH:      return shift ? '?' : '/';
    case KEY_GRAVE:      return shift ? '~' : '`';
    default:             return 0;  /* modifier/fn keys — don't forward */
    }
}

static int viewport_event(widget_t *w, const widget_event_t *ev)
{
    (void)w;
    if (!nsaether_bw) return 0;

    /* Screen-absolute → viewport-relative coordinates */
    int vx = ev->mx - g_win_x;
    int vy = ev->my - (g_win_y + TITLE_H + VP_OFF);
    int dy = vy + g_scroll_y;

    switch (ev->type) {
    case WEV_MOUSE_DOWN:
        widget_set_focused(&g_viewport);
        browser_window_mouse_click(nsaether_bw, BROWSER_MOUSE_PRESS_1, vx, dy);
        nsaether_dirty = true;  /* show focus / cursor in form inputs */
        return 1;
    case WEV_MOUSE_UP:
        browser_window_mouse_click(nsaether_bw, BROWSER_MOUSE_CLICK_1, vx, dy);
        js_handle_mouse_click(vx, dy);
        nsaether_dirty = true;
        return 1;
    case WEV_MOUSE_MOVE:
        browser_window_mouse_click(nsaether_bw, BROWSER_MOUSE_HOVER, vx, dy);
        return 0;
    case WEV_KEY_DOWN:
        /* Ctrl shortcuts — handled exclusively, no character forwarding */
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
            case KEY_T:
                tab_new("http://info.cern.ch");
                return 1;
            case KEY_W:
                tab_close(g_active_tab);
                return 1;
            case KEY_D:
                star_toggle();
                return 1;
            case KEY_S: {
                char dlpath[128];
                if (download_save_page(dlpath, sizeof(dlpath)))
                    snprintf(g_dl_msg, sizeof(g_dl_msg), "Saved: %s", dlpath);
                else
                    snprintf(g_dl_msg, sizeof(g_dl_msg), "Download failed");
                g_dl_msg_ticks = 90;
                widget_invalidate(&g_status);
                return 1;
            }
            case KEY_1: tab_switch(0); return 1;
            case KEY_2: tab_switch(1); return 1;
            case KEY_3: tab_switch(2); return 1;
            case KEY_4: tab_switch(3); return 1;
            case KEY_5: tab_switch(4); return 1;
            case KEY_6: tab_switch(5); return 1;
            case KEY_7: tab_switch(6); return 1;
            case KEY_8: tab_switch(7); return 1;
            case KEY_9: tab_switch(g_tab_count - 1); return 1;
            default: return 0;
            }
        }

        {
            /* Convert to NetSurf key; let NetSurf handle it first.
             * If NetSurf consumes the key (focused form input), force a
             * redraw so cursor / text changes appear immediately.
             * If NetSurf ignores it, fall back to page-scroll behaviour. */
            uint32_t ns_key = keycode_to_ns(ev->keycode, ev->modifiers);
            if (ns_key != 0) {
                bool handled = browser_window_key_press(nsaether_bw, ns_key);
                if (handled) {
                    nsaether_dirty = true;
                    return 1;
                }
            }

            /* NetSurf did not consume — page scroll fallback */
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
    const char *txt = (g_dl_msg_ticks > 0) ? g_dl_msg :
                      nsaether_status[0]   ? nsaether_status :
                      nsaether_loading     ? "Loading..." : "Done";
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

/* ── Smart address bar (omnibar) ─────────────────────────────────────────── */

static void url_encode(const char *in, char *out, int out_max)
{
    static const char hex[] = "0123456789ABCDEF";
    int oi = 0;
    for (int i = 0; in[i] && oi < out_max - 3; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == ' ') {
            out[oi++] = '+';
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.' || c == '~') {
            out[oi++] = (char)c;
        } else {
            out[oi++] = '%';
            out[oi++] = hex[c >> 4];
            out[oi++] = hex[c & 0xf];
        }
    }
    out[oi] = '\0';
}

static void smart_navigate(const char *input)
{
    if (!input || !input[0]) return;

    /* Already has a scheme — navigate directly */
    if (strncmp(input, "http://",  7) == 0 ||
        strncmp(input, "https://", 8) == 0 ||
        strncmp(input, "file://",  7) == 0) {
        navigate_to(input);
        return;
    }

    /* Contains a space → treat as search query */
    if (strchr(input, ' ')) {
        char encoded[768];
        char url[900];
        url_encode(input, encoded, sizeof(encoded));
        snprintf(url, sizeof(url),
                 "https://lite.duckduckgo.com/lite/?q=%s", encoded);
        navigate_to(url);
        return;
    }

    /* Has at least one dot with no spaces → bare domain, add https:// */
    if (strchr(input, '.')) {
        char url[600];
        snprintf(url, sizeof(url), "https://%s", input);
        navigate_to(url);
        return;
    }

    /* Single word with no dot → search query */
    char encoded[768];
    char url[900];
    url_encode(input, encoded, sizeof(encoded));
    snprintf(url, sizeof(url),
             "https://lite.duckduckgo.com/lite/?q=%s", encoded);
    navigate_to(url);
}

static void on_addr_submit(widget_t *w)
{
    smart_navigate(textinput_get_text(w));
    widget_set_focused(&g_viewport);
}

/* ── Per-frame hook: NetSurf scheduler + viewport sync ───────────────────── */

static void browser_per_frame(void *ud)
{
    (void)ud;

    nsaether_schedule_drain();
    js_timers_tick();

    /* Expire download status message */
    if (g_dl_msg_ticks > 0) {
        if (--g_dl_msg_ticks == 0)
            widget_invalidate(&g_status);
    }

    if (nsaether_dirty && nsaether_bw &&
            browser_window_redraw_ready(nsaether_bw)) {
        render_viewport();
        sys_sched_yield();
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
        g_btn_reload.data.button.text[0] = 'R';
        g_btn_reload.data.button.text[1] = '\0';
        if (nsaether_bw) {
            g_btn_back.state =
                browser_window_back_available(nsaether_bw) ? WS_NORMAL : WS_DISABLED;
            g_btn_fwd.state =
                browser_window_forward_available(nsaether_bw) ? WS_NORMAL : WS_DISABLED;
            /* Record page in history and refresh star bookmark indicator */
            struct nsurl *hurl = NULL;
            if (browser_window_get_url(nsaether_bw, false, &hurl) == NSERROR_OK && hurl) {
                history_append(nsurl_access(hurl));
                nsurl_unref(hurl);
            }
        }
        widget_invalidate(&g_btn_back);
        widget_invalidate(&g_btn_fwd);
        widget_invalidate(&g_btn_reload);
        widget_invalidate(&g_toolbar);   /* refresh star indicator */
        widget_invalidate(&g_status);
    }
    s_was_loading = (int)nsaether_loading;
}

/* ── Window chrome ───────────────────────────────────────────────────────── */

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

    /* Tab bar (Iteration 4) */
    widget_init(&g_tabbar, WIDGET_PANEL, 0, 0, g_win_w, TABBAR_H);
    g_tabbar.draw_fn  = tabbar_draw;
    g_tabbar.event_fn = tabbar_event;
    widget_add_child(&g_root, &g_tabbar);

    /* Toolbar panel (shifted down by TABBAR_H) */
    widget_init(&g_toolbar, WIDGET_PANEL,
                0, TABBAR_H, g_win_w, TOOLBAR_H + SEPARATOR_H);
    g_toolbar.draw_fn  = toolbar_draw;
    g_toolbar.event_fn = toolbar_event;
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

    /* Viewport */
    widget_init(&g_viewport, WIDGET_PANEL,
                0, VP_OFF, g_win_w, g_viewport_h);
    g_viewport.draw_fn  = viewport_draw;
    g_viewport.event_fn = viewport_event;
    g_viewport.focusable = 1;
    widget_add_child(&g_root, &g_viewport);

    /* Status bar */
    widget_init(&g_status, WIDGET_PANEL, 0, status_y, g_win_w, STATUS_H);
    g_status.draw_fn = status_draw;
    widget_add_child(&g_root, &g_status);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *start_url = (argc > 1) ? argv[1]
                                       : "http://info.cern.ch";

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

#ifdef AETHER_TLS_ENABLED
    tls_global_init();
#endif

    nsoption_set_bool(enable_javascript, true);

    /* 2. Screen dimensions and window geometry */
    gfx_init();
    persist_init();   /* load bookmarks + history from /config/ */
    cookies_init();   /* load cookie store from /config/cookies.txt */
    int scr_w = (int)gfx_width();
    int scr_h = (int)gfx_height();

    g_win_x      = 0;
    g_win_y      = TOPBAR_H + ACCENT_H;
    g_win_w      = scr_w;
    g_win_h      = scr_h - TOPBAR_H - ACCENT_H - DOCK_H - DOCK_BB_STRIP;
    g_viewport_h = g_win_h - TITLE_H - VP_OFF - STATUS_H;
    /* Leave room for the star button: [addr] [4px gap] [BTN_STAR_W] [4px margin] */
    g_addr_w     = g_win_w - ADDR_X - BTN_STAR_W - 8;

    {
        char dbg[80];
        snprintf(dbg, sizeof(dbg),
                 "aether_browser: scr=%dx%d win=%dx%d vp_h=%d\n",
                 scr_w, scr_h, g_win_w, g_win_h, g_viewport_h);
        uart(dbg);
    }

    nsaether_win_w = g_win_w;
    nsaether_win_h = g_viewport_h;

    /* 3. Register WM window */
    g_win_id = sys_wm_register(g_win_x, g_win_y, g_win_w, g_win_h,
                               "AetherOS Browser");
    if (g_win_id >= 0)
        sys_wm_set_zindex(g_win_id, 1);

    /* 4. Draw initial chrome */
    draw_chrome();

    /* 5. Build widget tree (tab bar + toolbar + viewport + status bar) */
    build_ui();

    /* 6. Open first tab — this calls browser_window_create internally, which
     *    triggers aether_window_create to set nsaether_bw / nsaether_pixels,
     *    then tab_new() saves them into g_tabs[0]. */
    if (tab_new(start_url) < 0) {
        uart("aether_browser FAIL: tab_new\n");
        netsurf_exit();
        return 1;
    }

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

    /* 8. Cleanup — destroy all open tabs */
    for (int i = 0; i < g_tab_count; i++) {
        if (g_tabs[i].bw) browser_window_destroy(g_tabs[i].bw);
    }
    netsurf_exit();
    sys_wm_request_close(g_win_id);
    return 0;
}
