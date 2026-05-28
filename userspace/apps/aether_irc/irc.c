/*
 * AetherIRC — IRC protocol implementation
 * File: userspace/apps/aether_irc/irc.c
 *
 * Implements RFC 1459 / RFC 2812 client-side wire protocol:
 *   - TCP connect + NICK/USER handshake
 *   - IRC message parsing  (:prefix CMD params :trailing)
 *   - Command senders (JOIN, PART, PRIVMSG, NICK, QUIT)
 *   - Line-framed receive with PING→PONG auto-reply
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys.h>

#include "irc.h"

/* ── Dotted-decimal IPv4 parser (same pattern as AetherTelnet) ───────────── */

static unsigned int parse_dotted(const char *s)
{
    int dots = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '.') { dots++; }
        else if (*p < '0' || *p > '9') return 0;
    }
    if (dots != 3) return 0;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    while (*s >= '0' && *s <= '9') a = a * 10 + (unsigned)(*s++ - '0');
    if (*s == '.') s++;
    while (*s >= '0' && *s <= '9') b = b * 10 + (unsigned)(*s++ - '0');
    if (*s == '.') s++;
    while (*s >= '0' && *s <= '9') c = c * 10 + (unsigned)(*s++ - '0');
    if (*s == '.') s++;
    while (*s >= '0' && *s <= '9') d = d * 10 + (unsigned)(*s++ - '0');
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* ── irc_parse ───────────────────────────────────────────────────────────── */

int irc_parse(const char *line, irc_msg_t *out)
{
    if (!line || !line[0]) return 0;
    memset(out, 0, sizeof(*out));
    const char *p = line;

    /* Optional prefix: :nick!user@host or :server.name */
    if (*p == ':') {
        p++;
        const char *sp = strchr(p, ' ');
        if (!sp) {
            strncpy(out->prefix, p, IRC_NICK_MAX - 1);
            return 1;
        }
        /* Extract nick (part before '!') */
        const char *ex = (const char *)memchr(p, '!', (size_t)(sp - p));
        int len = ex ? (int)(ex - p) : (int)(sp - p);
        if (len >= IRC_NICK_MAX) len = IRC_NICK_MAX - 1;
        memcpy(out->prefix, p, (size_t)len);
        p = sp + 1;
    }
    while (*p == ' ') p++;

    /* Command */
    {
        const char *sp = strchr(p, ' ');
        if (!sp) {
            strncpy(out->command, p, 31);
            return 1;
        }
        int len = (int)(sp - p);
        if (len > 31) len = 31;
        memcpy(out->command, p, (size_t)len);
        out->command[len] = 0;
        p = sp + 1;
    }
    while (*p == ' ') p++;

    /* Trailing starts with ':' right here */
    if (*p == ':') {
        strncpy(out->text, p + 1, IRC_TEXT_MAX - 1);
        int ti = (int)strlen(out->text);
        while (ti > 0 && (out->text[ti-1] == '\r' || out->text[ti-1] == '\n')) ti--;
        out->text[ti] = 0;
        out->has_text = 1;
        return 1;
    }

    /* Params + optional trailing */
    {
        /* Find the ' :' sequence that begins the trailing */
        const char *trail_start = NULL;
        const char *t = p;
        while (*t) {
            if (*t == ' ' && *(t + 1) == ':') { trail_start = t + 2; break; }
            t++;
        }

        int plen;
        if (trail_start) {
            plen = (int)(trail_start - 2 - p);
        } else {
            plen = (int)strlen(p);
        }
        if (plen > IRC_TEXT_MAX - 1) plen = IRC_TEXT_MAX - 1;
        memcpy(out->params, p, (size_t)plen);
        out->params[plen] = 0;
        /* Trim trailing whitespace from params */
        int pi = (int)strlen(out->params);
        while (pi > 0 && (out->params[pi-1] == ' ' || out->params[pi-1] == '\r'
                         || out->params[pi-1] == '\n')) pi--;
        out->params[pi] = 0;

        if (trail_start) {
            strncpy(out->text, trail_start, IRC_TEXT_MAX - 1);
            int ti = (int)strlen(out->text);
            while (ti > 0 && (out->text[ti-1] == '\r' || out->text[ti-1] == '\n')) ti--;
            out->text[ti] = 0;
            out->has_text = 1;
        }
    }

    return 1;
}

/* ── irc_connect ─────────────────────────────────────────────────────────── */

int irc_connect(irc_conn_t *c, const char *host, unsigned short port,
                const char *nick, const char *username)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;

    unsigned int ip = parse_dotted(host);
    if (!ip) {
        ip = sys_net_dns(host);
        if (!ip) return -1;
    }

    long fd = sys_socket(SOCK_TCP);
    if (fd < 0) return -1;

    if (sys_connect(fd, ip, port) < 0) {
        sys_net_close(fd);
        return -1;
    }

    c->fd      = fd;
    c->rxpos   = 0;
    strncpy(c->nick, nick, IRC_NICK_MAX - 1);

    /* Handshake */
    {
        char buf[IRC_NICK_MAX + 8];
        snprintf(buf, sizeof(buf), "NICK %s", nick);
        if (irc_send_raw(c, buf) < 0) goto fail;
    }
    {
        char buf[IRC_NICK_MAX + 64];
        snprintf(buf, sizeof(buf), "USER %s 0 * :AetherOS IRC User", username);
        if (irc_send_raw(c, buf) < 0) goto fail;
    }
    return 0;

fail:
    sys_net_close(fd);
    c->fd = -1;
    return -1;
}

/* ── irc_disconnect ──────────────────────────────────────────────────────── */

void irc_disconnect(irc_conn_t *c)
{
    if (c->fd >= 0) {
        sys_net_close(c->fd);
        c->fd = -1;
    }
    c->logged_in = 0;
    c->rxpos     = 0;
}

/* ── irc_send_raw ────────────────────────────────────────────────────────── */

int irc_send_raw(irc_conn_t *c, const char *line)
{
    if (c->fd < 0 || !line) return -1;
    static char buf[514];
    int n = 0;
    while (line[n] && n < 510) { buf[n] = line[n]; n++; }
    buf[n++] = '\r';
    buf[n++] = '\n';
    return (int)sys_net_send(c->fd, buf, (long)n);
}

/* ── Command senders ─────────────────────────────────────────────────────── */

int irc_join(irc_conn_t *c, const char *chan)
{
    char buf[IRC_CHAN_MAX + 8];
    snprintf(buf, sizeof(buf), "JOIN %s", chan);
    return irc_send_raw(c, buf) > 0 ? 0 : -1;
}

int irc_part(irc_conn_t *c, const char *chan)
{
    char buf[IRC_CHAN_MAX + 8];
    snprintf(buf, sizeof(buf), "PART %s", chan);
    return irc_send_raw(c, buf) > 0 ? 0 : -1;
}

int irc_privmsg(irc_conn_t *c, const char *target, const char *text)
{
    char buf[IRC_CHAN_MAX + IRC_TEXT_MAX + 12];
    snprintf(buf, sizeof(buf), "PRIVMSG %s :%s", target, text);
    return irc_send_raw(c, buf) > 0 ? 0 : -1;
}

int irc_nick_cmd(irc_conn_t *c, const char *new_nick)
{
    char buf[IRC_NICK_MAX + 8];
    snprintf(buf, sizeof(buf), "NICK %s", new_nick);
    return irc_send_raw(c, buf) > 0 ? 0 : -1;
}

int irc_quit(irc_conn_t *c, const char *msg)
{
    char buf[IRC_TEXT_MAX + 8];
    snprintf(buf, sizeof(buf), "QUIT :%s", msg ? msg : "Leaving");
    irc_send_raw(c, buf);
    return 0;
}

/* ── irc_recv / irc_recv_nb ──────────────────────────────────────────────── */

static char g_rxraw[2048];

static int irc_recv_impl(irc_conn_t *c,
                         void (*on_line)(const char *line, void *ud), void *ud,
                         int nonblocking)
{
    if (c->fd < 0) return -1;

    long n = nonblocking
           ? sys_net_recv_nb(c->fd, g_rxraw, (long)sizeof(g_rxraw) - 1)
           : sys_net_recv   (c->fd, g_rxraw, (long)sizeof(g_rxraw) - 1);
    if (n < 0) return -1;
    if (n == 0) return 0;
    g_rxraw[n] = '\0';

    int count = 0;
    for (long i = 0; i < n; i++) {
        char ch = g_rxraw[i];
        if (c->rxpos < (int)sizeof(c->rxbuf) - 1)
            c->rxbuf[c->rxpos++] = ch;

        if (ch == '\n') {
            int end = c->rxpos;
            if (end > 0 && c->rxbuf[end - 1] == '\n') end--;
            if (end > 0 && c->rxbuf[end - 1] == '\r') end--;
            c->rxbuf[end] = '\0';

            if (end > 0) {
                if (strncmp(c->rxbuf, "PING ", 5) == 0) {
                    char pong[128];
                    snprintf(pong, sizeof(pong), "PONG %s", c->rxbuf + 5);
                    irc_send_raw(c, pong);
                }
                if (on_line) on_line(c->rxbuf, ud);
                count++;
            }
            c->rxpos = 0;
        }
    }
    return count;
}

int irc_recv(irc_conn_t *c, void (*on_line)(const char *line, void *ud), void *ud)
{
    return irc_recv_impl(c, on_line, ud, 0);
}

int irc_recv_nb(irc_conn_t *c, void (*on_line)(const char *line, void *ud), void *ud)
{
    return irc_recv_impl(c, on_line, ud, 1);
}
