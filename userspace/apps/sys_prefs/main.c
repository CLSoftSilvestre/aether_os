/*
 * AetherOS — System Preferences
 * File: userspace/apps/sys_prefs/main.c
 *
 * macOS-style settings hub.  Three panes:
 *   Display  — resolution presets (applies on reboot)
 *   Network  — DHCP or static IP (static applies immediately)
 *   Users    — account management with admin/user roles
 *
 * Settings are persisted to /config/ on FAT32.
 * Fallbacks: defaults used when config files are missing.
 *
 * Architecture:
 *   - widget_run() with content_dx=0 so the root panel covers the full
 *     window width (160px sidebar + 560px content area).
 *   - Root panel's draw_fn renders the sidebar; child widgets render content.
 *   - Root panel's event_fn intercepts sidebar clicks; child widgets handle
 *     the rest.  On pane switch: ctx.running=0, outer loop rebuilds the tree.
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <gfx.h>
#include <gpu.h>
#include <sys.h>
#include <widget.h>
#include <icon_cache.h>
#include <config.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define WIN_W       720
#define WIN_H       520
#define TITLE_H      28
#define ACCENT_H      2

#define SIDE_W      160     /* sidebar width */
#define SIDE_ITEM_H  60     /* height of each sidebar nav item */
#define SIDE_PAD_TOP 12     /* top padding inside sidebar before first item */

#define CONT_PAD     14     /* padding inside content area */
#define CONT_X       SIDE_W /* content area x start (in window coords) */
#define CONT_W      (WIN_W - SIDE_W)               /* 560 */
#define CONT_H      (WIN_H - TITLE_H - ACCENT_H)  /* 490 */

/* Content area inner origin (relative to root panel at (0,0)) */
#define INX         (SIDE_W + CONT_PAD)   /* 174 */
#define INY         CONT_PAD              /* 14  */
#define INW         (CONT_W - 2 * CONT_PAD) /* 532 */

/* Standard widget row height */
#define ROW_H        28
#define ROW_GAP       8
#define LABEL_W      110
#define INPUT_X      (INX + LABEL_W + 4)
#define INPUT_W      (INW - LABEL_W - 4)

/* Sidebar colors */
#define C_SIDEBAR    GFX_RGB( 22,  22,  38)
#define C_SIDE_SEL   GFX_RGB( 40,  36,  72)
#define C_SIDE_TXT   GFX_RGB(200, 200, 220)
#define C_SIDE_DIM   GFX_RGB(110, 110, 140)
#define C_SIDE_SEP   GFX_RGB( 42,  42,  70)

/* Status colors */
#define C_OK         GFX_RGB( 80, 200,  75)
#define C_WARN       GFX_RGB(247, 201,  72)
#define C_ERR        GFX_RGB(235,  87,  87)

/* ── Pane IDs ────────────────────────────────────────────────────────────── */
#define PANE_DISPLAY  0
#define PANE_NETWORK  1
#define PANE_USERS    2
#define PANE_SOUND    3
#define PANE_COUNT    4

static const char *k_pane_names[PANE_COUNT] = { "Display", "Network", "Users", "Sound" };

/* ── Global state ────────────────────────────────────────────────────────── */

static long        g_win_id  = -1;
static int         g_win_x   = 80;
static int         g_win_y   = 60;
static int         g_pane    = PANE_DISPLAY;
static int         g_running      = 1;
static int         g_pane_switch  = 0;  /* 1 = pane switch requested by sidebar */

/* widget_ctx passed by address into widget_run; event_fn sets running=0 */
static widget_ctx_t g_ctx;

/* ── Widget storage (shared across rebuilds) ─────────────────────────────── */

static widget_t g_root;           /* full-window root panel (always) */

/* Display pane */
static widget_t g_d_cur_lbl;      /* "Current: W × H" */
static widget_t g_d_list;         /* resolution listview */
static widget_t g_d_apply;        /* Apply button */
static widget_t g_d_status;       /* result label */

/* Network pane */
static widget_t g_n_stat_lbl;     /* connection status */
static widget_t g_n_dhcp_cb;      /* checkbox DHCP */
static widget_t g_n_static_cb;    /* checkbox Static */
static widget_t g_n_note;         /* note label */
static widget_t g_n_ip_lbl,   g_n_ip_in;
static widget_t g_n_mask_lbl, g_n_mask_in;
static widget_t g_n_gw_lbl,   g_n_gw_in;
static widget_t g_n_dns_lbl,  g_n_dns_in;
static widget_t g_n_apply;
static widget_t g_n_result;

/* Users pane */
static widget_t g_u_list;         /* user listview */
static widget_t g_u_add_btn;
static widget_t g_u_del_btn;
static widget_t g_u_cpw_btn;
static widget_t g_u_sep;          /* "── New User ──" label */
static widget_t g_u_name_lbl, g_u_name_in;
static widget_t g_u_pw_lbl,   g_u_pw_in;
static widget_t g_u_admin_cb;
static widget_t g_u_create_btn;
static widget_t g_u_result;

/* Sound pane */
static widget_t g_s_out_list;   /* output device selector */
static widget_t g_s_vol_sb;     /* master output volume 0-100 */
static widget_t g_s_bal_sb;     /* balance 0-200 (center=100) */
static widget_t g_s_mute_cb;    /* mute checkbox */
static widget_t g_s_in_list;    /* input device selector */
static widget_t g_s_gain_sb;    /* input gain 0-100 */
static widget_t g_s_sr_list;    /* sample rate preset */
static widget_t g_s_buf_list;   /* buffer size / period frames */
static widget_t g_s_alert_sb;   /* alert / UI sounds volume 0-100 */
static widget_t g_s_apply;      /* Apply + restart button */
static widget_t g_s_status;     /* result label */

/* ── IP address helpers ──────────────────────────────────────────────────── */

static void ip_to_str(unsigned int ip, char *buf)
{
    snprintf(buf, 16, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >>  8) & 0xFF,  ip        & 0xFF);
}

static unsigned int str_to_ip(const char *s)
{
    unsigned int a = 0, b = 0, c = 0, d = 0;
    int dots = 0;
    unsigned int cur = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            cur = cur * 10u + (unsigned int)(*s - '0');
        } else if (*s == '.' && dots < 3) {
            if (dots == 0) a = cur;
            else if (dots == 1) b = cur;
            else c = cur;
            cur = 0;
            dots++;
        } else break;
    }
    d = cur;
    if (dots != 3) return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* ── Sound pane layout constants ─────────────────────────────────────────── */
/* All Y values are relative to the root panel origin (content area top-left).
 * Labels are drawn directly in root_draw; only interactive widgets go in tree. */
#define SP_Y_OUT_HDR   INY                        /* "Output" section header   */
#define SP_Y_OUT_LIST  (SP_Y_OUT_HDR  + 14)       /* output device listview h=65 */
#define SP_Y_VOL       (SP_Y_OUT_LIST + 65 + 4)   /* volume row                */
#define SP_Y_BAL       (SP_Y_VOL      + ROW_H + 4)/* balance row               */
#define SP_Y_MUTE      (SP_Y_BAL      + ROW_H + 4)/* mute checkbox             */
#define SP_Y_IN_HDR    (SP_Y_MUTE     + ROW_H + ROW_GAP) /* "Input" header    */
#define SP_Y_IN_LIST   (SP_Y_IN_HDR   + 14)       /* input device listview h=65*/
#define SP_Y_GAIN      (SP_Y_IN_LIST  + 65 + 4)   /* gain row                  */
#define SP_Y_PQ_HDR    (SP_Y_GAIN     + ROW_H + ROW_GAP) /* "Playback Quality"*/
#define SP_Y_PQ_COL    (SP_Y_PQ_HDR   + 14)       /* sr / buf column labels    */
#define SP_Y_PQ_LIST   (SP_Y_PQ_COL   + 12)       /* sr + buf listviews h=55   */
#define SP_Y_ALT_HDR   (SP_Y_PQ_LIST  + 55 + ROW_GAP)  /* "Alert Sounds"      */
#define SP_Y_ALT_VOL   (SP_Y_ALT_HDR  + 14)       /* alert volume row          */
#define SP_Y_APPLY     (SP_Y_ALT_VOL  + ROW_H + ROW_GAP) /* apply + status    */

#define SP_LBL    90                               /* label column width        */
#define SP_SBX    (INX + SP_LBL)                  /* scrollbar x origin        */
#define SP_SBW    300                              /* scrollbar width           */
#define SP_HALF   270                              /* half-pane column width    */

/* ── Sidebar drawing ─────────────────────────────────────────────────────── */

static void draw_sidebar(int ax, int ay)
{
    /* Background */
    gfx_fill(ax, ay, SIDE_W, CONT_H, C_SIDEBAR);

    /* Vertical separator line */
    gfx_fill(ax + SIDE_W - 1, ay, 1, CONT_H, C_SIDE_SEP);

    /* Pane nav items */
    for (int i = 0; i < PANE_COUNT; i++) {
        int iy = ay + SIDE_PAD_TOP + i * SIDE_ITEM_H;

        if (i == g_pane) {
            /* Selected: accent highlight */
            gfx_fill(ax, iy, SIDE_W - 1, SIDE_ITEM_H, C_SIDE_SEL);
            gfx_fill(ax, iy, 3, SIDE_ITEM_H, C_ACCENT);
        }

        /* Pane name */
        unsigned tc = (i == g_pane) ? C_TEXT : C_SIDE_DIM;
        int tx = ax + 18;
        int ty = iy + (SIDE_ITEM_H - WGT_FONT_H) / 2;
        gfx_text(tx, ty, k_pane_names[i], tc, 0);
    }
}

/* ── Root panel draw_fn: sidebar + content background ────────────────────── */

static void root_draw(widget_t *w, int ax, int ay)
{
    (void)w;
    /* Content area background */
    gfx_fill(ax + SIDE_W, ay, CONT_W, CONT_H, C_WIN_BG);
    /* Sidebar */
    draw_sidebar(ax, ay);

    if (g_pane == PANE_SOUND) {
        /* Section headers */
        gfx_text(ax + INX, ay + SP_Y_OUT_HDR, "Output",           C_ACCENT,   0);
        gfx_text(ax + INX, ay + SP_Y_IN_HDR,  "Input",            C_ACCENT,   0);
        gfx_text(ax + INX, ay + SP_Y_PQ_HDR,  "Playback Quality", C_ACCENT,   0);
        gfx_text(ax + INX, ay + SP_Y_ALT_HDR, "Alert Sounds",     C_ACCENT,   0);
        /* Row labels */
        gfx_text(ax + INX, ay + SP_Y_VOL  + 6, "Volume:",      C_TEXT, 0);
        gfx_text(ax + INX, ay + SP_Y_BAL  + 6, "Balance:",     C_TEXT, 0);
        gfx_text(ax + INX, ay + SP_Y_GAIN + 6, "Input Gain:",  C_TEXT, 0);
        gfx_text(ax + INX, ay + SP_Y_ALT_VOL + 6, "Alert Vol:",C_TEXT, 0);
        /* Playback quality sub-column labels */
        gfx_text(ax + INX,           ay + SP_Y_PQ_COL, "Sample Rate",  C_TEXT_DIM, 0);
        gfx_text(ax + INX + SP_HALF, ay + SP_Y_PQ_COL, "Buffer Size",  C_TEXT_DIM, 0);
    }
}

/* ── Root panel event_fn: intercept sidebar clicks ──────────────────────── */

static int root_event(widget_t *w, const widget_event_t *ev)
{
    (void)w;
    if (ev->type != WEV_MOUSE_DOWN) return 0;

    int rel_x = ev->mx - g_win_x;
    int rel_y = ev->my - g_win_y - TITLE_H - ACCENT_H;

    if (rel_x < 0 || rel_x >= SIDE_W || rel_y < SIDE_PAD_TOP) return 0;

    int item = (rel_y - SIDE_PAD_TOP) / SIDE_ITEM_H;
    if (item < 0 || item >= PANE_COUNT) return 0;

    if (item != g_pane) {
        g_pane = item;
        g_pane_switch = 1;   /* tell main loop this is a switch, not a close */
        g_ctx.running = 0;   /* exit widget_run; outer loop rebuilds */
    }
    return 1;
}

/* ── Window chrome ───────────────────────────────────────────────────────── */

static void draw_chrome(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H,
                            TITLE_H, "System Preferences", 0);
}

static void on_reposition(void *ud)
{
    (void)ud;
    draw_chrome();
}

/* ── Display pane ────────────────────────────────────────────────────────── */

static const struct { unsigned int w, h; const char *label; } k_resolutions[] = {
    { 640,  480, " 640 \xc3\x97 480" },
    { 800,  600, " 800 \xc3\x97 600" },
    { 1024, 768, "1024 \xc3\x97 768" },
    { 1280, 720, "1280 \xc3\x97 720   (default)" },
    { 1920, 1080,"1920 \xc3\x97 1080" },
};
#define N_RES 5

static void on_display_apply(widget_t *w)
{
    (void)w;
    int sel = listview_get_selected(&g_d_list);
    if (sel < 0 || sel >= N_RES) {
        label_set_text(&g_d_status, "Select a resolution first.");
        return;
    }
    unsigned int rw = k_resolutions[sel].w;
    unsigned int rh = k_resolutions[sel].h;
    long r = sys_display_set_res(rw, rh);
    if (r == 0) {
        label_set_text(&g_d_status, "Saved — restart to apply.");
    } else {
        label_set_text(&g_d_status, "Failed to save resolution.");
    }
    widget_invalidate(&g_d_status);
}

static void build_pane_display(void)
{
    /* Re-init root to clear children */
    widget_init_panel(&g_root, 0, 0, WIN_W, CONT_H, C_WIN_BG);
    g_root.draw_fn  = root_draw;
    g_root.event_fn = root_event;

    /* Get current resolution */
    unsigned int cw = 0, ch = 0;
    char cur_lbl[48];
    if (sys_display_get_res(&cw, &ch) == 0)
        snprintf(cur_lbl, sizeof(cur_lbl), "Current resolution: %u \xc3\x97 %u", cw, ch);
    else
        snprintf(cur_lbl, sizeof(cur_lbl), "Current resolution: unknown");

    int y = INY;

    widget_init_label(&g_d_cur_lbl, INX, y, INW, ROW_H,
                      cur_lbl, WGT_ALIGN_LEFT);
    y += ROW_H + 4;

    widget_init_listview(&g_d_list, INX, y, INW, 160, N_RES, NULL);
    for (int i = 0; i < N_RES; i++) {
        listview_add_item(&g_d_list, k_resolutions[i].label, NULL);
        /* Pre-select current resolution */
        if (k_resolutions[i].w == cw && k_resolutions[i].h == ch) {
            g_d_list.data.listview.selected = i;
            g_d_list.data.listview.scroll_top = i > 2 ? i - 2 : 0;
        }
    }
    y += 160 + ROW_GAP;

    widget_init_button(&g_d_apply, INX, y, 100, ROW_H, "Apply",
                       on_display_apply);
    y += ROW_H + ROW_GAP;

    widget_init_label(&g_d_status, INX, y, INW, ROW_H,
                      "", WGT_ALIGN_LEFT);

    widget_add_child(&g_root, &g_d_cur_lbl);
    widget_add_child(&g_root, &g_d_list);
    widget_add_child(&g_root, &g_d_apply);
    widget_add_child(&g_root, &g_d_status);
}

/* ── Network pane ────────────────────────────────────────────────────────── */

static int g_net_mode = 0;   /* 0=DHCP, 1=static */

static void refresh_net_fields(void)
{
    /* Disable/grey note when in DHCP mode */
    static char note[80];
    if (g_net_mode == 0)
        snprintf(note, sizeof(note),
                 "Static fields are saved but only apply in Manual mode.");
    else
        snprintf(note, sizeof(note), "");
    label_set_text(&g_n_note, note);
    widget_invalidate(&g_n_note);
}

static void on_dhcp_click(widget_t *w, int checked)
{
    (void)w; (void)checked;
    g_net_mode = 0;
    checkbox_set_checked(&g_n_dhcp_cb, 1);
    checkbox_set_checked(&g_n_static_cb, 0);
    refresh_net_fields();
}

static void on_static_click(widget_t *w, int checked)
{
    (void)w; (void)checked;
    g_net_mode = 1;
    checkbox_set_checked(&g_n_dhcp_cb, 0);
    checkbox_set_checked(&g_n_static_cb, 1);
    refresh_net_fields();
}

static void on_net_apply(widget_t *w)
{
    (void)w;
    net_conf_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = (unsigned char)g_net_mode;

    if (g_net_mode == 1) {
        cfg.ip      = str_to_ip(textinput_get_text(&g_n_ip_in));
        cfg.mask    = str_to_ip(textinput_get_text(&g_n_mask_in));
        cfg.gateway = str_to_ip(textinput_get_text(&g_n_gw_in));
        cfg.dns     = str_to_ip(textinput_get_text(&g_n_dns_in));

        if (cfg.ip == 0) {
            label_set_text(&g_n_result, "Invalid IP address.");
            widget_invalidate(&g_n_result);
            return;
        }
        if (cfg.mask == 0) cfg.mask    = 0xFFFFFF00u;
        if (cfg.dns  == 0) cfg.dns     = 0x08080808u;
    }

    long r = sys_net_conf_set(&cfg);
    if (r == 0) {
        if (g_net_mode == 1)
            label_set_text(&g_n_result, "Static IP applied.");
        else
            label_set_text(&g_n_result, "DHCP mode saved — restart to re-run DHCP.");
    } else {
        label_set_text(&g_n_result, "Failed to apply network config.");
    }
    widget_invalidate(&g_n_result);
}

static void build_pane_network(void)
{
    widget_init_panel(&g_root, 0, 0, WIN_W, CONT_H, C_WIN_BG);
    g_root.draw_fn  = root_draw;
    g_root.event_fn = root_event;

    /* Fetch current config */
    net_conf_t cur;
    memset(&cur, 0, sizeof(cur));
    sys_net_conf_get(&cur);
    g_net_mode = cur.mode;

    /* Status label */
    char stat[64];
    if (cur.ready) {
        char ipbuf[16];
        ip_to_str(cur.ip, ipbuf);
        snprintf(stat, sizeof(stat), "Connected \xe2\x80\xa2 %s", ipbuf);
    } else {
        snprintf(stat, sizeof(stat), "Not connected");
    }

    int y = INY;
    widget_init_label(&g_n_stat_lbl, INX, y, INW, ROW_H, stat, WGT_ALIGN_LEFT);
    y += ROW_H + ROW_GAP;

    /* DHCP / Static toggles */
    widget_init_checkbox(&g_n_dhcp_cb,   INX, y, INW, ROW_H,
                         "DHCP (automatic)", on_dhcp_click);
    checkbox_set_checked(&g_n_dhcp_cb, g_net_mode == 0);
    y += ROW_H + 4;

    widget_init_checkbox(&g_n_static_cb, INX, y, INW, ROW_H,
                         "Manual (static IP)", on_static_click);
    checkbox_set_checked(&g_n_static_cb, g_net_mode == 1);
    y += ROW_H + ROW_GAP;

    /* Note */
    widget_init_label(&g_n_note, INX, y, INW, ROW_H, "", WGT_ALIGN_LEFT);
    y += ROW_H + 4;

    /* IP / Mask / Gateway / DNS rows */
    char ipbuf[16];

    widget_init_label(&g_n_ip_lbl, INX, y, LABEL_W, ROW_H,
                      "IP Address:", WGT_ALIGN_LEFT);
    ip_to_str(cur.ip, ipbuf);
    widget_init_textinput(&g_n_ip_in, INPUT_X, y, INPUT_W, ROW_H, NULL, NULL);
    textinput_set_text(&g_n_ip_in, ipbuf);
    y += ROW_H + 4;

    widget_init_label(&g_n_mask_lbl, INX, y, LABEL_W, ROW_H,
                      "Subnet:", WGT_ALIGN_LEFT);
    ip_to_str(cur.mask ? cur.mask : 0xFFFFFF00u, ipbuf);
    widget_init_textinput(&g_n_mask_in, INPUT_X, y, INPUT_W, ROW_H, NULL, NULL);
    textinput_set_text(&g_n_mask_in, ipbuf);
    y += ROW_H + 4;

    widget_init_label(&g_n_gw_lbl, INX, y, LABEL_W, ROW_H,
                      "Gateway:", WGT_ALIGN_LEFT);
    ip_to_str(cur.gateway, ipbuf);
    widget_init_textinput(&g_n_gw_in, INPUT_X, y, INPUT_W, ROW_H, NULL, NULL);
    textinput_set_text(&g_n_gw_in, ipbuf);
    y += ROW_H + 4;

    widget_init_label(&g_n_dns_lbl, INX, y, LABEL_W, ROW_H,
                      "DNS Server:", WGT_ALIGN_LEFT);
    ip_to_str(cur.dns ? cur.dns : 0x08080808u, ipbuf);
    widget_init_textinput(&g_n_dns_in, INPUT_X, y, INPUT_W, ROW_H, NULL, NULL);
    textinput_set_text(&g_n_dns_in, ipbuf);
    y += ROW_H + ROW_GAP;

    widget_init_button(&g_n_apply, INX, y, 100, ROW_H, "Apply", on_net_apply);
    y += ROW_H + ROW_GAP;

    widget_init_label(&g_n_result, INX, y, INW, ROW_H, "", WGT_ALIGN_LEFT);

    /* Build tree */
    widget_add_child(&g_root, &g_n_stat_lbl);
    widget_add_child(&g_root, &g_n_dhcp_cb);
    widget_add_child(&g_root, &g_n_static_cb);
    widget_add_child(&g_root, &g_n_note);
    widget_add_child(&g_root, &g_n_ip_lbl);
    widget_add_child(&g_root, &g_n_ip_in);
    widget_add_child(&g_root, &g_n_mask_lbl);
    widget_add_child(&g_root, &g_n_mask_in);
    widget_add_child(&g_root, &g_n_gw_lbl);
    widget_add_child(&g_root, &g_n_gw_in);
    widget_add_child(&g_root, &g_n_dns_lbl);
    widget_add_child(&g_root, &g_n_dns_in);
    widget_add_child(&g_root, &g_n_apply);
    widget_add_child(&g_root, &g_n_result);

    refresh_net_fields();
}

/* ── Users pane ──────────────────────────────────────────────────────────── */

#define MAX_USERS_DISP  16

static user_info_t g_users_buf[MAX_USERS_DISP];
static int         g_users_n = 0;
static int         g_sel_uid = -1;  /* uid of listview-selected user, or -1 */

/* User mode: 0=normal, 1=adding new user, 2=changing password */
#define UMODE_NORMAL  0
#define UMODE_ADD     1
#define UMODE_CHANGEPW 2
static int g_umode = UMODE_NORMAL;

static void reload_user_list(void)
{
    g_users_n = (int)sys_user_list(g_users_buf, MAX_USERS_DISP);
    if (g_users_n < 0) g_users_n = 0;

    listview_clear(&g_u_list);
    for (int i = 0; i < g_users_n; i++) {
        char item[48];
        snprintf(item, sizeof(item), "%s  [%s]",
                 g_users_buf[i].name,
                 g_users_buf[i].role == 1 ? "Admin" : "User");
        listview_add_item(&g_u_list, item, (void *)(uintptr_t)g_users_buf[i].uid);
    }
    widget_invalidate(&g_u_list);
}

static void update_sep_label(void)
{
    const char *txt;
    switch (g_umode) {
    case UMODE_ADD:      txt = "--- Add New User ---";      break;
    case UMODE_CHANGEPW: txt = "--- Change Password ---";   break;
    default:             txt = "--- Add New User ---";      break;
    }
    label_set_text(&g_u_sep, txt);
    widget_invalidate(&g_u_sep);
}

static void on_add_user(widget_t *w)
{
    (void)w;
    g_umode = UMODE_ADD;
    g_sel_uid = -1;
    g_u_list.data.listview.selected = -1;
    textinput_set_text(&g_u_name_in, "");
    textinput_set_text(&g_u_pw_in, "");
    checkbox_set_checked(&g_u_admin_cb, 0);
    label_set_text(&g_u_result, "Enter details below.");
    update_sep_label();
    widget_invalidate(&g_u_sep);
    widget_invalidate(&g_u_result);
}

static void on_del_user(widget_t *w)
{
    (void)w;
    if (g_sel_uid < 0) {
        label_set_text(&g_u_result, "Select a user first.");
        widget_invalidate(&g_u_result);
        return;
    }
    long r = sys_user_delete((unsigned int)g_sel_uid);
    if (r == 0) {
        g_sel_uid = -1;
        reload_user_list();
        label_set_text(&g_u_result, "User deleted.");
    } else {
        label_set_text(&g_u_result,
                       "Cannot delete (last admin, or self).");
    }
    widget_invalidate(&g_u_result);
}

static void on_change_pw(widget_t *w)
{
    (void)w;
    if (g_sel_uid < 0) {
        label_set_text(&g_u_result, "Select a user first.");
        widget_invalidate(&g_u_result);
        return;
    }
    g_umode = UMODE_CHANGEPW;
    textinput_set_text(&g_u_name_in, "");
    textinput_set_text(&g_u_pw_in, "");
    label_set_text(&g_u_result, "Enter old pw then new pw.");
    update_sep_label();
    widget_invalidate(&g_u_result);
}

static void on_create_user(widget_t *w)
{
    (void)w;
    const char *name = textinput_get_text(&g_u_name_in);
    const char *pw   = textinput_get_text(&g_u_pw_in);
    int         adm  = checkbox_get_checked(&g_u_admin_cb);

    if (g_umode == UMODE_ADD) {
        if (!name || !name[0]) {
            label_set_text(&g_u_result, "Username cannot be empty.");
            widget_invalidate(&g_u_result);
            return;
        }
        long r = sys_user_create(name, pw ? pw : "",
                                  adm ? 1 : 0);
        if (r >= 0) {
            reload_user_list();
            label_set_text(&g_u_result, "User created.");
            g_umode = UMODE_NORMAL;
        } else {
            label_set_text(&g_u_result, "Failed (duplicate name or no permission).");
        }

    } else if (g_umode == UMODE_CHANGEPW) {
        /* name field = old password, pw field = new password */
        long r = sys_user_set_pw((unsigned int)g_sel_uid, name, pw ? pw : "");
        if (r == 0) {
            label_set_text(&g_u_result, "Password changed.");
            g_umode = UMODE_NORMAL;
        } else {
            label_set_text(&g_u_result, "Wrong old password or no permission.");
        }
    }
    widget_invalidate(&g_u_result);
}

static void on_user_select(widget_t *w, int idx, void *ud)
{
    (void)w; (void)ud;
    if (idx < 0 || idx >= g_users_n) { g_sel_uid = -1; return; }
    g_sel_uid = (int)g_users_buf[idx].uid;
    g_umode   = UMODE_NORMAL;
    char msg[48];
    snprintf(msg, sizeof(msg), "Selected: %s", g_users_buf[idx].name);
    label_set_text(&g_u_result, msg);
    widget_invalidate(&g_u_result);
}

static void build_pane_users(void)
{
    widget_init_panel(&g_root, 0, 0, WIN_W, CONT_H, C_WIN_BG);
    g_root.draw_fn  = root_draw;
    g_root.event_fn = root_event;

    g_sel_uid = -1;
    g_umode   = UMODE_NORMAL;

    int y = INY;

    /* User listview */
    widget_init_listview(&g_u_list, INX, y, INW, 148, 16, on_user_select);
    y += 148 + ROW_GAP;

    /* Action buttons row */
    widget_init_button(&g_u_add_btn, INX,           y, 120, ROW_H,
                       "Add User",      on_add_user);
    widget_init_button(&g_u_del_btn, INX + 128,     y, 120, ROW_H,
                       "Delete",        on_del_user);
    widget_init_button(&g_u_cpw_btn, INX + 256,     y, 140, ROW_H,
                       "Change PW",     on_change_pw);
    y += ROW_H + ROW_GAP;

    /* Separator */
    widget_init_label(&g_u_sep, INX, y, INW, ROW_H,
                      "--- Add New User ---", WGT_ALIGN_LEFT);
    y += ROW_H + 4;

    /* Name / old-pw field */
    widget_init_label(&g_u_name_lbl, INX, y, LABEL_W, ROW_H,
                      "Username:", WGT_ALIGN_LEFT);
    widget_init_textinput(&g_u_name_in, INPUT_X, y, INPUT_W, ROW_H, NULL, NULL);
    y += ROW_H + 4;

    /* Password / new-pw field */
    widget_init_label(&g_u_pw_lbl, INX, y, LABEL_W, ROW_H,
                      "Password:", WGT_ALIGN_LEFT);
    widget_init_textinput(&g_u_pw_in, INPUT_X, y, INPUT_W, ROW_H, NULL, NULL);
    y += ROW_H + 4;

    /* Admin checkbox */
    widget_init_checkbox(&g_u_admin_cb, INX, y, INW, ROW_H,
                         "Administrator role", NULL);
    y += ROW_H + ROW_GAP;

    /* Create / Apply button */
    widget_init_button(&g_u_create_btn, INX, y, 140, ROW_H,
                       "Create User", on_create_user);
    y += ROW_H + ROW_GAP;

    /* Status */
    widget_init_label(&g_u_result, INX, y, INW, ROW_H, "", WGT_ALIGN_LEFT);

    /* Build tree */
    widget_add_child(&g_root, &g_u_list);
    widget_add_child(&g_root, &g_u_add_btn);
    widget_add_child(&g_root, &g_u_del_btn);
    widget_add_child(&g_root, &g_u_cpw_btn);
    widget_add_child(&g_root, &g_u_sep);
    widget_add_child(&g_root, &g_u_name_lbl);
    widget_add_child(&g_root, &g_u_name_in);
    widget_add_child(&g_root, &g_u_pw_lbl);
    widget_add_child(&g_root, &g_u_pw_in);
    widget_add_child(&g_root, &g_u_admin_cb);
    widget_add_child(&g_root, &g_u_create_btn);
    widget_add_child(&g_root, &g_u_result);

    /* Populate the list */
    reload_user_list();
}

/* ── Sound pane ──────────────────────────────────────────────────────────── */

static audio_dev_info_t g_s_devs[8];
static int              g_s_ndevs = 0;

static const unsigned int  k_sample_rates[]  = { 44100, 48000, 96000 };
static const char *k_sr_labels[] = {
    " 44 100 Hz",
    " 48 000 Hz  (default)",
    " 96 000 Hz"
};
#define N_SR 3

static const unsigned short k_periods[]      = { 64, 128, 256, 512 };
static const char *k_period_labels[] = {
    " 64  frames  (~1.3 ms)",
    "128  frames  (~2.7 ms)",
    "256  frames  (~5.3 ms)",
    "512  frames (~10.7 ms)"
};
#define N_PERIOD 4

static int find_sr_idx(unsigned int sr)
{
    for (int i = 0; i < N_SR; i++)
        if (k_sample_rates[i] == sr) return i;
    return 1; /* default: 48000 */
}

static int find_period_idx(unsigned short p)
{
    for (int i = 0; i < N_PERIOD; i++)
        if (k_periods[i] == p) return i;
    return 0; /* default: 64 */
}

static int dev_name_eq(const char *a, const char *b)
{
    int i = 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return !a[i] && !b[i];
}

static void on_sound_apply(widget_t *w)
{
    (void)w;

    audio_conf_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.output_volume  = (unsigned char)g_s_vol_sb.data.scrollbar.value;
    cfg.output_balance = (signed char)(g_s_bal_sb.data.scrollbar.value - 100);
    cfg.output_mute    = (unsigned char)g_s_mute_cb.data.checkbox.checked;
    cfg.input_gain     = (unsigned char)g_s_gain_sb.data.scrollbar.value;
    cfg.alert_volume   = (unsigned char)g_s_alert_sb.data.scrollbar.value;

    int sr_sel  = listview_get_selected(&g_s_sr_list);
    cfg.sample_rate   = (sr_sel  >= 0 && sr_sel  < N_SR)     ? k_sample_rates[sr_sel]  : 48000;
    int buf_sel = listview_get_selected(&g_s_buf_list);
    cfg.period_frames = (unsigned short)((buf_sel >= 0 && buf_sel < N_PERIOD) ?
                         k_periods[buf_sel] : 64);
    cfg.bit_depth = 16;

    /* Device names from selected list rows */
    int out_sel = listview_get_selected(&g_s_out_list);
    if (out_sel >= 0 && out_sel < g_s_ndevs) {
        const char *dn = g_s_devs[out_sel].name;
        int j = 0;
        while (dn[j] && j < AUDIO_CONF_NAME_MAX - 1) { cfg.output_dev[j] = dn[j]; j++; }
        cfg.output_dev[j] = '\0';
    }
    int in_sel  = listview_get_selected(&g_s_in_list);
    if (in_sel  >= 0 && in_sel  < g_s_ndevs) {
        const char *dn = g_s_devs[in_sel].name;
        int j = 0;
        while (dn[j] && j < AUDIO_CONF_NAME_MAX - 1) { cfg.input_dev[j] = dn[j]; j++; }
        cfg.input_dev[j] = '\0';
    }

    long r = sys_audio_conf_set(&cfg);
    if (r == 0) {
        /* Find aether_sound and restart it so new settings take effect */
        ps_entry_t procs[32];
        int n = (int)sys_ps(procs, 32);
        int snd_pid = -1;
        for (int i = 0; i < n; i++) {
            if (dev_name_eq(procs[i].name, "aether_sound")) {
                snd_pid = (int)procs[i].pid;
                break;
            }
        }
        if (snd_pid > 0) {
            sys_kill((unsigned)snd_pid);
            sys_sleep(20);                /* wait ~200 ms (20 × 10 ms ticks) */
            sys_spawn("/aether_sound");
            label_set_text(&g_s_status, "Applied — audio server restarted.");
        } else {
            label_set_text(&g_s_status, "Saved. Audio server not running.");
        }
    } else {
        label_set_text(&g_s_status, "Failed: invalid audio configuration.");
    }
    widget_invalidate(&g_s_status);
}

static void build_pane_sound(void)
{
    widget_init_panel(&g_root, 0, 0, WIN_W, CONT_H, C_WIN_BG);
    g_root.draw_fn  = root_draw;
    g_root.event_fn = root_event;

    /* Enumerate audio devices once */
    g_s_ndevs = (int)sys_audio_enum(g_s_devs, 8);

    /* Read current config */
    audio_conf_t cur;
    memset(&cur, 0, sizeof(cur));
    sys_audio_conf_get(&cur);

    /* ── Output device listview ──────────────────────────────────── */
    widget_init_listview(&g_s_out_list, INX, SP_Y_OUT_LIST, INW, 65,
                         8, NULL);
    for (int i = 0; i < g_s_ndevs; i++) {
        listview_add_item(&g_s_out_list, g_s_devs[i].name, NULL);
        if (cur.output_dev[0] && dev_name_eq(cur.output_dev, g_s_devs[i].name))
            g_s_out_list.data.listview.selected = i;
    }
    if (g_s_ndevs == 0)
        listview_add_item(&g_s_out_list, "(no devices found)", NULL);

    /* ── Volume scrollbar ────────────────────────────────────────── */
    widget_init_scrollbar_h(&g_s_vol_sb, SP_SBX, SP_Y_VOL, SP_SBW, ROW_H,
                            100, 5);
    g_s_vol_sb.data.scrollbar.value = (int)cur.output_volume;

    /* ── Balance scrollbar (0=full-left, 100=center, 200=full-right) */
    widget_init_scrollbar_h(&g_s_bal_sb, SP_SBX, SP_Y_BAL, SP_SBW, ROW_H,
                            200, 10);
    g_s_bal_sb.data.scrollbar.value = (int)(cur.output_balance) + 100;

    /* ── Mute checkbox ───────────────────────────────────────────── */
    widget_init_checkbox(&g_s_mute_cb, INX, SP_Y_MUTE, INW / 2, ROW_H,
                         "Mute output", NULL);
    checkbox_set_checked(&g_s_mute_cb, cur.output_mute);

    /* ── Input device listview ───────────────────────────────────── */
    widget_init_listview(&g_s_in_list, INX, SP_Y_IN_LIST, INW, 65,
                         8, NULL);
    for (int i = 0; i < g_s_ndevs; i++) {
        listview_add_item(&g_s_in_list, g_s_devs[i].name, NULL);
        if (cur.input_dev[0] && dev_name_eq(cur.input_dev, g_s_devs[i].name))
            g_s_in_list.data.listview.selected = i;
    }
    if (g_s_ndevs == 0)
        listview_add_item(&g_s_in_list, "(no devices found)", NULL);

    /* ── Input gain scrollbar ────────────────────────────────────── */
    widget_init_scrollbar_h(&g_s_gain_sb, SP_SBX, SP_Y_GAIN, SP_SBW, ROW_H,
                            100, 5);
    g_s_gain_sb.data.scrollbar.value = (int)cur.input_gain;

    /* ── Sample rate listview ────────────────────────────────────── */
    widget_init_listview(&g_s_sr_list, INX, SP_Y_PQ_LIST, SP_HALF - 4, 55,
                         N_SR, NULL);
    for (int i = 0; i < N_SR; i++)
        listview_add_item(&g_s_sr_list, k_sr_labels[i], NULL);
    g_s_sr_list.data.listview.selected = find_sr_idx(cur.sample_rate);

    /* ── Buffer size listview ────────────────────────────────────── */
    widget_init_listview(&g_s_buf_list, INX + SP_HALF, SP_Y_PQ_LIST,
                         INW - SP_HALF, 55, N_PERIOD, NULL);
    for (int i = 0; i < N_PERIOD; i++)
        listview_add_item(&g_s_buf_list, k_period_labels[i], NULL);
    g_s_buf_list.data.listview.selected = find_period_idx(cur.period_frames);

    /* ── Alert volume scrollbar ──────────────────────────────────── */
    widget_init_scrollbar_h(&g_s_alert_sb, SP_SBX, SP_Y_ALT_VOL, SP_SBW, ROW_H,
                            100, 5);
    g_s_alert_sb.data.scrollbar.value = (int)cur.alert_volume;

    /* ── Apply button + status label ─────────────────────────────── */
    widget_init_button(&g_s_apply, INX, SP_Y_APPLY, 120, ROW_H,
                       "Apply", on_sound_apply);
    widget_init_label(&g_s_status, INX + 128, SP_Y_APPLY, INW - 128, ROW_H,
                      "", WGT_ALIGN_LEFT);

    /* Build widget tree (all 11 children ≤ WIDGET_MAX_CHILDREN=16) */
    widget_add_child(&g_root, &g_s_out_list);
    widget_add_child(&g_root, &g_s_vol_sb);
    widget_add_child(&g_root, &g_s_bal_sb);
    widget_add_child(&g_root, &g_s_mute_cb);
    widget_add_child(&g_root, &g_s_in_list);
    widget_add_child(&g_root, &g_s_gain_sb);
    widget_add_child(&g_root, &g_s_sr_list);
    widget_add_child(&g_root, &g_s_buf_list);
    widget_add_child(&g_root, &g_s_alert_sb);
    widget_add_child(&g_root, &g_s_apply);
    widget_add_child(&g_root, &g_s_status);
}

/* ── Pane builder dispatch ───────────────────────────────────────────────── */

static void build_current_pane(void)
{
    switch (g_pane) {
    case PANE_DISPLAY: build_pane_display(); break;
    case PANE_NETWORK: build_pane_network(); break;
    case PANE_USERS:   build_pane_users();   break;
    case PANE_SOUND:   build_pane_sound();   break;
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    /* Center the window on a 1280×720 screen */
    g_win_x = (1280 - WIN_W) / 2;
    g_win_y = (720  - WIN_H) / 2 + 36;   /* 36 = topbar height */

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H,
                                "System Preferences");
    if (g_win_id < 0) return 1;

    icon_cache_init();

    g_running = 1;
    while (g_running) {
        g_pane_switch = 0;  /* clear flag before each pane run */
        build_current_pane();

        g_ctx.win_x         = &g_win_x;
        g_ctx.win_y         = &g_win_y;
        g_ctx.content_dx    = 0;             /* root covers full width */
        g_ctx.content_dy    = TITLE_H + ACCENT_H;
        g_ctx.win_id        = (int)g_win_id;
        g_ctx.win_w         = WIN_W;
        g_ctx.win_h         = WIN_H;
        g_ctx.on_reposition = on_reposition;
        g_ctx.per_frame_fn  = NULL;
        g_ctx.userdata      = NULL;
        g_ctx.running       = 1;

        widget_run(&g_root, &g_ctx);

        /* g_pane_switch=1 → sidebar click; continue outer loop with new pane.
         * g_pane_switch=0 → WM_EV_CLOSE_REQUEST (user clicked X); stop. */
        if (!g_pane_switch)
            g_running = 0;
    }

    sys_wm_request_close(g_win_id);
    return 0;
}
