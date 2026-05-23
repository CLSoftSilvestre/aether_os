/*
 * AetherOS — AetherMail Email Client
 * File: userspace/apps/aether_mail/main.c
 *
 * Lumina glassmorphism email client with IMAP4rev1 + SMTP support.
 *
 * Window: 900×636.  Two separate widget trees:
 *
 * Setup tree (first-run):
 *   Custom glass form panel + 6 input widgets (email, pass, imap, smtp, save, status)
 *
 * Main tree (after setup / on launch with existing config):
 *   content_dy = TITLE_H  (toolbar is inside the widget area)
 *
 *   Widget y=0..55  — custom toolbar widget (TOOLBAR_H=36 + HDR_H=20)
 *                     draws Compose/Reply/Forward/Delete/Refresh buttons
 *                     and the "FOLDERS" / column label row below them
 *   Widget y=56..577 — three-pane layout:
 *     Left   180 px  — folder listview
 *     Divider  2 px  — accent glow
 *     Right  702 px  — message listview (180 px) + separator + body textarea
 *
 *   Compose sub-view (toggled via hidden flags, same tree):
 *     To: / Subject: labels + textinputs + body textarea + Send/Cancel buttons
 */

#include <gfx.h>
#include <gpu.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "mail_store.h"
#include "imap.h"
#include "smtp.h"

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define WIN_W      900
#define WIN_H      580   /* fits between topbar (38px) and dock (56px) at 720p */
#define TITLE_H     32
#define TOOLBAR_H   36   /* glass nav bar */
#define HDR_H       20   /* "FOLDERS" + column label row */
#define STATUS_H    26   /* status strip at the bottom */
#define SIDE_PAD     8

/* Widget-coordinate space (content_dy = TITLE_H) */
#define ROOT_H     (WIN_H - TITLE_H - STATUS_H)   /* 578 */
#define CONT_W     (WIN_W - 2 * SIDE_PAD)         /* 884 */

/* Pane dimensions */
#define TLBAR_HDR_H (TOOLBAR_H + HDR_H)           /* 56  */
#define SIDEBAR_W   180
#define DIV_W         2
#define RIGHT_W     (CONT_W - SIDEBAR_W - DIV_W)  /* 702 */
#define MSGLIST_H   180                            /* rows inside message list */
#define SEP_Y       (TLBAR_HDR_H + MSGLIST_H)     /* 236 */
#define BODY_Y      (SEP_Y + 1)                   /* 237 */
#define BODY_H      (ROOT_H - BODY_Y)             /* 341 */
#define BODY_SB_W    10
#define BODY_TA_W   (RIGHT_W - BODY_SB_W)         /* 692 */
#define RIGHT_X     (SIDEBAR_W + DIV_W)

/* Compose view y-offsets (within the widget coordinate space, below toolbar) */
#define CO_Y0       (TOOLBAR_H + 6)
#define CO_LBL_W    80
#define CO_INP_X    (CO_LBL_W)
#define CO_INP_W    (CONT_W - CO_LBL_W)
#define CO_BODY_Y   (CO_Y0 + 60)
#define CO_BODY_H   (ROOT_H - CO_BODY_Y - 36)
#define CO_BTN_Y    (ROOT_H - 34)

/* Initial window position */
#define WIN_X_INIT  60
#define WIN_Y_INIT  42   /* 4px below topbar (38px) */

/* ── Glassmorphism colour palette ────────────────────────────────────────── */

#define C_GLASS_TOOLBAR  GFX_RGB( 24,  22,  46)
#define C_GLASS_SIDEBAR  GFX_RGB( 15,  13,  27)
#define C_GLASS_STATUS   GFX_RGB( 20,  18,  36)
#define C_GLASS_SPEC     GFX_RGB(190, 170, 255)
#define C_GLASS_HIGH     GFX_RGB( 82,  70, 158)
#define C_GLASS_EDGE     GFX_RGB(  8,   6,  16)
#define C_GLOW_DIV       GFX_RGB( 68,  56, 132)
#define C_BTN_BG         GFX_RGB( 36,  32,  70)
#define C_HDR_BG         GFX_RGB( 18,  16,  32)
#define C_SETUP_PANEL    GFX_RGB( 28,  26,  52)
#define C_MSGSEL         GFX_RGB( 55,  46, 112)

/* ── App state ───────────────────────────────────────────────────────────── */

#define STATE_MAIN    1
#define STATE_COMPOSE 2
static int g_state = STATE_MAIN;

/* ── Window position ─────────────────────────────────────────────────────── */

static int  g_win_x  = WIN_X_INIT;
static int  g_win_y  = WIN_Y_INIT;

static widget_ctx_t g_ctx;          /* main widget loop context */
static int          g_want_settings; /* set by Settings button → re-run setup */
static long g_win_id = -1;

/* ── Account and mail data ───────────────────────────────────────────────── */

static mail_account_t g_acc;
static imap_conn_t    g_imap;
static int            g_imap_ok = 0;

static mail_msg_t g_msgs[MAIL_MSG_MAX];
static int        g_msg_count  = 0;
static int        g_msg_sel    = -1;

static char g_status[128]  = "Not connected.";
static char g_reply_to[MAIL_FROM_MAX];
static char g_reply_subj[MAIL_SUBJECT_MAX];

/* ── Toolbar button layout ───────────────────────────────────────────────── */

#define TB_BTN_H  26
#define TB_BTN_Y  ((TOOLBAR_H - TB_BTN_H) / 2)

typedef struct { int rx; int w; unsigned char icon; const char *label; int id; } tbtn_t;

#define TB_COMPOSE  0
#define TB_REPLY    1
#define TB_FORWARD  2
#define TB_DELETE   3
#define TB_REFRESH  4
#define TB_SETTINGS 5

static const tbtn_t k_tbbtns[6] = {
    {   4, 82, ICON_BTN_COMPOSE,  "Compose",  TB_COMPOSE  },
    {  88, 74, ICON_BTN_REPLY,    "Reply",    TB_REPLY    },
    { 164, 82, ICON_BTN_FORWARD,  "Forward",  TB_FORWARD  },
    { 248, 70, ICON_BTN_CLEAR,    "Delete",   TB_DELETE   },
    { 320, 80, ICON_BTN_REFRESH,  "Refresh",  TB_REFRESH  },
    { 402, 82, ICON_BTN_SETTINGS, "Settings", TB_SETTINGS },
};

/* Saved absolute draw origin of the toolbar widget (set each draw call). */
static int g_tb_ax, g_tb_ay;

/* ── Forward declarations ────────────────────────────────────────────────── */

static void draw_chrome(void);
static void draw_status(void);
static void do_refresh(void);
static void do_delete(void);
static void do_connect(void);
static void enter_main(void);
static void enter_compose(int reply);
static void msg_list_rebuild(void);

/* ── Toolbar widget: custom draw + event ─────────────────────────────────── */

static void toolbar_draw(widget_t *w, int ax, int ay)
{
    (void)w;
    g_tb_ax = ax;
    g_tb_ay = ay;

    int pw = CONT_W;

    /* Toolbar glass background */
    gfx_fill((unsigned)ax, (unsigned)ay,
              (unsigned)pw, (unsigned)TOOLBAR_H, C_GLASS_TOOLBAR);
    gfx_hline((unsigned)ax, (unsigned)ay, (unsigned)pw, C_GLASS_SPEC);
    gfx_fill((unsigned)ax, (unsigned)(ay+1), (unsigned)pw, 2, C_GLASS_HIGH);
    gfx_hline((unsigned)ax, (unsigned)(ay + TOOLBAR_H - 1), (unsigned)pw, C_ACCENT);

    /* Toolbar buttons */
    int in_compose = (g_state == STATE_COMPOSE);
    for (int i = 0; i < 5; i++) {
        int bx = ax + k_tbbtns[i].rx;
        int by = ay + TB_BTN_Y;
        int dim = in_compose && (i == TB_DELETE || i == TB_REFRESH);
        gfx_fill_rounded((unsigned)bx, (unsigned)by,
                          (unsigned)k_tbbtns[i].w, (unsigned)TB_BTN_H, 4, C_BTN_BG);
        gfx_toolbar_icon(bx + 4, by + (TB_BTN_H - 14) / 2, k_tbbtns[i].icon);
        unsigned fg = dim ? C_TEXT_DIM : C_TEXT;
        gfx_text_transparent((unsigned)(bx + 4 + 14 + 3),
                              (unsigned)(by + (TB_BTN_H - WGT_FONT_H) / 2),
                              k_tbbtns[i].label, fg);
    }

    /* Separator + account email display */
    int sx = ax + k_tbbtns[5].rx + k_tbbtns[5].w + 8;
    gfx_vline((unsigned)sx, (unsigned)(ay + 4), (unsigned)(TOOLBAR_H - 8), C_SEP);
    gfx_text_transparent((unsigned)(sx + 6),
                          (unsigned)(ay + (TOOLBAR_H - WGT_FONT_H) / 2),
                          g_acc.email, C_TEXT_DIM);

    /* Header row (directly below toolbar, inside the same widget) */
    int hy = ay + TOOLBAR_H;
    if (g_state == STATE_MAIN) {
        /* "FOLDERS" header */
        gfx_fill((unsigned)ax, (unsigned)hy,
                  (unsigned)SIDEBAR_W, (unsigned)HDR_H, C_HDR_BG);
        gfx_text_transparent((unsigned)(ax + 8), (unsigned)(hy + 2),
                              "FOLDERS", C_TEXT_DIM);
        /* Column label row */
        int rx2 = ax + RIGHT_X;
        gfx_fill((unsigned)rx2, (unsigned)hy,
                  (unsigned)RIGHT_W, (unsigned)HDR_H, C_HDR_BG);
        gfx_text_transparent((unsigned)(rx2 + 4),   (unsigned)(hy + 2), "FROM",    C_TEXT_DIM);
        gfx_text_transparent((unsigned)(rx2 + 164), (unsigned)(hy + 2), "SUBJECT", C_TEXT_DIM);
        gfx_text_transparent((unsigned)(rx2 + 534), (unsigned)(hy + 2), "DATE",    C_TEXT_DIM);
        gfx_hline((unsigned)rx2, (unsigned)(hy + HDR_H - 1), (unsigned)RIGHT_W, C_SEP);
    } else if (g_state == STATE_COMPOSE) {
        gfx_fill((unsigned)ax, (unsigned)hy,
                  (unsigned)CONT_W, (unsigned)HDR_H, C_WIN_BG);
    }
}

static int toolbar_event(widget_t *w, const widget_event_t *ev)
{
    (void)w;
    if (ev->type != WEV_MOUSE_DOWN) return 0;

    int rx = ev->mx - g_tb_ax;
    int ry = ev->my - g_tb_ay;
    if (ry < 0 || ry >= TOOLBAR_H) return 0;   /* only the button strip */

    for (int i = 0; i < 6; i++) {
        if (rx >= k_tbbtns[i].rx && rx < k_tbbtns[i].rx + k_tbbtns[i].w) {
            int id = k_tbbtns[i].id;
            if (id == TB_SETTINGS) {
                g_want_settings = 1;
                g_ctx.running   = 0;
                return 1;
            }
            if (g_state == STATE_COMPOSE) {
                if (id == TB_COMPOSE) enter_compose(0);
            } else {
                switch (id) {
                case TB_COMPOSE: enter_compose(0); break;
                case TB_REPLY:   enter_compose(1); break;
                case TB_FORWARD: enter_compose(0); break;
                case TB_DELETE:  do_delete();      break;
                case TB_REFRESH: do_refresh();     break;
                }
            }
            return 1;
        }
    }
    return 0;
}

/* ── Main widget tree ────────────────────────────────────────────────────── */

static widget_t g_root;

static widget_t g_toolbar;
static widget_t g_folder_list;
static widget_t g_div;
static widget_t g_msg_list;
static widget_t g_sep;
static widget_t g_body_ta;
static widget_t g_body_sb;

static widget_t g_co_to_lbl;
static widget_t g_co_to;
static widget_t g_co_subj_lbl;
static widget_t g_co_subj;
static widget_t g_co_body;
static widget_t g_co_send;
static widget_t g_co_cancel;   /* total: 15 — one slot to spare */

/* ── Widget visibility ───────────────────────────────────────────────────── */

static void show_mail(int vis)
{
    int h = !vis;
    g_folder_list.hidden = h;
    g_div.hidden         = h;
    g_msg_list.hidden    = h;
    g_sep.hidden         = h;
    g_body_ta.hidden     = h;
    g_body_sb.hidden     = h;
    widget_invalidate_all(&g_root);
}

static void show_compose(int vis)
{
    int h = !vis;
    g_co_to_lbl.hidden   = h;
    g_co_to.hidden       = h;
    g_co_subj_lbl.hidden = h;
    g_co_subj.hidden     = h;
    g_co_body.hidden     = h;
    g_co_send.hidden     = h;
    g_co_cancel.hidden   = h;
    widget_invalidate_all(&g_root);
}

/* ── Folder listview ─────────────────────────────────────────────────────── */

static void folder_list_populate(void)
{
    listview_clear(&g_folder_list);
    listview_add_item(&g_folder_list, "  Inbox",  (void *)0);
    listview_add_item(&g_folder_list, "  Sent",   (void *)1);
    listview_add_item(&g_folder_list, "  Drafts", (void *)2);
    listview_add_item(&g_folder_list, "  Trash",  (void *)3);
    widget_invalidate(&g_folder_list);
}

/* ── Message listview ────────────────────────────────────────────────────── */

static void msg_list_rebuild(void)
{
    listview_clear(&g_msg_list);
    for (int i = 0; i < g_msg_count; i++) {
        static char lbl[64];
        snprintf(lbl, sizeof(lbl), "%-20.20s %-34.34s %.8s",
                 g_msgs[i].from[0]    ? g_msgs[i].from    : "(no sender)",
                 g_msgs[i].subject[0] ? g_msgs[i].subject : "(no subject)",
                 g_msgs[i].date);
        listview_add_item(&g_msg_list, lbl, (void *)(long)i);
    }
    widget_invalidate(&g_msg_list);
}

/* ── IMAP helpers ────────────────────────────────────────────────────────── */

static void status(const char *msg)
{
    strncpy(g_status, msg, sizeof(g_status) - 1);
    g_status[sizeof(g_status) - 1] = '\0';
    draw_status();
}

static void do_connect(void)
{
    if (g_imap_ok) { imap_logout(&g_imap); g_imap_ok = 0; }
    status("Resolving...");
    if (imap_connect(&g_imap, g_acc.imap_host, g_acc.imap_port, g_acc.imap_tls) < 0) {
        static char m[96]; snprintf(m, sizeof(m), "Cannot connect to %s", g_acc.imap_host);
        status(m); return;
    }
    status("Authenticating...");
    if (imap_login(&g_imap, g_acc.imap_user, g_acc.imap_pass) <= 0) {
        status("Login failed — check credentials."); imap_logout(&g_imap); return;
    }
    g_imap_ok = 1;
    static char m[96]; snprintf(m, sizeof(m), "Connected to %s", g_acc.imap_host);
    status(m);
}

static void do_refresh(void)
{
    if (!g_imap_ok) { do_connect(); if (!g_imap_ok) return; }
    status("Selecting INBOX...");
    if (imap_select(&g_imap, "INBOX") <= 0) { status("Failed to select INBOX."); return; }
    int total = g_imap.selected_exists;
    if (total <= 0) {
        g_msg_count = 0; msg_list_rebuild();
        static char m[80]; snprintf(m, sizeof(m), "INBOX empty — %s", g_acc.imap_host);
        status(m); return;
    }
    int first = total - MAIL_MSG_MAX + 1;
    if (first < 1) first = 1;
    status("Fetching headers...");
    int count = 0;
    if (imap_fetch_headers(&g_imap, first, total, g_msgs, MAIL_MSG_MAX, &count) < 0) {
        status("Error fetching headers."); return;
    }
    g_msg_count = count; g_msg_sel = -1;
    msg_list_rebuild();
    textarea_set_text(&g_body_ta, ""); widget_invalidate(&g_body_ta);
    static char m[80];
    snprintf(m, sizeof(m), "INBOX: %d messages — %s", g_msg_count, g_acc.imap_host);
    status(m);
}

static void do_fetch_body(int idx)
{
    if (idx < 0 || idx >= g_msg_count || !g_imap_ok) return;
    mail_msg_t *msg = &g_msgs[idx];

    if (!msg->body[0]) {
        status("Fetching message...");
        imap_fetch_body(&g_imap, msg->seq_num, msg->body, MAIL_BODY_MAX);
    }

    static char disp[MAIL_BODY_MAX + 300];
    snprintf(disp, sizeof(disp),
        "From:    %s\n"
        "To:      %s\n"
        "Subject: %s\n"
        "Date:    %s\n"
        "─────────────────────────────────────────────────────\n\n"
        "%s",
        msg->from, g_acc.email, msg->subject, msg->date, msg->body);
    textarea_set_text(&g_body_ta, disp);
    textarea_scroll_to_bottom(&g_body_ta);
    widget_invalidate(&g_body_ta);

    static char m[80];
    snprintf(m, sizeof(m), "INBOX: %d messages — %s", g_msg_count, g_acc.imap_host);
    status(m);
}

static void do_delete(void)
{
    if (g_msg_sel < 0) { status("Select a message to delete."); return; }
    if (!g_imap_ok)    { status("Not connected.");              return; }
    status("Deleting...");
    imap_delete_and_expunge(&g_imap, g_msgs[g_msg_sel].seq_num);
    g_msg_sel = -1;
    do_refresh();
}

/* ── Compose / send ──────────────────────────────────────────────────────── */

static void do_send(void)
{
    const char *to      = textinput_get_text(&g_co_to);
    const char *subject = textinput_get_text(&g_co_subj);
    static char body[MAIL_BODY_MAX];
    textarea_get_text(&g_co_body, body, sizeof(body));

    if (!to || !to[0]) { status("Enter recipient address."); return; }
    status("Sending...");
    static char err[128];
    if (smtp_send(&g_acc, to,
                  subject ? subject : "(no subject)",
                  body, err, sizeof(err)) < 0) {
        static char m[160];
        snprintf(m, sizeof(m), "Send failed: %s", err);
        status(m); return;
    }
    status("Message sent.");
    enter_main();
}

/* ── State transitions ───────────────────────────────────────────────────── */

static void enter_main(void)
{
    g_state = STATE_MAIN;
    show_compose(0);
    show_mail(1);
    widget_invalidate(&g_toolbar);
}

static void enter_compose(int reply)
{
    g_state = STATE_COMPOSE;
    show_mail(0);
    show_compose(1);
    widget_invalidate(&g_toolbar);

    textinput_clear(&g_co_to);
    textinput_clear(&g_co_subj);
    textarea_set_text(&g_co_body, "");

    if (reply && g_msg_sel >= 0) {
        textinput_set_text(&g_co_to,   g_reply_to);
        textinput_set_text(&g_co_subj, g_reply_subj);
        /* Quote original body */
        static char qbody[MAIL_BODY_MAX];
        snprintf(qbody, sizeof(qbody), "\n\n--- Original message ---\n%s",
                 g_msgs[g_msg_sel].body);
        textarea_set_text(&g_co_body, qbody);
    }
    widget_set_focused(&g_co_to);
}

/* ── Widget callbacks ────────────────────────────────────────────────────── */

static void on_folder_sel(widget_t *w, int idx, void *ud)
{
    (void)w; (void)ud; (void)idx;
    do_refresh();
}

static void on_msg_sel(widget_t *w, int idx, void *ud)
{
    (void)w; (void)ud;
    g_msg_sel = idx;
    if (idx >= 0) {
        strncpy(g_reply_to, g_msgs[idx].from, MAIL_FROM_MAX - 1);
        snprintf(g_reply_subj, sizeof(g_reply_subj), "Re: %s", g_msgs[idx].subject);
        do_fetch_body(idx);
    }
}

static void on_co_send(widget_t *w)   { (void)w; do_send();   }
static void on_co_cancel(widget_t *w) { (void)w; enter_main(); }

/* ── Chrome drawing (outside widget area) ────────────────────────────────── */

static void draw_status(void)
{
    int sx = g_win_x + SIDE_PAD;
    int sy = g_win_y + TITLE_H + ROOT_H;   /* = win_y + 610 */
    gfx_fill((unsigned)sx, (unsigned)sy,
              (unsigned)CONT_W, (unsigned)STATUS_H, C_GLASS_STATUS);
    gfx_hline((unsigned)sx, (unsigned)sy, (unsigned)CONT_W, C_SEP);
    unsigned dot = g_imap_ok ? C_GREEN : C_RED;
    gfx_fill((unsigned)(sx + 6), (unsigned)(sy + (STATUS_H - 8) / 2), 8, 8, dot);
    gfx_text_transparent((unsigned)(sx + 18),
                          (unsigned)(sy + (STATUS_H - WGT_FONT_H) / 2),
                          g_status, C_TEXT);
    if (g_imap_ok && g_imap.selected_unseen > 0) {
        static char u[24];
        snprintf(u, sizeof(u), "%d new", g_imap.selected_unseen);
        int uw = (int)strlen(u) * WGT_FONT_W;
        gfx_text_transparent(
            (unsigned)(g_win_x + WIN_W - SIDE_PAD - uw - 8),
            (unsigned)(sy + (STATUS_H - WGT_FONT_H) / 2), u, C_ACCENT);
    }
}

static void draw_chrome(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H,
                            TITLE_H, "AetherMail", 0);
    draw_status();
    widget_invalidate(&g_toolbar);
}

static void on_reposition(void *ud) { (void)ud; draw_chrome(); }

/* ── Per-frame hook (scrollbar sync) ─────────────────────────────────────── */

static void per_frame(void *ud)
{
    (void)ud;
    if (g_state != STATE_MAIN) return;
    wdata_textarea_t  *ta = &g_body_ta.data.textarea;
    wdata_scrollbar_t *sb = &g_body_sb.data.scrollbar;
    int vis = BODY_H / WGT_FONT_H;
    int mxv = (ta->n_lines > vis) ? ta->n_lines - vis : 0;
    if (sb->max != mxv) { sb->max = mxv; sb->page = vis; widget_invalidate(&g_body_sb); }
    static int prev_sb = 0;
    if (sb->value != prev_sb) {
        /* user moved scrollbar — push to textarea */
        ta->scroll_top = sb->value;
        prev_sb = sb->value;
        widget_invalidate(&g_body_ta);
    } else if (ta->scroll_top != sb->value) {
        /* textarea scrolled programmatically — push to scrollbar */
        sb->value = ta->scroll_top;
        prev_sb   = ta->scroll_top;
        widget_invalidate(&g_body_sb);
    }
}

/* ── Build main widget tree ──────────────────────────────────────────────── */

static void build_main_ui(void)
{
    widget_init_panel(&g_root, 0, 0, CONT_W, ROOT_H, C_WIN_BG);

    /* Custom toolbar widget */
    widget_init_panel(&g_toolbar, 0, 0, CONT_W, TLBAR_HDR_H, C_GLASS_TOOLBAR);
    g_toolbar.draw_fn  = toolbar_draw;
    g_toolbar.event_fn = toolbar_event;

    /* Folder sidebar */
    widget_init_listview(&g_folder_list, 0, TLBAR_HDR_H,
                         SIDEBAR_W, ROOT_H - TLBAR_HDR_H,
                         8, on_folder_sel);

    /* Accent divider */
    widget_init_panel(&g_div, SIDEBAR_W, TLBAR_HDR_H, DIV_W,
                      ROOT_H - TLBAR_HDR_H, C_GLOW_DIV);

    /* Message list (right pane, top) */
    widget_init_listview(&g_msg_list, RIGHT_X, TLBAR_HDR_H,
                         RIGHT_W, MSGLIST_H, MAIL_MSG_MAX, on_msg_sel);

    /* Separator */
    widget_init_panel(&g_sep, RIGHT_X, SEP_Y, RIGHT_W, 1, C_ACCENT);

    /* Body textarea + scrollbar */
    widget_init_textarea(&g_body_ta, RIGHT_X, BODY_Y, BODY_TA_W, BODY_H, 512);
    widget_init_scrollbar_v(&g_body_sb, RIGHT_X + BODY_TA_W, BODY_Y,
                            BODY_SB_W, BODY_H, 0, BODY_H / WGT_FONT_H);


    /* Compose widgets (initially hidden) */
    widget_init_label(&g_co_to_lbl, 0, CO_Y0, CO_LBL_W, 26,
                      "To:", WGT_ALIGN_RIGHT);
    widget_init_textinput(&g_co_to, CO_INP_X, CO_Y0, CO_INP_W, 26,
                          NULL, NULL);
    widget_init_label(&g_co_subj_lbl, 0, CO_Y0 + 30, CO_LBL_W, 26,
                      "Subject:", WGT_ALIGN_RIGHT);
    widget_init_textinput(&g_co_subj, CO_INP_X, CO_Y0 + 30, CO_INP_W, 26,
                          NULL, NULL);
    widget_init_textarea(&g_co_body, 0, CO_BODY_Y, CONT_W, CO_BODY_H, 256);
    widget_init_button(&g_co_send,   CONT_W - 156, CO_BTN_Y, 74, 28,
                       "Send", on_co_send);
    widget_init_button(&g_co_cancel, CONT_W - 78,  CO_BTN_Y, 74, 28,
                       "Cancel", on_co_cancel);

    /* Wire child list */
    widget_add_child(&g_root, &g_toolbar);
    widget_add_child(&g_root, &g_folder_list);
    widget_add_child(&g_root, &g_div);
    widget_add_child(&g_root, &g_msg_list);
    widget_add_child(&g_root, &g_sep);
    widget_add_child(&g_root, &g_body_ta);
    widget_add_child(&g_root, &g_body_sb);
    widget_add_child(&g_root, &g_co_to_lbl);
    widget_add_child(&g_root, &g_co_to);
    widget_add_child(&g_root, &g_co_subj_lbl);
    widget_add_child(&g_root, &g_co_subj);
    widget_add_child(&g_root, &g_co_body);
    widget_add_child(&g_root, &g_co_send);
    widget_add_child(&g_root, &g_co_cancel);  /* 14 total */

    folder_list_populate();
    show_mail(1);
    show_compose(0);
}

/* ── Account setup (separate widget tree) ────────────────────────────────── */

static widget_t g_se_root;
static widget_t g_se_bg;
static widget_t g_se_email;
static widget_t g_se_pass;
static widget_t g_se_imap;
static widget_t g_se_smtp;
static widget_t g_se_save;
static widget_t g_se_status;

static int g_setup_done = 0;  /* set to 1 by save button → exits setup loop */

static void se_panel_draw(widget_t *w, int ax, int ay)
{
    (void)w;
    int pw = 460, ph = 300;
    int px = ax + (CONT_W - pw) / 2;
    int py = ay + (ROOT_H - ph) / 2 - 20;

    /* Panel background */
    gfx_fill((unsigned)ax, (unsigned)ay, (unsigned)CONT_W,
              (unsigned)ROOT_H, C_WIN_BG);
    gfx_fill_rounded((unsigned)px, (unsigned)py, (unsigned)pw, (unsigned)ph,
                      8, C_SETUP_PANEL);
    gfx_rect_rounded((unsigned)px, (unsigned)py, (unsigned)pw, (unsigned)ph,
                      8, C_ACCENT);

    /* Title */
    gfx_text_center_transparent((unsigned)px, (unsigned)pw,
                                 (unsigned)(py + 10), "Account Setup", C_TEXT);
    gfx_hline((unsigned)px, (unsigned)(py + 28), (unsigned)pw, C_SEP);

    /* Field labels — aligned with input rows (44 px apart) */
    int lx  = px + 8;
    int fy0 = py + 40;
    gfx_text_transparent((unsigned)lx, (unsigned)(fy0 +  8),       "Email:",       C_TEXT);
    gfx_text_transparent((unsigned)lx, (unsigned)(fy0 + 52),       "Password:",    C_TEXT);
    gfx_text_transparent((unsigned)lx, (unsigned)(fy0 + 96),       "IMAP server:", C_TEXT);
    gfx_text_transparent((unsigned)lx, (unsigned)(fy0 + 140),      "SMTP server:", C_TEXT);
    /* Hint below SMTP input, above the Save button */
    gfx_text_transparent((unsigned)lx, (unsigned)(fy0 + 170),
                          "Ports default to 993/465 (IMAPS/SMTPS)", C_TEXT_DIM);
}

static void on_se_save(widget_t *btn)
{
    (void)btn;
    const char *email = textinput_get_text(&g_se_email);
    const char *pass  = textinput_get_text(&g_se_pass);
    const char *imap  = textinput_get_text(&g_se_imap);
    const char *smtp  = textinput_get_text(&g_se_smtp);

    if (!email || !email[0] || !imap || !imap[0]) {
        label_set_text(&g_se_status, "Email and IMAP server are required.");
        return;
    }

    memset(&g_acc, 0, sizeof(g_acc));

    strncpy(g_acc.email,     email, MAIL_EMAIL_MAX - 1);
    strncpy(g_acc.imap_host, imap,  MAIL_HOST_MAX  - 1);
    strncpy(g_acc.imap_user, email, MAIL_USER_MAX  - 1);
    strncpy(g_acc.imap_pass, pass ? pass : "", MAIL_PASS_MAX - 1);
    g_acc.imap_port = 993;
    g_acc.imap_tls  = 1;

    const char *sh = (smtp && smtp[0]) ? smtp : imap;
    strncpy(g_acc.smtp_host, sh, MAIL_HOST_MAX - 1);
    strncpy(g_acc.smtp_user, email, MAIL_USER_MAX - 1);
    strncpy(g_acc.smtp_pass, pass ? pass : "", MAIL_PASS_MAX - 1);
    g_acc.smtp_port = 465;
    g_acc.smtp_tls  = 1;

    /* Extract display name from email (before @) */
    const char *at = strchr(email, '@');
    int nlen = at ? (int)(at - email) : (int)strlen(email);
    if (nlen > MAIL_NAME_MAX - 1) nlen = MAIL_NAME_MAX - 1;
    strncpy(g_acc.display_name, email, (unsigned)nlen);
    g_acc.display_name[nlen] = '\0';

    if (mail_config_save(&g_acc) < 0)
        label_set_text(&g_se_status, "Could not write /email/config.");
    else {
        label_set_text(&g_se_status, "Saved.");
        g_setup_done = 1;
        /* Exit setup widget_run */
        /* We signal via the label text; the se_ctx.running is not directly
         * accessible here — we store a flag and let main() check it. */
    }
}

static widget_ctx_t g_se_ctx;

/* Hack: per_frame checks g_setup_done and stops the loop */
static void se_per_frame(void *ud)
{
    (void)ud;
    if (g_setup_done) g_se_ctx.running = 0;
}

static void run_setup(void)
{
    int pw = 460;
    int px_off = (CONT_W - pw) / 2;  /* offset within content area */
    /* py0: widget-space y of the first input row.
     * Must match se_panel_draw's fy0 = py+40 where py=(ROOT_H-300)/2-20. */
    int py0    = (ROOT_H - 300) / 2 - 20 + 40;  /* first field y in widget space */
    int inp_x  = px_off + 116;
    int inp_w  = pw - 116 - 12;

    widget_init_panel(&g_se_root, 0, 0, CONT_W, ROOT_H, C_WIN_BG);

    /* Custom background / labels panel */
    widget_init_panel(&g_se_bg, 0, 0, CONT_W, ROOT_H, C_WIN_BG);
    g_se_bg.draw_fn = se_panel_draw;

    /* Input fields — 44 px row pitch (26 px tall + 18 px gap) */
    widget_init_textinput(&g_se_email, inp_x, py0,       inp_w, 26, NULL, NULL);
    widget_init_textinput(&g_se_pass,  inp_x, py0 +  44, inp_w, 26, NULL, NULL);
    widget_init_textinput(&g_se_imap,  inp_x, py0 +  88, inp_w, 26, NULL, NULL);
    widget_init_textinput(&g_se_smtp,  inp_x, py0 + 132, inp_w, 26, NULL, NULL);
    /* 26px gap after last input (bottom at py0+158), then 18px hint, then 10px */
    widget_init_button(   &g_se_save,  inp_x, py0 + 194, 160, 28,
                          "Save & Connect", on_se_save);
    widget_init_label(    &g_se_status, px_off, py0 + 232, pw, 20,
                          "Enter your account details.",
                          WGT_ALIGN_CENTER);

    widget_add_child(&g_se_root, &g_se_bg);
    widget_add_child(&g_se_root, &g_se_email);
    widget_add_child(&g_se_root, &g_se_pass);
    widget_add_child(&g_se_root, &g_se_imap);
    widget_add_child(&g_se_root, &g_se_smtp);
    widget_add_child(&g_se_root, &g_se_save);
    widget_add_child(&g_se_root, &g_se_status);

    widget_set_focused(&g_se_email);

    g_se_ctx.win_x         = &g_win_x;
    g_se_ctx.win_y         = &g_win_y;
    g_se_ctx.content_dx    = SIDE_PAD;
    g_se_ctx.content_dy    = TITLE_H;
    g_se_ctx.win_id        = (int)g_win_id;
    g_se_ctx.win_w         = WIN_W;
    g_se_ctx.win_h         = WIN_H;
    g_se_ctx.on_reposition = on_reposition;
    g_se_ctx.per_frame_fn  = se_per_frame;
    g_se_ctx.userdata      = NULL;
    g_se_ctx.running       = 1;

    widget_run(&g_se_root, &g_se_ctx);
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "AetherMail");
    draw_chrome();

    /* ── Phase 1: initial config / first-run setup ──────────────────────── */
    if (!mail_config_load(&g_acc)) {
        g_setup_done = 0;
        run_setup();
        if (!g_setup_done) goto done;
    }

    /* ── Phase 2: main mail loop (re-entered after Settings) ────────────── */
    for (;;) {
        g_want_settings = 0;

        build_main_ui();
        draw_chrome();
        do_connect();
        if (g_imap_ok) do_refresh();

        g_ctx.win_x         = &g_win_x;
        g_ctx.win_y         = &g_win_y;
        g_ctx.content_dx    = SIDE_PAD;
        g_ctx.content_dy    = TITLE_H;
        g_ctx.win_id        = (int)g_win_id;
        g_ctx.win_w         = WIN_W;
        g_ctx.win_h         = WIN_H;
        g_ctx.on_reposition = on_reposition;
        g_ctx.per_frame_fn  = per_frame;
        g_ctx.userdata      = NULL;
        g_ctx.running       = 1;
        widget_run(&g_root, &g_ctx);

        if (!g_want_settings) break;   /* normal exit */

        /* Settings button pressed — re-run setup then rebuild */
        if (g_imap_ok) { imap_logout(&g_imap); g_imap_ok = 0; }
        g_setup_done = 0;
        run_setup();
        if (!g_setup_done) break;      /* user closed without saving */
    }

done:
    if (g_imap_ok) imap_logout(&g_imap);
    sys_wm_request_close(g_win_id);
    return 0;
}
