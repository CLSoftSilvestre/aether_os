/*
 * AetherOS — Top Bar daemon
 * File: userspace/apps/topbar/main.c
 *
 * macOS-style menu bar: full-width strip at the top of the screen.
 * Runs as a standalone process spawned by init after the compositor.
 * Uses a GPU BO so the compositor z-orders it above all app windows.
 *
 * Layout:
 *   Left  : "AetherOS  v0.0.8"
 *   Center: "Lumina Desktop — Phase 6.2"
 *   Right : [net][vol][user][bell] | Weekday DD Mon  HH:MM
 *
 * Right-side status icons (14×14 px, vertically centered in the 36px bar):
 *   net  — WiFi-style 3-arc icon; C_ACCENT when connected, dim when not
 *   vol  — Speaker cone + waves; C_TEXT active, dim+red-slash when muted
 *   user — Person silhouette; C_ACCENT2 (cyan)
 *   bell — Notification bell; bright when unread, dim when none;
 *           red badge dot when unread > 0; clicking spawns /notif_center
 *
 * Click handling: the topbar window receives mouse events forwarded by
 * init (same mechanism as the dock).  Only the bell icon triggers an
 * action right now; the others are reserved for future menus.
 */

#include <gfx.h>
#include <gpu.h>
#include <input.h>
#include <notif.h>
#include <stdio.h>
#include <string.h>
#include <sys.h>

/* ── Bar geometry ────────────────────────────────────────────────────────── */

#define TOPBAR_H   36
#define ACCENT_H    2
#define BAR_H      (TOPBAR_H + ACCENT_H)

/* Status icon geometry (14×14, vertically centered in 36px bar) */
#define ICON_SZ    14
#define ICON_Y     ((TOPBAR_H - ICON_SZ) / 2)   /* = 11 */
#define ICON_GAP    6                             /* pixels between icon starts */
#define ICON_STEP  (ICON_SZ + ICON_GAP)          /* = 20 */

/* Right panel: erase this many px from the right edge each refresh */
#define RIGHT_PANEL_W  290

/* Separator between icon cluster and date/time */
#define SEP_HALF_PAD   6

/* ── Globals ─────────────────────────────────────────────────────────────── */

static int            SCR_W;
static int            SCR_H;
static long           g_win_id = -1;
static gpu_bo_t       g_bo     = GPU_BO_INVALID;
static unsigned int  *g_bo_ptr = NULL;

/* Status state (polled every ~100 ticks) */
static int            g_net_connected = 0;
static int            g_vol           = 100;
static int            g_muted         = 0;
static unsigned int   g_notif_unread  = 0;

/* Icon X positions updated each right-panel draw */
static int g_x_net, g_x_vol, g_x_user, g_x_bell;

/* PID of spawned notification center (0 = not running) */
static long g_nc_pid = 0;

/* ── Calendar helpers ────────────────────────────────────────────────────── */

static const char *const DAY3[7]  = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static const char *const MON3[12] = {"Jan","Feb","Mar","Apr","May","Jun",
                                      "Jul","Aug","Sep","Oct","Nov","Dec"};

/*
 * epoch_to_date — Gregorian calendar decomposition (O(1)).
 * Algorithm: https://howardhinnant.github.io/date_algorithms.html
 * Writes day-of-week (0=Sun), calendar day (1-31), month (1-12).
 */
static void epoch_to_date(unsigned long ts, int *out_dow, int *out_day, int *out_mon)
{
    unsigned long d = ts / 86400UL;
    *out_dow = (int)((d + 4UL) % 7UL);   /* epoch was Thursday (4) */

    long z   = (long)d + 719468L;
    long era = (z >= 0 ? z : z - 146096L) / 146097L;
    long doe = z - era * 146097L;
    long yoe = (doe - doe/1460L + doe/36524L - doe/146096L) / 365L;
    long doy = doe - (365L*yoe + yoe/4L - yoe/100L);
    long mp  = (5L*doy + 2L) / 153L;
    *out_day = (int)(doy - (153L*mp + 2L)/5L + 1L);
    *out_mon = (int)(mp < 10L ? mp + 3L : mp - 9L);
}

static void fmt_time(char *buf, unsigned long ts)
{
    unsigned long s = ts % 86400UL;
    unsigned long h = s / 3600UL;
    unsigned long m = (s % 3600UL) / 60UL;
    snprintf(buf, 6, "%02lu:%02lu", h, m);
}

static void fmt_date(char *buf, unsigned long ts)
{
    int dow, day, mon;
    epoch_to_date(ts, &dow, &day, &mon);
    snprintf(buf, 16, "%s %02d %s", DAY3[dow], day, MON3[mon - 1]);
}

/* ── Status polling ──────────────────────────────────────────────────────── */

static unsigned int read_notif_unread(void)
{
    long vfd = sys_fs_open(NOTIF_PATH);
    if (vfd < 0) return 0u;
    notif_header_t hdr;
    long n = sys_fs_read(vfd, &hdr, (long)sizeof(hdr));
    sys_fs_close(vfd);
    if (n < (long)sizeof(hdr) || hdr.magic != NOTIF_MAGIC) return 0u;
    return hdr.unread;
}

static void poll_status(void)
{
    net_status_t ns;
    if (sys_net_status(&ns) == 0)
        g_net_connected = (ns.ip != 0u);

    audio_conf_t ac;
    if (sys_audio_conf_get(&ac) == 0) {
        g_vol   = (int)ac.output_volume;
        g_muted = (int)ac.output_mute;
    }

    g_notif_unread = read_notif_unread();
}

/* ── Status icon drawing (14×14 into C_PANEL background) ────────────────── */

static void draw_icon_net(int x, int y)
{
    unsigned c = g_net_connected ? C_ACCENT : C_TEXT_DIM;
    gfx_fill(x, y, ICON_SZ, ICON_SZ, C_PANEL);

    /* Outer arc: top bar + side endpoints */
    gfx_fill(x+1,  y+0,  12, 1, c);
    gfx_fill(x+0,  y+1,   1, 1, c);
    gfx_fill(x+13, y+1,   1, 1, c);
    /* Middle arc */
    gfx_fill(x+3,  y+3,   8, 1, c);
    gfx_fill(x+2,  y+4,   1, 1, c);
    gfx_fill(x+11, y+4,   1, 1, c);
    /* Inner arc */
    gfx_fill(x+5,  y+6,   4, 1, c);
    gfx_fill(x+4,  y+7,   1, 1, c);
    gfx_fill(x+9,  y+7,   1, 1, c);
    /* Center dot */
    gfx_fill(x+5,  y+10,  4, 2, c);
}

static void draw_icon_vol(int x, int y)
{
    unsigned c = (g_muted || g_vol == 0) ? C_TEXT_DIM : C_TEXT;
    gfx_fill(x, y, ICON_SZ, ICON_SZ, C_PANEL);

    /* Speaker body (rectangular back, left side) */
    gfx_fill(x+0, y+4, 3, 6, c);

    /* Speaker cone: vertical bars expanding rightward from body */
    gfx_fill(x+3, y+3, 1, 8, c);    /* attachment edge */
    gfx_fill(x+4, y+2, 1, 10, c);   /* expanding */
    gfx_fill(x+5, y+1, 1, 12, c);   /* horn opening face */

    if (!g_muted && g_vol > 0) {
        /* Small sound wave (arc close to horn) */
        gfx_fill(x+7,  y+4,  1, 1, c);
        gfx_fill(x+8,  y+5,  1, 4, c);
        gfx_fill(x+7,  y+9,  1, 1, c);
        if (g_vol > 50) {
            /* Large sound wave (arc further right) */
            gfx_fill(x+10, y+3,  1, 1, c);
            gfx_fill(x+11, y+4,  1, 6, c);
            gfx_fill(x+10, y+10, 1, 1, c);
        }
    } else {
        /* Mute: red diagonal slash (top-right → bottom-left) */
        unsigned r = C_RED;
        gfx_fill(x+12, y+2,  1, 2, r);
        gfx_fill(x+11, y+4,  1, 2, r);
        gfx_fill(x+10, y+6,  1, 2, r);
        gfx_fill(x+9,  y+8,  1, 2, r);
        gfx_fill(x+8,  y+10, 1, 2, r);
    }
}

static void draw_icon_user(int x, int y)
{
    unsigned c = C_ACCENT2;
    gfx_fill(x, y, ICON_SZ, ICON_SZ, C_PANEL);

    /* Head: rounded-rectangle circle */
    gfx_fill(x+4, y+0,  6, 1, c);   /* top */
    gfx_fill(x+3, y+1,  8, 3, c);   /* head body */
    gfx_fill(x+4, y+4,  6, 1, c);   /* bottom */

    /* Shoulders / torso */
    gfx_fill(x+3, y+7,  8, 1, c);   /* narrow shoulder line */
    gfx_fill(x+2, y+8,  10, 1, c);  /* broader */
    gfx_fill(x+1, y+9,  12, 4, c);  /* chest/torso down to bar bottom */
}

static void draw_icon_bell(int x, int y)
{
    unsigned c = (g_notif_unread > 0) ? C_TEXT : C_TEXT_DIM;
    gfx_fill(x, y, ICON_SZ, ICON_SZ, C_PANEL);

    /* Handle (ring at top) */
    gfx_fill(x+5, y+0,  4, 1, c);

    /* Bell dome */
    gfx_fill(x+4, y+1,  6, 1, c);   /* narrow top of dome */
    gfx_fill(x+3, y+2,  8, 2, c);   /* dome sides */
    gfx_fill(x+2, y+4,  10, 5, c);  /* dome body */
    gfx_fill(x+1, y+9,  12, 1, c);  /* base rim */

    /* Clapper */
    gfx_fill(x+5, y+11, 4, 2, c);

    /* Red badge dot when there are unread notifications */
    if (g_notif_unread > 0)
        gfx_fill(x+9, y+0, 5, 4, C_RED);
}

/* ── Right-panel draw ────────────────────────────────────────────────────── */

static void draw_right_panel(void)
{
    unsigned long ts = sys_rtc_get();

    char tbuf[6];
    fmt_time(tbuf, ts);

    char dbuf[16];
    fmt_date(dbuf, ts);

    /* Erase right panel */
    gfx_fill(SCR_W - RIGHT_PANEL_W, 0, RIGHT_PANEL_W, TOPBAR_H, C_PANEL);

    /* Clock (rightmost text) */
    int clock_x = SCR_W - 14 - gfx_text_width(tbuf);
    gfx_text(clock_x, 10, tbuf, C_TEXT, C_PANEL);

    /* Date (left of clock) */
    int date_x = clock_x - 8 - gfx_text_width(dbuf);
    gfx_text(date_x, 10, dbuf, C_TEXT_DIM, C_PANEL);

    /* Vertical separator between date/time and icon cluster */
    int sep_x = date_x - SEP_HALF_PAD;
    gfx_vline(sep_x, 6, TOPBAR_H - 12, C_SEP);

    /* Icon positions (right-to-left: bell, user, vol, net) */
    g_x_bell = sep_x - SEP_HALF_PAD - ICON_SZ;
    g_x_user = g_x_bell - ICON_STEP;
    g_x_vol  = g_x_user - ICON_STEP;
    g_x_net  = g_x_vol  - ICON_STEP;

    draw_icon_net (g_x_net,  ICON_Y);
    draw_icon_vol (g_x_vol,  ICON_Y);
    draw_icon_user(g_x_user, ICON_Y);
    draw_icon_bell(g_x_bell, ICON_Y);
}

/* ── Click handling ──────────────────────────────────────────────────────── */

static void handle_click(int cx, int cy)
{
    if (cy < 0 || cy >= TOPBAR_H) return;

    /* Bell icon → launch notification center (one instance at a time) */
    if (cx >= g_x_bell && cx < g_x_bell + ICON_SZ) {
        /* Reap previous NC if it already exited */
        if (g_nc_pid > 0) {
            int st = 0;
            if (sys_waitpid_nb(g_nc_pid, &st) != 0)
                g_nc_pid = 0;
        }
        if (g_nc_pid <= 0)
            g_nc_pid = sys_spawn("/notif_center");
    }
    /* Future: vol click → quick volume menu, net → network settings, etc. */
}

/* ── Full topbar redraw ──────────────────────────────────────────────────── */

static void draw_topbar(void)
{
    if (g_bo_ptr)
        gfx_begin_frame(g_bo_ptr, (unsigned)SCR_W, BAR_H, 0, 0);

    gfx_fill(0, 0, (unsigned)SCR_W, TOPBAR_H, C_PANEL);

    /* Branding — left */
    gfx_text(14, 10, "AetherOS", C_TEXT, C_PANEL);
    gfx_text((unsigned)(14 + gfx_text_width("AetherOS") + 8), 10,
             "v0.0.8", C_TEXT_DIM, C_PANEL);

    /* Center label
    gfx_text_center(0, (unsigned)SCR_W, 10,
                    "Lumina Desktop  \xe2\x80\x94  Phase 6.2",
                    C_TEXT_DIM, C_PANEL); */

    /* Right panel: icons + date + clock */
    draw_right_panel();

    /* Accent line */
    // gfx_fill(0, TOPBAR_H, (unsigned)SCR_W, ACCENT_H, C_ACCENT);

    if (g_bo_ptr)
        gfx_end_frame();
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    SCR_W = (int)gfx_width();
    SCR_H = (int)gfx_height();

    g_win_id = sys_wm_register(0, 0, (unsigned)SCR_W, BAR_H, "topbar");
    if (g_win_id >= 0) {
        sys_wm_set_zindex(g_win_id, WM_Z_DOCK);
        sys_wm_set_flags(g_win_id, WM_FLAG_NO_CHROME);
    }

    unsigned bo_bytes = (unsigned)SCR_W * (unsigned)BAR_H * 4u;
    if (g_win_id >= 0) {
        g_bo = gpu_alloc(bo_bytes);
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
    }

    /* Initial status poll before first draw */
    poll_status();
    draw_topbar();

    int tick = 0;
    int prev_lbtn = 0;

    for (;;) {
        sys_vsync_wait();

        /* Drain WM events — mouse clicks forwarded from init */
        unsigned long long wev;
        while ((wev = sys_wm_event_poll()) != 0u) {
            if (wm_event_is_mouse(wev)) {
                mouse_event_t me = wm_event_mouse_unpack(wev);
                int lbtn = (int)(me.buttons & 1u);
                if (lbtn && !prev_lbtn)
                    handle_click((int)me.x, (int)me.y);
                prev_lbtn = lbtn;
            }
        }

        /* Refresh status + right panel every ~1 s (100 vsyncs at 60 Hz) */
        if (++tick >= 100) {
            tick = 0;
            poll_status();
            if (g_bo_ptr)
                gfx_begin_frame(g_bo_ptr, (unsigned)SCR_W, BAR_H, 0, 0);
            draw_right_panel();
            /* Accent line stays constant — no need to redraw */
            if (g_bo_ptr)
                gfx_end_frame();
        }
    }
}
