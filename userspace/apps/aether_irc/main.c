/*
 * AetherOS — AetherIRC
 * File: userspace/apps/aether_irc/main.c
 *
 * RFC 1459 / RFC 2812 IRC client with Lumina glassmorphism UI (libwidget + GPU BO).
 *
 * Window: 860×560.  Three-pane layout inside content area (844×516):
 *
 *   [  0.. 33]  Toolbar: Server / Port / Nick inputs + Connect button
 *   [ 34.. 34]  Separator (glass)
 *   [ 35.. 52]  Status label
 *   [ 53.. 53]  Separator (accent)
 *   [ 54..484]  Body (431 px):
 *                 Left  130 px  — channel/DM list (listview)
 *                 |vsep|
 *                 Center 562 px — chat textarea + 10 px scrollbar
 *                 |vsep|
 *                 Right 140 px  — user list (listview)
 *   [484..484]  Separator (accent)
 *   [486..515]  Input bar: message textinput + Refresh + Send buttons
 *
 * Network model:
 *   sys_net_recv() blocks for up to 5 s in the AetherOS kernel when no data
 *   is pending.  To keep the UI responsive, receive is only triggered by:
 *     - User sending a message (Enter / Send button)
 *     - Refresh button or F5 key (when g_inp_msg is focused)
 *     - Slash commands that send data to the server
 *
 * Slash commands:
 *   /join #channel       — join a channel
 *   /part [#channel]     — leave current or named channel
 *   /nick newnick        — change nickname
 *   /quit [reason]       — disconnect gracefully
 *   /msg nick text       — open private message
 *   /me action           — CTCP ACTION emote
 *   Empty input + Enter  — manual refresh (same as Refresh button)
 */

#include <gfx.h>
#include <gpu.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <notif.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "irc.h"

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define WIN_W       860
#define WIN_H       560
#define TITLE_H      32
#define SIDE_PAD      8
#define CONT_PAD      6

#define WIN_X_INIT   70
#define WIN_Y_INIT   42   /* 4 px below topbar */

#define CONT_W  (WIN_W - 2 * SIDE_PAD)           /* 844 */
#define CONT_H  (WIN_H - TITLE_H - 2 * CONT_PAD) /* 516 */

/* Toolbar row (y = 0 .. TOOLBAR_H-1, all in widget-coords) */
#define TOOLBAR_H    34
#define SERV_INP_X   60    /* after "Server:" label (56 px + 4 gap) */
#define SERV_INP_W  238
#define PORT_INP_X  348    /* after "Port:" label  (305+40+3 gap) */
#define PORT_INP_W   68
#define NICK_INP_X  466    /* after "Nick:" label  (422+40+4 gap) */
#define NICK_INP_W  134
#define CONN_BTN_X  606
#define CONN_BTN_W  132

/* Status strip */
#define STATUS_Y     36
#define STATUS_H     16

/* Body area */
#define BODY_Y       54
#define BODY_H      431

/* Vertical dividers (drawn in draw_chrome) */
#define CHAN_W      130   /* channel list width  */
#define CHAT_X      131   /* chat textarea x     */
#define CHAT_W      562   /* chat textarea width */
#define CHAT_SB_X   693   /* scrollbar x         */
#define CHAT_SB_W    10
#define USER_X      704   /* user list x         */
#define USER_W      140   /* user list width     */

/* Input bar */
#define INPUT_Y     486
#define INPUT_H      30
#define MSG_INP_W   582
#define REFRESH_BTN_X  584
#define REFRESH_BTN_W  126
#define SEND_BTN_X     712
#define SEND_BTN_W     132

/* ── Extra Lumina colours for AetherIRC ──────────────────────────────────── */
#define C_IRC_TOOLBAR  GFX_RGB( 20,  18,  42)
#define C_IRC_STATUS   GFX_RGB( 18,  16,  34)
#define C_IRC_INPUT    GFX_RGB( 22,  20,  42)
#define C_IRC_GLOW     GFX_RGB( 68,  56, 132)
#define C_GLASS_HIGH   GFX_RGB( 82,  70, 158)

/* Auto-refresh: poll every 3 s when visible, every 1 s when minimized */
#define POLL_TICKS_NORMAL    300L    /*  3 s at 100 Hz */
#define POLL_TICKS_MINIMIZED 100L    /*  1 s at 100 Hz */

/* ── Global window state ─────────────────────────────────────────────────── */

static int  g_win_x      = WIN_X_INIT;
static int  g_win_y      = WIN_Y_INIT;
static long g_win_id     = -1;
static int  g_minimized  = 0;   /* updated by widget_run via minimized_flag */

/* Per-channel notification bitmask — cleared when app is restored */
static unsigned int g_notif_mask = 0;
/* Tick of last background recv */
static long g_last_poll_tick = 0;

/* ── IRC protocol state ──────────────────────────────────────────────────── */

static irc_conn_t g_conn;

/* ── Channel state ───────────────────────────────────────────────────────── */

static irc_chan_t g_chans[IRC_MAX_CHANS];
static int        g_n_chans     = 0;
static int        g_current_chan = 0;

/* ── Widget declarations ─────────────────────────────────────────────────── */

static widget_t g_root;

/* Toolbar */
static widget_t g_inp_server;
static widget_t g_inp_port;
static widget_t g_inp_nick;
static widget_t g_btn_connect;

/* Status */
static widget_t g_lbl_status;

/* Body */
static widget_t g_chan_list;
static widget_t g_chat;
static widget_t g_chat_sb;
static widget_t g_user_list;

/* Input bar */
static widget_t g_inp_msg;
static widget_t g_btn_refresh;
static widget_t g_btn_send;

/* Saved default event_fn for g_inp_msg (overridden to intercept F5) */
static widget_event_fn g_inp_msg_orig_fn;

/* ── Forward declarations ────────────────────────────────────────────────── */

static void rebuild_chan_list(void);
static void rebuild_user_list(void);
static void sync_chat_scrollbar(void);
static void switch_to_chan(int idx);
static void do_recv(void);
static void on_irc_line(const char *line, void *ud);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int irc_strcmp_ci(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
        a++; b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static const char *strip_mode_prefix(const char *nick)
{
    while (*nick == '@' || *nick == '+' || *nick == '%' ||
           *nick == '~' || *nick == '&')
        nick++;
    return nick;
}

static void extract_first_word(const char *src, char *out, int max)
{
    int i = 0;
    while (src[i] && src[i] != ' ' && src[i] != '\t' && i < max - 1) {
        out[i] = src[i]; i++;
    }
    out[i] = 0;
}

/* Returns pointer to first '#' or '&' in params, or NULL. */
static const char *find_chan_in_str(const char *s)
{
    while (*s) {
        if (*s == '#' || *s == '&') return s;
        s++;
    }
    return NULL;
}

/* Copy channel name from ptr (starting at '#') into out. */
static void extract_channame(const char *ptr, char *out)
{
    int i = 0;
    while (ptr[i] && ptr[i] != ' ' && i < IRC_CHAN_MAX - 1) {
        out[i] = ptr[i]; i++;
    }
    out[i] = 0;
}

/* ── Channel management ──────────────────────────────────────────────────── */

static irc_chan_t *find_chan(const char *name)
{
    for (int i = 0; i < g_n_chans; i++) {
        if (irc_strcmp_ci(g_chans[i].name, name) == 0)
            return &g_chans[i];
    }
    return NULL;
}

static irc_chan_t *find_or_create_chan(const char *name)
{
    irc_chan_t *existing = find_chan(name);
    if (existing) { existing->active = 1; return existing; }
    if (g_n_chans >= IRC_MAX_CHANS) return NULL;
    irc_chan_t *ch = &g_chans[g_n_chans++];
    memset(ch, 0, sizeof(*ch));
    strncpy(ch->name, name, IRC_CHAN_MAX - 1);
    ch->active = 1;
    rebuild_chan_list();
    return ch;
}

static void add_user_to_chan(irc_chan_t *chan, const char *nick)
{
    if (!nick || !nick[0] || chan->n_users >= IRC_MAX_USERS) return;
    const char *clean = strip_mode_prefix(nick);
    for (int i = 0; i < chan->n_users; i++) {
        if (irc_strcmp_ci(strip_mode_prefix(chan->users[i]), clean) == 0) {
            strncpy(chan->users[i], nick, IRC_NICK_MAX - 1);
            return;
        }
    }
    strncpy(chan->users[chan->n_users++], nick, IRC_NICK_MAX - 1);
}

static void remove_user_from_chan(irc_chan_t *chan, const char *nick)
{
    const char *clean = strip_mode_prefix(nick);
    for (int i = 0; i < chan->n_users; i++) {
        if (irc_strcmp_ci(strip_mode_prefix(chan->users[i]), clean) == 0) {
            for (int j = i; j < chan->n_users - 1; j++)
                memcpy(chan->users[j], chan->users[j+1], (size_t)IRC_NICK_MAX);
            chan->n_users--;
            return;
        }
    }
}

static int user_in_chan(irc_chan_t *chan, const char *nick)
{
    const char *clean = strip_mode_prefix(nick);
    for (int i = 0; i < chan->n_users; i++) {
        if (irc_strcmp_ci(strip_mode_prefix(chan->users[i]), clean) == 0)
            return 1;
    }
    return 0;
}

static void rename_user_in_chan(irc_chan_t *chan, const char *old_nick, const char *new_nick)
{
    const char *old_clean = strip_mode_prefix(old_nick);
    for (int i = 0; i < chan->n_users; i++) {
        if (irc_strcmp_ci(strip_mode_prefix(chan->users[i]), old_clean) == 0) {
            strncpy(chan->users[i], new_nick, IRC_NICK_MAX - 1);
            return;
        }
    }
}

static void parse_names_into_chan(irc_chan_t *chan, const char *text)
{
    const char *p = text;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        char nick[IRC_NICK_MAX];
        int i = 0;
        while (*p && *p != ' ' && i < IRC_NICK_MAX - 1)
            nick[i++] = *p++;
        nick[i] = 0;
        if (i > 0) add_user_to_chan(chan, nick);
    }
}

/* ── Channel buffer helpers ──────────────────────────────────────────────── */

static void chan_append(irc_chan_t *chan, const char *text)
{
    int blen = (int)strlen(chan->buf);
    int tlen = (int)strlen(text);

    if (blen + tlen >= IRC_CHAN_BUF - 1) {
        int discard = IRC_CHAN_BUF / 3;
        const char *nl = chan->buf + discard;
        while (*nl && *nl != '\n') nl++;
        if (*nl == '\n') nl++;
        int off = (int)(nl - chan->buf);
        memmove(chan->buf, nl, (size_t)(blen - off + 1));
        blen -= off;
    }

    if (tlen > 0 && blen + tlen < IRC_CHAN_BUF - 1)
        memcpy(chan->buf + blen, text, (size_t)(tlen + 1));

    if (chan == &g_chans[g_current_chan]) {
        textarea_set_text(&g_chat, chan->buf);
        textarea_scroll_to_bottom(&g_chat);
        sync_chat_scrollbar();
    } else {
        chan->unread = 1;
    }
}

static void server_append(const char *text)
{
    chan_append(&g_chans[0], text);
}

/* ── UI rebuild helpers ──────────────────────────────────────────────────── */

static void sync_chat_scrollbar(void)
{
    wdata_textarea_t  *td = &g_chat.data.textarea;
    wdata_scrollbar_t *sb = &g_chat_sb.data.scrollbar;
    int visible = (BODY_H - 8) / WGT_FONT_H;
    sb->max   = (td->n_lines > visible) ? (td->n_lines - visible) : 0;
    sb->page  = visible;
    sb->value = td->scroll_top;
    widget_invalidate(&g_chat_sb);
}

static void rebuild_user_list(void)
{
    irc_chan_t *chan = &g_chans[g_current_chan];
    listview_clear(&g_user_list);
    for (int i = 0; i < chan->n_users; i++)
        listview_add_item(&g_user_list, chan->users[i], (void *)(long)i);
    widget_invalidate(&g_user_list);
}

static void rebuild_chan_list(void)
{
    listview_clear(&g_chan_list);
    int sel_row = -1, row = 0;
    for (int i = 0; i < g_n_chans; i++) {
        if (i > 0 && !g_chans[i].active) continue;
        char label[IRC_CHAN_MAX + 3];
        if (g_chans[i].unread)
            snprintf(label, sizeof(label), "* %s", g_chans[i].name);
        else
            strncpy(label, g_chans[i].name, sizeof(label) - 1);
        listview_add_item(&g_chan_list, label, (void *)(long)i);
        if (i == g_current_chan) sel_row = row;
        row++;
    }
    g_chan_list.data.listview.selected = sel_row;
    widget_invalidate(&g_chan_list);
}

static void update_status_for_chan(int idx)
{
    if (g_conn.fd < 0 || !g_conn.logged_in) return;
    static char st[160];
    snprintf(st, sizeof(st), "Connected as %s  |  %s  |  F5=refresh  F6=test notif",
             g_conn.nick, g_chans[idx].name);
    label_set_text(&g_lbl_status, st);
}

static void switch_to_chan(int idx)
{
    if (idx < 0 || idx >= g_n_chans) return;
    g_current_chan   = idx;
    g_chans[idx].unread = 0;
    textarea_set_text(&g_chat, g_chans[idx].buf);
    textarea_scroll_to_bottom(&g_chat);
    sync_chat_scrollbar();
    rebuild_user_list();
    rebuild_chan_list();
    update_status_for_chan(idx);
    widget_invalidate_all(&g_root);
}

/* ── IRC line handler ────────────────────────────────────────────────────── */

static void on_irc_line(const char *line, void *ud)
{
    (void)ud;
    irc_msg_t msg;
    if (!irc_parse(line, &msg)) return;

    int cmd_num = atoi(msg.command);

    /* ── 001 RPL_WELCOME ─────────────────────────────────────────────────── */
    if (strcmp(msg.command, "001") == 0) {
        g_conn.logged_in = 1;
        char welcome[IRC_TEXT_MAX + 8];
        snprintf(welcome, sizeof(welcome), "** %s\n", msg.text);
        server_append(welcome);
        update_status_for_chan(g_current_chan);
        widget_invalidate(&g_lbl_status);
        return;
    }

    /* ── ERROR (server disconnect) ───────────────────────────────────────── */
    if (strcmp(msg.command, "ERROR") == 0) {
        char line2[IRC_TEXT_MAX + 12];
        snprintf(line2, sizeof(line2), "!! ERROR: %s\n", msg.has_text ? msg.text : msg.params);
        server_append(line2);
        switch_to_chan(0);
        irc_disconnect(&g_conn);
        label_set_text(&g_lbl_status, "Disconnected — server error.");
        strncpy(g_btn_connect.data.button.text, "Connect", 127);
        widget_invalidate(&g_btn_connect);
        g_inp_msg.hidden    = 1;
        g_btn_refresh.hidden = 1;
        g_btn_send.hidden   = 1;
        widget_invalidate_all(&g_root);
        return;
    }

    /* ── Server info numerics (002-099) ──────────────────────────────────── */
    if (cmd_num >= 2 && cmd_num <= 99) {
        char line2[IRC_TEXT_MAX + 8];
        snprintf(line2, sizeof(line2), "** %s\n", msg.has_text ? msg.text : msg.params);
        server_append(line2);
        return;
    }

    /* ── MOTD (372 body, 375 start, 376/422 end) ─────────────────────────── */
    if (cmd_num == 375 || cmd_num == 372) {
        char line2[IRC_TEXT_MAX + 4];
        snprintf(line2, sizeof(line2), "  %s\n", msg.text);
        server_append(line2);
        return;
    }
    if (cmd_num == 376 || cmd_num == 422) {
        server_append("-- End of MOTD --\n");
        return;
    }

    /* ── LUSERS / server stats ───────────────────────────────────────────── */
    if (cmd_num == 251 || cmd_num == 252 || cmd_num == 253 ||
        cmd_num == 254 || cmd_num == 255 || cmd_num == 265 ||
        cmd_num == 266) {
        char line2[IRC_TEXT_MAX + 8];
        snprintf(line2, sizeof(line2), "** %s\n", msg.has_text ? msg.text : msg.params);
        server_append(line2);
        return;
    }

    /* ── 332 RPL_TOPIC ───────────────────────────────────────────────────── */
    if (cmd_num == 332) {
        const char *cp = find_chan_in_str(msg.params);
        if (cp) {
            char channame[IRC_CHAN_MAX];
            extract_channame(cp, channame);
            irc_chan_t *chan = find_chan(channame);
            if (chan) {
                char line2[IRC_CHAN_MAX + IRC_TEXT_MAX + 20];
                snprintf(line2, sizeof(line2), "** Topic for %s: %s\n", channame, msg.text);
                chan_append(chan, line2);
            }
        }
        return;
    }

    /* ── 353 RPL_NAMREPLY ────────────────────────────────────────────────── */
    if (cmd_num == 353) {
        const char *cp = find_chan_in_str(msg.params);
        if (cp) {
            char channame[IRC_CHAN_MAX];
            extract_channame(cp, channame);
            irc_chan_t *chan = find_or_create_chan(channame);
            if (chan) parse_names_into_chan(chan, msg.text);
        }
        return;
    }

    /* ── 366 RPL_ENDOFNAMES ──────────────────────────────────────────────── */
    if (cmd_num == 366) {
        const char *cp = find_chan_in_str(msg.params);
        if (cp) {
            char channame[IRC_CHAN_MAX];
            extract_channame(cp, channame);
            irc_chan_t *chan = find_chan(channame);
            if (chan && chan == &g_chans[g_current_chan])
                rebuild_user_list();
        } else {
            rebuild_user_list();
        }
        return;
    }

    /* ── 4xx/5xx error numerics ──────────────────────────────────────────── */
    if (cmd_num >= 400 && cmd_num < 600) {
        char line2[IRC_TEXT_MAX + 16];
        snprintf(line2, sizeof(line2), "! Error %s: %s\n",
                 msg.command, msg.has_text ? msg.text : msg.params);
        server_append(line2);
        return;
    }

    /* ── Other numerics → server log ─────────────────────────────────────── */
    if (cmd_num > 0) {
        char line2[IRC_TEXT_MAX + 8];
        snprintf(line2, sizeof(line2), "** %s\n", msg.has_text ? msg.text : msg.params);
        server_append(line2);
        return;
    }

    /* ── JOIN ────────────────────────────────────────────────────────────── */
    if (strcmp(msg.command, "JOIN") == 0) {
        const char *channame = msg.has_text ? msg.text : msg.params;
        irc_chan_t *chan = find_or_create_chan(channame);
        if (!chan) return;

        if (strcmp(msg.prefix, g_conn.nick) == 0) {
            char line2[IRC_CHAN_MAX + 24];
            snprintf(line2, sizeof(line2), "-- You joined %s\n", channame);
            chan->n_users = 0;   /* names list incoming */
            chan_append(chan, line2);
            rebuild_chan_list();
            switch_to_chan((int)(chan - g_chans));
        } else {
            char line2[IRC_NICK_MAX + IRC_CHAN_MAX + 24];
            snprintf(line2, sizeof(line2), ">> %s joined %s\n", msg.prefix, channame);
            chan_append(chan, line2);
            add_user_to_chan(chan, msg.prefix);
            if (chan == &g_chans[g_current_chan]) rebuild_user_list();
        }
        return;
    }

    /* ── PART ────────────────────────────────────────────────────────────── */
    if (strcmp(msg.command, "PART") == 0) {
        char channame[IRC_CHAN_MAX];
        extract_first_word(msg.params[0] ? msg.params : msg.text, channame, IRC_CHAN_MAX);
        irc_chan_t *chan = find_chan(channame);
        if (!chan) return;

        char line2[IRC_NICK_MAX + IRC_CHAN_MAX + IRC_TEXT_MAX + 16];
        if (strcmp(msg.prefix, g_conn.nick) == 0) {
            if (msg.has_text)
                snprintf(line2, sizeof(line2), "-- You left %s (%s)\n", channame, msg.text);
            else
                snprintf(line2, sizeof(line2), "-- You left %s\n", channame);
            chan_append(chan, line2);
            chan->active = 0;
            rebuild_chan_list();
            if (&g_chans[g_current_chan] == chan) switch_to_chan(0);
        } else {
            if (msg.has_text)
                snprintf(line2, sizeof(line2), "<< %s left %s (%s)\n", msg.prefix, channame, msg.text);
            else
                snprintf(line2, sizeof(line2), "<< %s left %s\n", msg.prefix, channame);
            chan_append(chan, line2);
            remove_user_from_chan(chan, msg.prefix);
            if (chan == &g_chans[g_current_chan]) rebuild_user_list();
        }
        return;
    }

    /* ── KICK ────────────────────────────────────────────────────────────── */
    if (strcmp(msg.command, "KICK") == 0) {
        char channame[IRC_CHAN_MAX], kicked[IRC_NICK_MAX];
        extract_first_word(msg.params, channame, IRC_CHAN_MAX);
        const char *rest = msg.params + strlen(channame);
        while (*rest == ' ') rest++;
        extract_first_word(rest, kicked, IRC_NICK_MAX);

        irc_chan_t *chan = find_chan(channame);
        if (!chan) return;

        char line2[IRC_NICK_MAX * 3 + IRC_TEXT_MAX + 32];
        snprintf(line2, sizeof(line2), "** %s was kicked from %s by %s (%s)\n",
                 kicked, channame, msg.prefix, msg.has_text ? msg.text : "");
        chan_append(chan, line2);
        remove_user_from_chan(chan, kicked);
        if (chan == &g_chans[g_current_chan]) rebuild_user_list();

        if (irc_strcmp_ci(kicked, g_conn.nick) == 0) {
            chan->active = 0;
            rebuild_chan_list();
            if (&g_chans[g_current_chan] == chan) switch_to_chan(0);
        }
        return;
    }

    /* ── PRIVMSG ─────────────────────────────────────────────────────────── */
    if (strcmp(msg.command, "PRIVMSG") == 0) {
        const char *target = msg.params;

        /* CTCP detection (\x01...\x01) */
        if (msg.text[0] == '\001') {
            if (strncmp(msg.text + 1, "ACTION ", 7) == 0) {
                /* /me action */
                irc_chan_t *chan;
                if (target[0] == '#' || target[0] == '&')
                    chan = find_or_create_chan(target);
                else
                    chan = find_or_create_chan(msg.prefix);
                if (chan) {
                    const char *action = msg.text + 8;
                    const char *action_end = strrchr(action, '\001');
                    char act[IRC_TEXT_MAX];
                    int alen = action_end ? (int)(action_end - action) : (int)strlen(action);
                    if (alen >= IRC_TEXT_MAX) alen = IRC_TEXT_MAX - 1;
                    memcpy(act, action, (size_t)alen);
                    act[alen] = 0;
                    char line2[IRC_NICK_MAX + IRC_TEXT_MAX + 4];
                    snprintf(line2, sizeof(line2), "* %s %s\n", msg.prefix, act);
                    chan_append(chan, line2);
                }
            } else if (strncmp(msg.text + 1, "VERSION", 7) == 0) {
                /* Reply to CTCP VERSION */
                if (g_conn.fd >= 0) {
                    char resp[80];
                    snprintf(resp, sizeof(resp),
                             "NOTICE %s :\001VERSION AetherIRC 1.0 / AetherOS\001",
                             msg.prefix);
                    irc_send_raw(&g_conn, resp);
                }
            }
            /* Other CTCPs silently ignored */
            return;
        }

        /* Regular message */
        irc_chan_t *chan;
        if (target[0] == '#' || target[0] == '&')
            chan = find_or_create_chan(target);
        else
            chan = find_or_create_chan(msg.prefix);   /* private message */
        if (!chan) return;

        char line2[IRC_NICK_MAX + IRC_TEXT_MAX + 8];
        snprintf(line2, sizeof(line2), "<%s> %s\n", msg.prefix, msg.text);
        chan_append(chan, line2);
        return;
    }

    /* ── NOTICE ──────────────────────────────────────────────────────────── */
    if (strcmp(msg.command, "NOTICE") == 0) {
        char line2[IRC_NICK_MAX + IRC_TEXT_MAX + 8];
        snprintf(line2, sizeof(line2), "-%s- %s\n",
                 msg.prefix[0] ? msg.prefix : "server", msg.text);
        server_append(line2);
        return;
    }

    /* ── QUIT ────────────────────────────────────────────────────────────── */
    if (strcmp(msg.command, "QUIT") == 0) {
        char line2[IRC_NICK_MAX + IRC_TEXT_MAX + 12];
        if (msg.has_text)
            snprintf(line2, sizeof(line2), "<< %s quit (%s)\n", msg.prefix, msg.text);
        else
            snprintf(line2, sizeof(line2), "<< %s quit\n", msg.prefix);

        for (int i = 0; i < g_n_chans; i++) {
            if (user_in_chan(&g_chans[i], msg.prefix)) {
                chan_append(&g_chans[i], line2);
                remove_user_from_chan(&g_chans[i], msg.prefix);
            }
        }
        if (g_chans[g_current_chan].active) rebuild_user_list();
        return;
    }

    /* ── NICK ────────────────────────────────────────────────────────────── */
    if (strcmp(msg.command, "NICK") == 0) {
        const char *new_nick = msg.has_text ? msg.text : msg.params;
        char line2[IRC_NICK_MAX * 2 + 32];

        if (irc_strcmp_ci(msg.prefix, g_conn.nick) == 0) {
            snprintf(line2, sizeof(line2), "** You are now known as %s\n", new_nick);
            strncpy(g_conn.nick, new_nick, IRC_NICK_MAX - 1);
            server_append(line2);
            update_status_for_chan(g_current_chan);
            widget_invalidate(&g_lbl_status);
        } else {
            snprintf(line2, sizeof(line2), "** %s is now known as %s\n", msg.prefix, new_nick);
            for (int i = 0; i < g_n_chans; i++) {
                if (user_in_chan(&g_chans[i], msg.prefix)) {
                    chan_append(&g_chans[i], line2);
                    rename_user_in_chan(&g_chans[i], msg.prefix, new_nick);
                }
            }
            if (g_chans[g_current_chan].active) rebuild_user_list();
        }
        return;
    }

    /* ── MODE ────────────────────────────────────────────────────────────── */
    if (strcmp(msg.command, "MODE") == 0) {
        char line2[IRC_TEXT_MAX + IRC_CHAN_MAX + 20];
        snprintf(line2, sizeof(line2), "** Mode %s %s\n", msg.params, msg.text);
        if (msg.params[0] == '#' || msg.params[0] == '&') {
            irc_chan_t *chan = find_chan(msg.params);
            if (chan) chan_append(chan, line2);
        } else {
            server_append(line2);
        }
        return;
    }

    /* ── TOPIC ───────────────────────────────────────────────────────────── */
    if (strcmp(msg.command, "TOPIC") == 0) {
        char channame[IRC_CHAN_MAX];
        extract_first_word(msg.params, channame, IRC_CHAN_MAX);
        irc_chan_t *chan = find_chan(channame);
        if (chan) {
            char line2[IRC_NICK_MAX + IRC_CHAN_MAX + IRC_TEXT_MAX + 24];
            snprintf(line2, sizeof(line2), "** %s changed topic of %s to: %s\n",
                     msg.prefix, channame, msg.text);
            chan_append(chan, line2);
        }
        return;
    }
}

/* ── Notifications ───────────────────────────────────────────────────────── */

static void post_notif(const char *chan, const char *msg)
{
    printf("[IRC] post_notif: chan='%s'\n", chan);

    notif_header_t hdr;
    notif_entry_t  entries[NOTIF_MAX];
    memset(&hdr,    0, sizeof(hdr));
    memset(entries, 0, sizeof(entries));
    unsigned cnt = 0;

    long vfd = sys_fs_open(NOTIF_PATH);
    if (vfd >= 0) {
        if (sys_fs_read(vfd, &hdr, (long)sizeof(hdr)) == (long)sizeof(hdr)
                && hdr.magic == NOTIF_MAGIC) {
            cnt = (hdr.count < (unsigned)NOTIF_MAX) ? hdr.count : (unsigned)NOTIF_MAX;
            sys_fs_read(vfd, entries, (long)(cnt * sizeof(notif_entry_t)));
        }
        sys_fs_close(vfd);
    }

    if (hdr.magic != NOTIF_MAGIC) {
        hdr.magic   = NOTIF_MAGIC;
        hdr.version = NOTIF_VERSION;
        hdr.count   = 0;
        hdr.unread  = 0;
        cnt         = 0;
    }

    if (cnt >= NOTIF_MAX) {
        memmove(entries, entries + 1, (NOTIF_MAX - 1) * sizeof(notif_entry_t));
        cnt = NOTIF_MAX - 1;
    }

    notif_entry_t *e = &entries[cnt++];
    memset(e, 0, sizeof(*e));
    e->id        = hdr.count + 1;
    e->timestamp = (unsigned int)sys_rtc_get();
    strncpy(e->app,   "AetherIRC",    NOTIF_APP_MAX   - 1);
    snprintf(e->title, NOTIF_TITLE_MAX, "New message in %s", chan);
    strncpy(e->msg,    msg,            NOTIF_MSG_MAX   - 1);

    hdr.count  = cnt;
    hdr.unread++;

    vfd = sys_fs_create(NOTIF_PATH);
    printf("[IRC] post_notif: create vfd=%ld cnt=%u\n", vfd, cnt);
    if (vfd >= 0) {
        long w1 = sys_fs_write(vfd, &hdr,    (long)sizeof(hdr));
        long w2 = sys_fs_write(vfd, entries, (long)(cnt * sizeof(notif_entry_t)));
        printf("[IRC] post_notif: wrote hdr=%ld entries=%ld\n", w1, w2);
        sys_fs_close(vfd);
    }
}

/* ── Background auto-refresh ─────────────────────────────────────────────── */

static void per_frame_poll(void *ud)
{
    (void)ud;
    if (g_conn.fd < 0 || !g_conn.logged_in) return;

    /* Clear per-channel notification state when window is restored. */
    static int prev_minimized = 0;
    if (prev_minimized && !g_minimized)
        g_notif_mask = 0;
    prev_minimized = g_minimized;

    long now      = sys_get_ticks();
    long interval = g_minimized ? POLL_TICKS_MINIMIZED : POLL_TICKS_NORMAL;
    if (now - g_last_poll_tick < interval) return;
    g_last_poll_tick = now;

    /* Snapshot unread flags before polling so we can detect new arrivals. */
    unsigned int prev_unread = 0;
    for (int i = 1; i < g_n_chans; i++)
        if (g_chans[i].unread) prev_unread |= (1u << i);

    int n = irc_recv_nb(&g_conn, on_irc_line, NULL);
    if (n <= 0) return;

    widget_invalidate_all(&g_root);

    /* Post a notification for each channel that became unread this poll.
     * When minimized: deduplicate per session via g_notif_mask.
     * When visible: always notify so the notification center can be tested. */
    for (int i = 1; i < g_n_chans; i++) {
        unsigned int bit = (1u << i);
        if (!g_chans[i].unread || (prev_unread & bit)) continue;
        if (g_minimized && (g_notif_mask & bit)) continue;
        if (g_minimized) g_notif_mask |= bit;
        post_notif(g_chans[i].name, "New message");
    }
}

/* ── Recv / poll ─────────────────────────────────────────────────────────── */

static void do_recv(void)
{
    if (g_conn.fd < 0) return;
    irc_recv(&g_conn, on_irc_line, NULL);
    widget_invalidate_all(&g_root);
}

/* Non-blocking drain: pick up any already-buffered server lines without
 * stalling the UI.  Used after sending commands so the UI stays responsive.
 * The background per_frame_poll() handles anything that arrives later. */
static void do_recv_nb(void)
{
    if (g_conn.fd < 0) return;
    for (int i = 0; i < 8; i++) {
        if (irc_recv_nb(&g_conn, on_irc_line, NULL) <= 0) break;
    }
    widget_invalidate_all(&g_root);
}

/* ── Slash command / send dispatch ───────────────────────────────────────── */

static void do_send(void)
{
    const char *text = textinput_get_text(&g_inp_msg);

    /* Empty input = manual refresh */
    if (!text || !text[0]) {
        do_recv();
        return;
    }

    if (text[0] == '/') {
        /* Slash commands */
        const char *cmd = text + 1;

        if (strncmp(cmd, "join ", 5) == 0) {
            const char *chan = cmd + 5;
            while (*chan == ' ') chan++;
            if (*chan && g_conn.fd >= 0) {
                irc_join(&g_conn, chan);
                do_recv_nb();
            }

        } else if (strncmp(cmd, "part", 4) == 0) {
            const char *rest = cmd + 4;
            while (*rest == ' ') rest++;
            if (g_conn.fd < 0) { server_append("! Not connected.\n"); }
            else {
                irc_chan_t *cur = &g_chans[g_current_chan];
                const char *target = *rest ? rest : (cur->active ? cur->name : NULL);
                if (target) { irc_part(&g_conn, target); do_recv_nb(); }
                else server_append("! No channel to part from.\n");
            }

        } else if (strncmp(cmd, "nick ", 5) == 0) {
            const char *nn = cmd + 5;
            while (*nn == ' ') nn++;
            if (*nn && g_conn.fd >= 0) {
                irc_nick_cmd(&g_conn, nn);
                strncpy(g_conn.nick, nn, IRC_NICK_MAX - 1);
                do_recv_nb();
            }

        } else if (strncmp(cmd, "quit", 4) == 0) {
            const char *reason = cmd + 4;
            while (*reason == ' ') reason++;
            if (g_conn.fd >= 0) {
                irc_quit(&g_conn, *reason ? reason : "AetherIRC — goodbye!");
                irc_disconnect(&g_conn);
            }
            label_set_text(&g_lbl_status, "Disconnected.");
            strncpy(g_btn_connect.data.button.text, "Connect", 127);
            widget_invalidate(&g_btn_connect);
            g_inp_msg.hidden    = 1;
            g_btn_refresh.hidden = 1;
            g_btn_send.hidden   = 1;
            widget_invalidate_all(&g_root);

        } else if (strncmp(cmd, "msg ", 4) == 0) {
            const char *rest = cmd + 4;
            while (*rest == ' ') rest++;
            char target[IRC_NICK_MAX];
            extract_first_word(rest, target, IRC_NICK_MAX);
            rest += strlen(target);
            while (*rest == ' ') rest++;
            if (*rest && g_conn.fd >= 0) {
                irc_privmsg(&g_conn, target, rest);
                irc_chan_t *chan = find_or_create_chan(target);
                if (chan) {
                    char line2[IRC_NICK_MAX + IRC_TEXT_MAX + 8];
                    snprintf(line2, sizeof(line2), "<%s> %s\n", g_conn.nick, rest);
                    chan_append(chan, line2);
                    rebuild_chan_list();
                    switch_to_chan((int)(chan - g_chans));
                }
                do_recv_nb();
            }

        } else if (strncmp(cmd, "me ", 3) == 0) {
            const char *action = cmd + 3;
            irc_chan_t *cur = &g_chans[g_current_chan];
            if (g_conn.fd >= 0 && cur->active) {
                char ctcp[IRC_TEXT_MAX + 16];
                snprintf(ctcp, sizeof(ctcp), "\001ACTION %s\001", action);
                irc_privmsg(&g_conn, cur->name, ctcp);
                char line2[IRC_NICK_MAX + IRC_TEXT_MAX + 4];
                snprintf(line2, sizeof(line2), "* %s %s\n", g_conn.nick, action);
                chan_append(cur, line2);
                do_recv_nb();
            }

        } else {
            char err[80];
            snprintf(err, sizeof(err), "! Unknown command: /%s\n", cmd);
            server_append(err);
        }

    } else {
        /* Regular message → PRIVMSG to current channel */
        irc_chan_t *cur = &g_chans[g_current_chan];
        if (g_conn.fd < 0) {
            server_append("! Not connected.\n");
        } else if (!cur->active || (cur->name[0] != '#' && cur->name[0] != '&'
                                    && irc_strcmp_ci(cur->name, "server") == 0)) {
            server_append("! No channel selected. Use /join #channel\n");
        } else {
            irc_privmsg(&g_conn, cur->name, text);
            char line2[IRC_NICK_MAX + IRC_TEXT_MAX + 8];
            snprintf(line2, sizeof(line2), "<%s> %s\n", g_conn.nick, text);
            chan_append(cur, line2);
            do_recv_nb();
        }
    }

    textinput_clear(&g_inp_msg);
    widget_invalidate(&g_inp_msg);
}

/* ── Connect / Disconnect callback ──────────────────────────────────────── */

static void on_connect_click(widget_t *btn)
{
    (void)btn;

    if (g_conn.fd >= 0) {
        /* Disconnect */
        irc_quit(&g_conn, "AetherIRC — goodbye!");
        irc_disconnect(&g_conn);
        label_set_text(&g_lbl_status, "Disconnected.");
        strncpy(g_btn_connect.data.button.text, "Connect", 127);
        widget_invalidate(&g_btn_connect);
        g_inp_msg.hidden    = 1;
        g_btn_refresh.hidden = 1;
        g_btn_send.hidden   = 1;
        widget_invalidate_all(&g_root);
        widget_set_focused(&g_inp_server);
        return;
    }

    const char *server  = textinput_get_text(&g_inp_server);
    const char *portstr = textinput_get_text(&g_inp_port);
    const char *nick    = textinput_get_text(&g_inp_nick);

    if (!server || !server[0]) {
        label_set_text(&g_lbl_status, "Enter a server address.");
        return;
    }
    if (!nick || !nick[0]) {
        label_set_text(&g_lbl_status, "Enter a nickname.");
        return;
    }

    unsigned short port = 6667;
    if (portstr && portstr[0]) {
        int p = atoi(portstr);
        if (p > 0 && p < 65536) port = (unsigned short)p;
    }

    static char status_buf[128];
    snprintf(status_buf, sizeof(status_buf), "Connecting to %s:%u …", server, (unsigned)port);
    label_set_text(&g_lbl_status, status_buf);
    widget_invalidate(&g_lbl_status);

    /* Clear server log for new session */
    g_chans[0].buf[0] = '\0';
    server_append("-- AetherIRC — connecting …\n");

    int r = irc_connect(&g_conn, server, port, nick, "aether");
    if (r < 0) {
        label_set_text(&g_lbl_status, "Connection failed — check server address.");
        g_chans[0].buf[0] = '\0';
        server_append("!! Connection failed.\n");
        return;
    }

    strncpy(g_btn_connect.data.button.text, "Disconnect", 127);
    widget_invalidate(&g_btn_connect);
    g_inp_msg.hidden    = 0;
    g_btn_refresh.hidden = 0;
    g_btn_send.hidden   = 0;
    widget_set_focused(&g_inp_msg);
    widget_invalidate_all(&g_root);

    /* Receive welcome messages (001, MOTD, etc.) */
    do_recv();
}

/* ── Widget callbacks ────────────────────────────────────────────────────── */

static void on_send_click(widget_t *btn)    { (void)btn; do_send(); }
static void on_send_submit(widget_t *inp)   { (void)inp; do_send(); }
static void on_refresh_click(widget_t *btn) { (void)btn; do_recv(); }

static void on_chan_select(widget_t *w, int idx, void *ud)
{
    (void)w; (void)idx;
    int ci = (int)(long)ud;
    if (ci >= 0 && ci < g_n_chans) switch_to_chan(ci);
}

static void on_user_select(widget_t *w, int idx, void *ud)
{
    (void)w; (void)idx;
    int ui = (int)(long)ud;
    irc_chan_t *chan = &g_chans[g_current_chan];
    if (ui < 0 || ui >= chan->n_users) return;
    /* Pre-fill /msg nick into the input for easy PM initiation */
    static char cmd[IRC_NICK_MAX + 8];
    const char *clean = strip_mode_prefix(chan->users[ui]);
    snprintf(cmd, sizeof(cmd), "/msg %s ", clean);
    textinput_set_text(&g_inp_msg, cmd);
    wdata_textinput_t *ti = &g_inp_msg.data.textinput;
    ti->cursor = (int)strlen(cmd);
    widget_set_focused(&g_inp_msg);
    widget_invalidate(&g_inp_msg);
}

/* Custom event_fn for g_inp_msg: intercepts F5 for manual refresh, F6 for test notif. */
static int inp_msg_event(widget_t *w, const widget_event_t *ev)
{
    if (ev->type == WEV_KEY_DOWN && ev->keycode == KEY_F5) {
        do_recv();
        return 1;
    }
    if (ev->type == WEV_KEY_DOWN && ev->keycode == KEY_F6) {
        post_notif("test", "F6 test notification");
        return 1;
    }
    return g_inp_msg_orig_fn ? g_inp_msg_orig_fn(w, ev) : 0;
}

/* ── Window chrome ───────────────────────────────────────────────────────── */

static void draw_chrome(void *ud)
{
    (void)ud;
    int ax = g_win_x + SIDE_PAD;
    int ay = g_win_y + TITLE_H + CONT_PAD;

    /* Title bar + rounded window frame */
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H, TITLE_H, "AetherIRC", 0);

    /* Toolbar glass background */
    gfx_fill((unsigned)ax, (unsigned)ay,
              (unsigned)CONT_W, (unsigned)TOOLBAR_H, C_IRC_TOOLBAR);
    gfx_hline((unsigned)ax, (unsigned)ay, (unsigned)CONT_W, C_GLASS_SPEC);
    gfx_fill((unsigned)ax, (unsigned)(ay + 1), (unsigned)CONT_W, 1u, C_GLASS_HIGH);
    gfx_hline((unsigned)ax, (unsigned)(ay + TOOLBAR_H - 1),
               (unsigned)CONT_W, C_ACCENT);

    /* Toolbar text labels (Server: / Port: / Nick:) */
    int ty = ay + (TOOLBAR_H - WGT_FONT_H) / 2;
    gfx_text_transparent((unsigned)ax, (unsigned)ty, "Server:", C_TEXT_DIM);
    gfx_text_transparent((unsigned)(ax + SERV_INP_X + SERV_INP_W + 7),
                          (unsigned)ty, "Port:", C_TEXT_DIM);
    gfx_text_transparent((unsigned)(ax + NICK_INP_X - 44),
                          (unsigned)ty, "Nick:", C_TEXT_DIM);

    /* Status row background */
    gfx_fill((unsigned)ax, (unsigned)(ay + STATUS_Y),
              (unsigned)CONT_W, (unsigned)STATUS_H, C_IRC_STATUS);

    /* Horizontal separators */
    gfx_hline((unsigned)ax, (unsigned)(ay + TOOLBAR_H),
               (unsigned)CONT_W, C_GLASS_SEP);                  /* below toolbar */
    gfx_hline((unsigned)ax, (unsigned)(ay + STATUS_Y - 1),
               (unsigned)CONT_W, C_SEP);                        /* above status  */
    gfx_hline((unsigned)ax, (unsigned)(ay + STATUS_Y + STATUS_H),
               (unsigned)CONT_W, C_SEP);                        /* below status  */
    gfx_hline((unsigned)ax, (unsigned)(ay + BODY_Y + BODY_H + 1),
               (unsigned)CONT_W, C_ACCENT);                     /* above input   */

    /* Vertical pane dividers */
    gfx_vline((unsigned)(ax + CHAN_W),
               (unsigned)(ay + BODY_Y), (unsigned)BODY_H, C_IRC_GLOW);
    gfx_vline((unsigned)(ax + CHAT_SB_X + CHAT_SB_W),
               (unsigned)(ay + BODY_Y), (unsigned)BODY_H, C_IRC_GLOW);

    /* Input bar background */
    gfx_fill((unsigned)ax, (unsigned)(ay + INPUT_Y),
              (unsigned)CONT_W, (unsigned)INPUT_H, C_IRC_INPUT);
    gfx_hline((unsigned)ax, (unsigned)(ay + INPUT_Y),
               (unsigned)CONT_W, C_GLASS_SEP);
}

/* ── Build widget tree ───────────────────────────────────────────────────── */

static void build_ui(void)
{
    widget_init_panel(&g_root, 0, 0, CONT_W, CONT_H, C_WIN_BG);

    /* ── Toolbar ───────────────────────────────────────────────────────── */
    widget_init_textinput(&g_inp_server, SERV_INP_X, 4, SERV_INP_W, 26,
                          NULL, on_connect_click);
    widget_init_textinput(&g_inp_port, PORT_INP_X, 4, PORT_INP_W, 26,
                          NULL, NULL);
    widget_init_textinput(&g_inp_nick, NICK_INP_X, 4, NICK_INP_W, 26,
                          NULL, on_connect_click);
    widget_init_button(&g_btn_connect, CONN_BTN_X, 4, CONN_BTN_W, 26,
                       "Connect", on_connect_click);

    textinput_set_text(&g_inp_server, "irc.libera.chat");
    textinput_set_text(&g_inp_port,   "6667");
    textinput_set_text(&g_inp_nick,   "aether_user");

    /* ── Status label ─────────────────────────────────────────────────── */
    widget_init_label(&g_lbl_status, 4, STATUS_Y, CONT_W - 8, STATUS_H,
                      "Enter server, nick and port, then click Connect.",
                      WGT_ALIGN_LEFT);

    /* ── Body panes ───────────────────────────────────────────────────── */
    widget_init_listview(&g_chan_list, 0, BODY_Y, CHAN_W, BODY_H,
                         IRC_MAX_CHANS + 1, on_chan_select);
    widget_init_textarea(&g_chat, CHAT_X, BODY_Y, CHAT_W, BODY_H, 512);
    textarea_set_word_wrap(&g_chat, 1);
    widget_init_scrollbar_v(&g_chat_sb, CHAT_SB_X, BODY_Y, CHAT_SB_W, BODY_H,
                            0, (BODY_H - 8) / WGT_FONT_H);
    widget_init_listview(&g_user_list, USER_X, BODY_Y, USER_W, BODY_H,
                         IRC_MAX_USERS + 4, on_user_select);

    /* ── Input bar ────────────────────────────────────────────────────── */
    widget_init_textinput(&g_inp_msg, 0, INPUT_Y, MSG_INP_W, INPUT_H,
                          NULL, on_send_submit);
    widget_init_button(&g_btn_refresh, REFRESH_BTN_X, INPUT_Y, REFRESH_BTN_W, INPUT_H,
                       "Refresh (F5)", on_refresh_click);
    widget_init_button(&g_btn_send, SEND_BTN_X, INPUT_Y, SEND_BTN_W, INPUT_H,
                       "Send", on_send_click);

    /* Intercept F5 in the message input */
    g_inp_msg_orig_fn  = g_inp_msg.event_fn;
    g_inp_msg.event_fn = inp_msg_event;

    /* Input bar hidden until connected */
    g_inp_msg.hidden    = 1;
    g_btn_refresh.hidden = 1;
    g_btn_send.hidden   = 1;

    /* Add all children to root (12 total, max = WIDGET_MAX_CHILDREN = 16) */
    widget_add_child(&g_root, &g_inp_server);
    widget_add_child(&g_root, &g_inp_port);
    widget_add_child(&g_root, &g_inp_nick);
    widget_add_child(&g_root, &g_btn_connect);
    widget_add_child(&g_root, &g_lbl_status);
    widget_add_child(&g_root, &g_chan_list);
    widget_add_child(&g_root, &g_chat);
    widget_add_child(&g_root, &g_chat_sb);
    widget_add_child(&g_root, &g_user_list);
    widget_add_child(&g_root, &g_inp_msg);
    widget_add_child(&g_root, &g_btn_refresh);
    widget_add_child(&g_root, &g_btn_send);

    widget_set_focused(&g_inp_server);

    /* Populate channel list with the server log entry */
    rebuild_chan_list();
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    /* Initialise IRC state */
    memset(&g_conn,  0, sizeof(g_conn));
    memset(g_chans,  0, sizeof(g_chans));
    g_conn.fd = -1;

    /* g_chans[0] = server log, always active */
    strncpy(g_chans[0].name, "server", IRC_CHAN_MAX - 1);
    g_chans[0].active = 1;
    g_n_chans     = 1;
    g_current_chan = 0;

    server_append("Welcome to AetherIRC!\n");
    server_append("Fill in Server / Port / Nick above, then click Connect.\n");
    server_append("Slash commands: /join #chan  /part  /nick  /msg nick  /quit\n");
    server_append("Press Enter on empty input or the Refresh button to receive new messages.\n\n");

    /* Register window; widget_run allocates the GPU BO automatically */
    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "AetherIRC");

    build_ui();

    widget_ctx_t ctx;
    ctx.win_x         = &g_win_x;
    ctx.win_y         = &g_win_y;
    ctx.content_dx    = SIDE_PAD;
    ctx.content_dy    = TITLE_H + CONT_PAD;
    ctx.win_id        = (int)g_win_id;
    ctx.win_w         = WIN_W;
    ctx.win_h         = WIN_H;
    ctx.on_reposition  = draw_chrome;
    ctx.per_frame_fn   = per_frame_poll;
    ctx.userdata       = NULL;
    ctx.minimized_flag = &g_minimized;
    ctx.running        = 1;

    widget_run(&g_root, &ctx);

    /* Cleanup */
    if (g_conn.fd >= 0) {
        irc_quit(&g_conn, "AetherIRC — farewell!");
        irc_disconnect(&g_conn);
    }
    sys_wm_request_close(g_win_id);
    return 0;
}
