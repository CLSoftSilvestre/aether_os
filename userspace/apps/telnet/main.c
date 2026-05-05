/*
 * AetherOS — AetherTelnet
 * File: userspace/apps/telnet/main.c
 *
 * RFC-854 Telnet client with a Lumina GUI (libwidget).
 * Inspired by PuTTY.
 *
 * Window: 640×480  Content area: 624×440
 * Layout (y positions within content area):
 *   [  0.. 33]  Connection bar: Host, Port, Connect/Disconnect
 *   [ 36.. 55]  Status label
 *   [ 57.. 57]  Separator (accent line)
 *   [ 60..391]  Terminal textarea (332 px, ~33 visible lines)
 *   [393..393]  Separator (accent line)
 *   [396..423]  Input bar: command input + Send button
 */

#include <gfx.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Window geometry ─────────────────────────────────────────────── */

#define WIN_W      640
#define WIN_H      480
#define TITLE_H     28
#define SIDE_PAD     8
#define CONT_PAD     6

#define WIN_X_INIT  180
#define WIN_Y_INIT   80

#define CONT_W  (WIN_W - 2 * SIDE_PAD)                    /* 624 */
#define CONT_H  (WIN_H - TITLE_H - 2 * CONT_PAD)          /* 440 */

/* ── Connection state ────────────────────────────────────────────── */

#define TELNET_DISCONNECTED  0
#define TELNET_CONNECTED     1

static int  g_state = TELNET_DISCONNECTED;
static long g_fd    = -1;

/* ── Window position ─────────────────────────────────────────────── */

static int  g_win_x  = WIN_X_INIT;
static int  g_win_y  = WIN_Y_INIT;
static long g_win_id = -1;

/* ── Terminal output buffer ──────────────────────────────────────── */

#define TERM_BUF_SIZE  (220 * 128)   /* ~220 lines × 128 chars */

static char g_term_buf[TERM_BUF_SIZE];

/* ── Widget tree (global statics) ────────────────────────────────── */

static widget_t g_root;

static widget_t g_lbl_host;      /* "Host:" label               */
static widget_t g_inp_host;      /* hostname / IP input         */
static widget_t g_lbl_port;      /* "Port:" label               */
static widget_t g_inp_port;      /* port number input           */
static widget_t g_btn_connect;   /* Connect / Disconnect button */

static widget_t g_lbl_status;    /* status line below bar       */

static widget_t g_sep1;          /* thin separator              */
static widget_t g_term;          /* terminal textarea           */
static widget_t g_sep2;          /* thin separator              */

static widget_t g_inp_cmd;       /* command / text input        */
static widget_t g_btn_send;      /* Send button                 */

/* ── Forward declarations ────────────────────────────────────────── */

static void term_append(const char *text);
static void telnet_disconnect(void);

/* ── Telnet IAC / protocol filter ────────────────────────────────── */

#define IAC      0xFFu
#define IAC_WILL 0xFBu
#define IAC_WONT 0xFCu
#define IAC_DO   0xFDu
#define IAC_DONT 0xFEu
#define IAC_SB   0xFAu
#define IAC_SE   0xF0u

/*
 * Strip Telnet IAC sequences from raw received data and reply to
 * WILL with DON'T and DO with WON'T (refuse all option negotiation).
 * Returns the number of printable bytes written to clean[].
 */
static long filter_iac(const unsigned char *raw, long len,
                       char *clean, long cap)
{
    long ci = 0;
    long i  = 0;

    while (i < len && ci < cap - 1) {
        unsigned char b = raw[i];

        if (b != IAC) {
            /* RFC 854: CR NUL → CR in NVT data stream */
            if (b == '\r' && i + 1 < len && raw[i + 1] == '\0') {
                i += 2;
                continue;
            }
            clean[ci++] = (char)b;
            i++;
            continue;
        }

        i++;  /* skip IAC byte */
        if (i >= len) break;

        unsigned char cmd = raw[i++];

        if (cmd == IAC_SB) {
            /* Subnegotiation block — skip until IAC SE */
            while (i < len - 1) {
                if (raw[i] == IAC && raw[i + 1] == IAC_SE) { i += 2; break; }
                i++;
            }
        } else if (cmd == IAC_WILL || cmd == IAC_DO) {
            if (i < len) {
                unsigned char opt = raw[i++];
                unsigned char rep[3];
                rep[0] = IAC;
                rep[1] = (cmd == IAC_WILL) ? IAC_DONT : IAC_WONT;
                rep[2] = opt;
                sys_net_send(g_fd, rep, 3);
            }
        } else if (cmd == IAC_WONT || cmd == IAC_DONT) {
            if (i < len) i++;  /* consume option byte */
        }
        /* GA, NOP, DM, BRK, IP, AO, AYT, EC, EL, SE — already consumed */
    }

    clean[ci] = '\0';
    return ci;
}

/* ── Terminal output buffer ──────────────────────────────────────── */

static void term_append(const char *text)
{
    int blen = (int)strlen(g_term_buf);
    int tlen = (int)strlen(text);

    if (blen + tlen >= TERM_BUF_SIZE - 1) {
        /* Scroll: discard the first third, align to a newline boundary */
        int discard = TERM_BUF_SIZE / 3;
        const char *nl = g_term_buf + discard;
        while (*nl && *nl != '\n') nl++;
        if (*nl == '\n') nl++;
        int off = (int)(nl - g_term_buf);
        memmove(g_term_buf, nl, (unsigned)(blen - off + 1));
        blen -= off;
    }

    if (tlen > 0 && blen + tlen < TERM_BUF_SIZE - 1)
        memcpy(g_term_buf + blen, text, (unsigned)(tlen + 1));

    textarea_set_text(&g_term, g_term_buf);
}

/* ── Network receive (one blocking call) ─────────────────────────── */

#define RECV_CAP  4096

static unsigned char g_rxraw[RECV_CAP];
static char          g_rxclean[RECV_CAP];

static void telnet_recv(void)
{
    if (g_fd < 0) return;

    long n = sys_net_recv(g_fd, g_rxraw, RECV_CAP - 1);
    if (n <= 0) return;

    long cn = filter_iac(g_rxraw, n, g_rxclean, RECV_CAP);
    if (cn > 0)
        term_append(g_rxclean);
}

/* ── Disconnect ──────────────────────────────────────────────────── */

static void telnet_disconnect(void)
{
    if (g_fd >= 0) {
        sys_net_close(g_fd);
        g_fd = -1;
    }
    g_state = TELNET_DISCONNECTED;
    label_set_text(&g_lbl_status, "Disconnected.");
    strncpy(g_btn_connect.data.button.text, "Connect", 127);
    widget_invalidate(&g_btn_connect);
    g_inp_cmd.hidden  = 1;
    g_btn_send.hidden = 1;
    widget_invalidate_all(&g_root);
    widget_set_focused(&g_inp_host);
}

/* ── IPv4 dotted-decimal parser ──────────────────────────────────── */

static unsigned int parse_dotted(const char *s)
{
    int dots = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '.') { dots++; }
        else if (*p < '0' || *p > '9') return 0;
    }
    if (dots != 3) return 0;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    while (*s >= '0' && *s <= '9') a = a * 10 + (*s++ - '0');
    if (*s == '.') s++;
    while (*s >= '0' && *s <= '9') b = b * 10 + (*s++ - '0');
    if (*s == '.') s++;
    while (*s >= '0' && *s <= '9') c = c * 10 + (*s++ - '0');
    if (*s == '.') s++;
    while (*s >= '0' && *s <= '9') d = d * 10 + (*s++ - '0');
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* ── Connect / Disconnect callback ──────────────────────────────── */

static void on_connect_click(widget_t *btn)
{
    (void)btn;

    if (g_state == TELNET_CONNECTED) {
        term_append("\n=== Disconnected ===\n");
        telnet_disconnect();
        return;
    }

    const char *host    = textinput_get_text(&g_inp_host);
    const char *portstr = textinput_get_text(&g_inp_port);

    if (!host || !host[0]) {
        label_set_text(&g_lbl_status, "Enter a hostname or IP address.");
        return;
    }

    unsigned short port = 23;
    if (portstr && portstr[0]) {
        int p = atoi(portstr);
        if (p > 0 && p < 65536) port = (unsigned short)p;
    }

    /* Resolve host to IP */
    unsigned int ip = parse_dotted(host);
    if (!ip) {
        static char res_msg[80];
        snprintf(res_msg, sizeof(res_msg), "Resolving %s ...", host);
        label_set_text(&g_lbl_status, res_msg);
        widget_invalidate(&g_lbl_status);
        ip = sys_net_dns(host);
        if (!ip) {
            label_set_text(&g_lbl_status, "DNS lookup failed — check hostname.");
            return;
        }
    }

    /* Create TCP socket */
    long fd = sys_socket(SOCK_TCP);
    if (fd < 0) {
        label_set_text(&g_lbl_status, "Error: could not create socket.");
        return;
    }

    /* Connect */
    static char conn_msg[80];
    snprintf(conn_msg, sizeof(conn_msg),
             "Connecting to %s port %u ...", host, (unsigned)port);
    label_set_text(&g_lbl_status, conn_msg);
    widget_invalidate(&g_lbl_status);

    if (sys_connect(fd, ip, port) < 0) {
        sys_net_close(fd);
        label_set_text(&g_lbl_status, "Connection refused or timed out.");
        return;
    }

    g_fd    = fd;
    g_state = TELNET_CONNECTED;

    /* Update button label to Disconnect */
    strncpy(g_btn_connect.data.button.text, "Disconnect", 127);
    widget_invalidate(&g_btn_connect);

    /* Show input bar */
    g_inp_cmd.hidden  = 0;
    g_btn_send.hidden = 0;
    widget_invalidate_all(&g_root);

    /* Status */
    static char ok_msg[80];
    snprintf(ok_msg, sizeof(ok_msg),
             "Connected to %s:%u", host, (unsigned)port);
    label_set_text(&g_lbl_status, ok_msg);

    /* Welcome line in terminal */
    static char welcome[128];
    snprintf(welcome, sizeof(welcome),
             "=== AetherTelnet: %s port %u ===\n", host, (unsigned)port);
    term_append(welcome);

    /* Initial receive: server banner + IAC option negotiation */
    telnet_recv();

    widget_set_focused(&g_inp_cmd);
}

/* ── Send / receive ──────────────────────────────────────────────── */

static void do_send(void)
{
    if (g_fd < 0 || g_state != TELNET_CONNECTED) {
        label_set_text(&g_lbl_status, "Not connected.");
        return;
    }

    const char *text = textinput_get_text(&g_inp_cmd);
    if (!text) return;

    /* Build Telnet line: text + CR LF */
    static char sbuf[300];
    int slen = 0;
    for (int i = 0; text[i] && slen < 297; i++)
        sbuf[slen++] = text[i];
    sbuf[slen++] = '\r';
    sbuf[slen++] = '\n';
    sbuf[slen]   = '\0';

    /* Echo typed line to terminal */
    static char echo[280];
    snprintf(echo, sizeof(echo), "> %s\n", text);
    term_append(echo);

    textinput_clear(&g_inp_cmd);

    if (sys_net_send(g_fd, sbuf, (long)slen) < 0) {
        term_append("[connection lost on send]\n");
        telnet_disconnect();
        return;
    }

    telnet_recv();
}

static void on_send_click(widget_t *btn)  { (void)btn; do_send(); }
static void on_cmd_submit(widget_t *inp)  { (void)inp; do_send(); }

/* ── Window chrome ───────────────────────────────────────────────── */

static void draw_frame(void)
{
    int wx = g_win_x, wy = g_win_y;

    gfx_fill(wx + 4, wy + 4, WIN_W, WIN_H, GFX_RGB(4, 4, 8));  /* shadow */
    gfx_fill(wx, wy, WIN_W, WIN_H, C_WIN_BG);
    gfx_fill(wx, wy, WIN_W, TITLE_H, C_TITLEBAR);
    gfx_draw_close_button(wx + 10, wy + 8, 0);
    gfx_text_center(wx, WIN_W, wy + 10, "AetherTelnet", C_TEXT, C_TITLEBAR);
    gfx_hline(wx, wy + TITLE_H, WIN_W, C_ACCENT);
    gfx_rect(wx, wy, WIN_W, WIN_H, C_SEP);
}

static void on_reposition(void *ud) { (void)ud; draw_frame(); }

/* ── Build widget tree ───────────────────────────────────────────── */

static void build_ui(void)
{
    widget_init_panel(&g_root, 0, 0, CONT_W, CONT_H, C_WIN_BG);

    /* ── Row 1: connection controls (y = 0..33) ── */
    widget_init_label(&g_lbl_host, 0, 9, 36, 18, "Host:", WGT_ALIGN_LEFT);
    widget_init_textinput(&g_inp_host, 38, 4, 256, 26, NULL, on_connect_click);
    widget_init_label(&g_lbl_port, 302, 9, 30, 18, "Port:", WGT_ALIGN_LEFT);
    widget_init_textinput(&g_inp_port, 334, 4, 72, 26, NULL, NULL);
    widget_init_button(&g_btn_connect, 414, 4, 100, 26, "Connect", on_connect_click);
    textinput_set_text(&g_inp_port, "23");

    /* ── Row 2: status label (y = 36..55) ── */
    widget_init_label(&g_lbl_status, 0, 36, CONT_W, 18,
                      "Enter host and port, then click Connect.",
                      WGT_ALIGN_LEFT);

    /* ── Separator line (y = 57) ── */
    widget_init_panel(&g_sep1, 0, 57, CONT_W, 1, C_SEP);

    /* ── Terminal output area (y = 60..391) ── */
    widget_init_textarea(&g_term, 0, 60, CONT_W, 332, 256);

    /* ── Separator line (y = 393) ── */
    widget_init_panel(&g_sep2, 0, 393, CONT_W, 1, C_ACCENT);

    /* ── Input bar (y = 396..423) ── */
    widget_init_textinput(&g_inp_cmd, 0, 396, 506, 28, NULL, on_cmd_submit);
    widget_init_button(&g_btn_send, 510, 396, 114, 28, "Send", on_send_click);

    /* Input bar hidden until connected */
    g_inp_cmd.hidden  = 1;
    g_btn_send.hidden = 1;

    /* Build tree */
    widget_add_child(&g_root, &g_lbl_host);
    widget_add_child(&g_root, &g_inp_host);
    widget_add_child(&g_root, &g_lbl_port);
    widget_add_child(&g_root, &g_inp_port);
    widget_add_child(&g_root, &g_btn_connect);
    widget_add_child(&g_root, &g_lbl_status);
    widget_add_child(&g_root, &g_sep1);
    widget_add_child(&g_root, &g_term);
    widget_add_child(&g_root, &g_sep2);
    widget_add_child(&g_root, &g_inp_cmd);
    widget_add_child(&g_root, &g_btn_send);

    widget_set_focused(&g_inp_host);
}

/* ── Entry point ─────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "AetherTelnet");

    draw_frame();
    build_ui();

    widget_ctx_t ctx;
    ctx.win_x         = &g_win_x;
    ctx.win_y         = &g_win_y;
    ctx.content_dx    = SIDE_PAD;
    ctx.content_dy    = TITLE_H + CONT_PAD;
    ctx.on_reposition = on_reposition;
    ctx.userdata      = NULL;
    ctx.running       = 1;

    widget_run(&g_root, &ctx);

    if (g_fd >= 0) sys_net_close(g_fd);
    sys_wm_unregister(g_win_id);
    return 0;
}
