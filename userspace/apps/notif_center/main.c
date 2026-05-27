/*
 * AetherOS — Notification Center
 * File: userspace/apps/notif_center/main.c
 *
 * Spawned by the topbar when the bell icon is clicked.
 * Opens a panel at the top-right of the screen (below the topbar).
 * Reads the notification store from NOTIF_PATH, displays all entries,
 * and marks them as read.  A "Clear All" button wipes the store.
 *
 * Window:  380 × 480 px, top-right corner, WM chrome (close button).
 * Notifications: each entry shows [app]  [time]  title + message (2 rows).
 * Empty state: centered "No notifications" text with a subtle bell glyph.
 *
 * Protocol: see userspace/lib/include/notif.h.
 * IPC: filesystem-based — reads/writes NOTIF_PATH on FAT32/AFS.
 */

#include <gfx.h>
#include <gpu.h>
#include <input.h>
#include <notif.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define NC_W         380
#define NC_H         480
#define NC_TH         28    /* title bar height (used by gfx_glass_window_frame) */
#define TOPBAR_STRIP  38    /* TOPBAR_H + ACCENT_H = 36 + 2 */

/* Content area (below titlebar + separator) */
#define CONTENT_Y    (NC_TH + 2)
#define BTN_H         30
#define BTN_Y        (NC_H - BTN_H - 8)
#define CONTENT_H    (BTN_Y - CONTENT_Y - 4)

/* Per-entry row height */
#define ROW_H         56
#define MAX_VISIBLE   (CONTENT_H / ROW_H)

/* "Clear All" button */
#define BTN_W        110
#define BTN_X        ((NC_W - BTN_W) / 2)

/* Close-button hit box (matches gfx_glass_window_frame placement) */
#define CLOSE_BTN_X  10
#define CLOSE_BTN_W  12

/* ── Globals ─────────────────────────────────────────────────────────────── */

static int            SCR_W, SCR_H;
static int            NC_X, NC_Y;

static long           g_win_id = -1;
static gpu_bo_t       g_bo     = GPU_BO_INVALID;
static unsigned int  *g_bo_ptr = NULL;

/* Loaded store */
static notif_header_t g_hdr;
static notif_entry_t  g_entries[NOTIF_MAX];
static int            g_count = 0;

/* UI state */
static int            g_scroll     = 0;   /* first visible entry index */
static int            g_hov_close  = 0;
static int            g_hov_clrall = 0;
static int            g_needs_draw = 1;

/* ── Store I/O ───────────────────────────────────────────────────────────── */

static void store_load(void)
{
    g_count = 0;
    long vfd = sys_fs_open(NOTIF_PATH);
    if (vfd < 0) return;

    long n = sys_fs_read(vfd, &g_hdr, (long)sizeof(g_hdr));
    if (n < (long)sizeof(g_hdr) || g_hdr.magic != NOTIF_MAGIC) {
        sys_fs_close(vfd);
        return;
    }
    unsigned cnt = (g_hdr.count < (unsigned)NOTIF_MAX)
                   ? g_hdr.count : (unsigned)NOTIF_MAX;
    g_count = (int)cnt;
    sys_fs_read(vfd, g_entries, (long)(cnt * sizeof(notif_entry_t)));
    sys_fs_close(vfd);
}

/* Mark all entries read and write back */
static void store_mark_read(void)
{
    if (g_count == 0) return;
    g_hdr.unread = 0u;
    for (int i = 0; i < g_count; i++)
        g_entries[i].read = 1;

    long vfd = sys_fs_create(NOTIF_PATH);
    if (vfd < 0) return;
    sys_fs_write(vfd, &g_hdr, (long)sizeof(g_hdr));
    sys_fs_write(vfd, g_entries, (long)((unsigned)g_count * sizeof(notif_entry_t)));
    sys_fs_close(vfd);
}

/* Clear all notifications */
static void store_clear(void)
{
    g_hdr.magic   = NOTIF_MAGIC;
    g_hdr.version = NOTIF_VERSION;
    g_hdr.count   = 0u;
    g_hdr.unread  = 0u;
    g_count = 0;

    long vfd = sys_fs_create(NOTIF_PATH);
    if (vfd < 0) return;
    sys_fs_write(vfd, &g_hdr, (long)sizeof(g_hdr));
    sys_fs_close(vfd);
}

/* ── Time helpers ────────────────────────────────────────────────────────── */

static void fmt_ts_short(char *buf, unsigned int ts)
{
    /* Show HH:MM from a Unix timestamp */
    unsigned long s = (unsigned long)ts % 86400UL;
    unsigned long h = s / 3600UL;
    unsigned long m = (s % 3600UL) / 60UL;
    snprintf(buf, 8, "%02lu:%02lu", h, m);
}

/* ── Drawing helpers ─────────────────────────────────────────────────────── */

/* Truncate string to fit within px_max pixels (8px/char fallback) */
static void draw_text_clipped(int x, int y, const char *s,
                               int max_px, unsigned fg, unsigned bg)
{
    int cw = gfx_text_width("M");     /* approximate char width */
    if (cw <= 0) cw = 8;
    int max_chars = max_px / cw;
    if (max_chars <= 0) return;

    int len = (int)strlen(s);
    if (gfx_text_width(s) <= max_px) {
        gfx_text(x, y, s, fg, bg);
        return;
    }
    /* Truncate + ellipsis */
    char tmp[128];
    int  n = max_chars > 3 ? max_chars - 3 : 0;
    if (n > 126) n = 126;
    if (n > len) n = len;
    int i;
    for (i = 0; i < n; i++) tmp[i] = s[i];
    tmp[i++] = '.'; tmp[i++] = '.'; tmp[i++] = '.';
    tmp[i] = '\0';
    gfx_text(x, y, tmp, fg, bg);
}

/* Draw a single notification row at window-relative (0,ry) */
static void draw_entry(int ry, const notif_entry_t *e)
{
    int abs_y = NC_Y + ry;
    unsigned row_bg = e->read ? C_WIN_BG : GFX_RGB(28, 24, 48);  /* unread tinted */
    gfx_fill(NC_X, abs_y, NC_W, ROW_H, row_bg);

    /* Unread indicator bar */
    if (!e->read)
        gfx_fill(NC_X, abs_y, 3, ROW_H, C_ACCENT);

    /* App name + timestamp on first line */
    char ts_buf[8];
    fmt_ts_short(ts_buf, e->timestamp);
    unsigned dim = GFX_RGB(90, 86, 128);
    gfx_text(NC_X + 10, abs_y + 6, e->app[0] ? e->app : "System", C_ACCENT, row_bg);
    int ts_x = NC_X + NC_W - 14 - gfx_text_width(ts_buf);
    gfx_text(ts_x, abs_y + 6, ts_buf, dim, row_bg);

    /* Title (bold-ish via repeated draw offset by 1px) */
    gfx_text(NC_X + 10, abs_y + 22, e->title, C_TEXT, row_bg);

    /* Message (dimmed, clipped) */
    draw_text_clipped(NC_X + 10, abs_y + 36, e->msg,
                      NC_W - 24, dim, row_bg);

    /* Bottom separator */
    gfx_hline(NC_X, abs_y + ROW_H - 1, NC_W, C_SEP);
}

/* Small bell glyph for the empty state (14×14 at x,y) */
static void draw_empty_bell(int x, int y)
{
    unsigned c = C_TEXT_DIM;
    gfx_fill(x+5,  y+0,  4, 1, c);
    gfx_fill(x+4,  y+1,  6, 1, c);
    gfx_fill(x+3,  y+2,  8, 2, c);
    gfx_fill(x+2,  y+4,  10, 5, c);
    gfx_fill(x+1,  y+9,  12, 1, c);
    gfx_fill(x+5,  y+11, 4, 2, c);
}

static void draw_window(void)
{
    if (g_bo_ptr)
        gfx_begin_frame(g_bo_ptr, NC_W, NC_H, NC_X, NC_Y);

    /* Window chrome */
    gfx_glass_window_frame(NC_X, NC_Y, NC_W, NC_H, NC_TH,
                            "Notification Center", g_hov_close);

    /* Content background */
    gfx_fill(NC_X, NC_Y + CONTENT_Y, NC_W, CONTENT_H, C_WIN_BG);

    if (g_count == 0) {
        /* Empty state */
        int cx = NC_X + (NC_W - 14) / 2;
        int cy = NC_Y + CONTENT_Y + (CONTENT_H - 14 - 20) / 2;
        draw_empty_bell(cx, cy);
        gfx_text_center(NC_X, NC_W, cy + 20,
                        "No notifications", C_TEXT_DIM, C_WIN_BG);
    } else {
        /* Render visible entries */
        int visible = g_count - g_scroll;
        if (visible > MAX_VISIBLE) visible = MAX_VISIBLE;
        for (int i = 0; i < visible; i++)
            draw_entry(CONTENT_Y + i * ROW_H, &g_entries[g_scroll + i]);

        /* Scroll hint if more entries above/below */
        if (g_scroll > 0) {
            gfx_text_center(NC_X, NC_W, NC_Y + CONTENT_Y - 14,
                            "\xe2\x96\xb2 scroll", C_TEXT_DIM, C_PANEL);
        }
        if (g_scroll + MAX_VISIBLE < g_count) {
            gfx_text_center(NC_X, NC_W,
                            NC_Y + CONTENT_Y + MAX_VISIBLE * ROW_H + 2,
                            "\xe2\x96\xbc scroll", C_TEXT_DIM, C_WIN_BG);
        }
    }

    /* "Clear All" button */
    unsigned btn_bg = g_hov_clrall ? C_ACCENT : GFX_RGB(50, 46, 84);
    gfx_fill_rounded(NC_X + BTN_X, NC_Y + BTN_Y, BTN_W, BTN_H,
                     4, btn_bg);
    gfx_rect_rounded(NC_X + BTN_X, NC_Y + BTN_Y, BTN_W, BTN_H,
                     4, C_ACCENT);
    gfx_text_center(NC_X + BTN_X, BTN_W,
                    NC_Y + BTN_Y + (BTN_H - gfx_font_height()) / 2,
                    "Clear All", C_TEXT, btn_bg);

    if (g_bo_ptr)
        gfx_end_frame();
}

/* ── Event handling ──────────────────────────────────────────────────────── */

static int hit_close(int mx, int my)
{
    return (mx >= NC_X + CLOSE_BTN_X &&
            mx <  NC_X + CLOSE_BTN_X + CLOSE_BTN_W + 4 &&
            my >= NC_Y && my < NC_Y + NC_TH);
}

static int hit_clrall(int mx, int my)
{
    return (mx >= NC_X + BTN_X && mx < NC_X + BTN_X + BTN_W &&
            my >= NC_Y + BTN_Y && my < NC_Y + BTN_Y + BTN_H);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    SCR_W = (int)gfx_width();
    SCR_H = (int)gfx_height();

    NC_X = SCR_W - NC_W - 8;
    NC_Y = TOPBAR_STRIP + 4;

    /* Register window */
    g_win_id = sys_wm_register(NC_X, NC_Y, NC_W, NC_H, "Notification Center");
    if (g_win_id < 0) {
        sys_exit(1);
    }

    /* GPU BO for compositor */
    g_bo = gpu_alloc((unsigned)(NC_W * NC_H) * 4u);
    if (g_bo != GPU_BO_INVALID) {
        g_bo_ptr = (unsigned int *)gpu_map(g_bo);
        if (g_bo_ptr) {
            sys_wm_set_buffer(g_win_id, g_bo);
            gfx_set_damage_target((int)g_win_id);
        } else {
            gpu_free(g_bo);
            g_bo = GPU_BO_INVALID;
        }
    }

    /* Load store and mark all as read */
    store_load();
    store_mark_read();

    draw_window();

    int prev_lbtn = 0;

    for (;;) {
        sys_vsync_wait();

        unsigned long long wev;
        while ((wev = sys_wm_event_poll()) != 0u) {

            unsigned int etype = wm_event_type(wev);

            if (etype == WM_EV_CLOSE_REQUEST) {
                sys_wm_request_close(g_win_id);
                sys_exit(0);
            }

            if (wm_event_is_mouse(wev)) {
                mouse_event_t me = wm_event_mouse_unpack(wev);
                int mx = (int)me.x, my = (int)me.y;
                int lbtn = (int)(me.buttons & 1u);

                /* Hover state updates */
                int hc = hit_close(mx, my);
                int ha = hit_clrall(mx, my);
                if (hc != g_hov_close || ha != g_hov_clrall) {
                    g_hov_close  = hc;
                    g_hov_clrall = ha;
                    g_needs_draw = 1;
                }

                /* Click actions (on button release) */
                if (!lbtn && prev_lbtn) {
                    if (hit_close(mx, my)) {
                        sys_wm_request_close(g_win_id);
                        sys_exit(0);
                    }
                    if (hit_clrall(mx, my)) {
                        store_clear();
                        g_count = 0;
                        g_scroll = 0;
                        g_needs_draw = 1;
                    }
                }

                /* Scroll: left-click in content area drags (simple up/down) */
                if (lbtn && !prev_lbtn) {
                    int cy = NC_Y + CONTENT_Y;
                    if (my >= cy && my < cy + CONTENT_H) {
                        /* First click in content — future: drag-scroll */
                    }
                }

                /* Mouse-wheel scroll (buttons bit 3=up, bit 4=down) */
                if (me.buttons & (1u << 3)) {
                    if (g_scroll > 0) { g_scroll--; g_needs_draw = 1; }
                }
                if (me.buttons & (1u << 4)) {
                    int max_sc = g_count - MAX_VISIBLE;
                    if (max_sc < 0) max_sc = 0;
                    if (g_scroll < max_sc) { g_scroll++; g_needs_draw = 1; }
                }

                prev_lbtn = lbtn;
            }

            if (etype == WM_EV_REDRAW)
                g_needs_draw = 1;
        }

        if (g_needs_draw) {
            g_needs_draw = 0;
            draw_window();
        }
    }
}
