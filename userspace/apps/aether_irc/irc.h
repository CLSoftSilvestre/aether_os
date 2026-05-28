/*
 * AetherIRC — IRC protocol layer
 * File: userspace/apps/aether_irc/irc.h
 *
 * RFC 1459 / RFC 2812 IRC client protocol.
 * Handles TCP transport, line framing, message parsing, and command senders.
 */

#ifndef AETHER_IRC_H
#define AETHER_IRC_H

/* ── Limits ──────────────────────────────────────────────────────────────── */
#define IRC_NICK_MAX    32      /* max nick length + NUL                    */
#define IRC_CHAN_MAX    64      /* max channel name length + NUL             */
#define IRC_TEXT_MAX   512      /* max trailing / text field length + NUL   */
#define IRC_MAX_CHANS    8      /* server log + up to 7 joined channels      */
#define IRC_MAX_USERS   64      /* max users shown in names list per channel */
#define IRC_CHAN_BUF   (160 * 128)  /* per-channel message history buffer   */

/* ── Parsed IRC message ──────────────────────────────────────────────────── */
typedef struct {
    char prefix[IRC_NICK_MAX];  /* nick from :nick!user@host, or server name */
    char command[32];           /* "PRIVMSG", "JOIN", "353", "PING", …       */
    char params[IRC_TEXT_MAX];  /* space-separated params before trailing     */
    char text[IRC_TEXT_MAX];    /* trailing text (after ':')                 */
    int  has_text;              /* 1 if trailing was present                 */
} irc_msg_t;

/* ── Per-channel state (owned by main.c) ─────────────────────────────────── */
typedef struct {
    char name[IRC_CHAN_MAX];                  /* "#channel", "nick" (PM), or "server" */
    char buf[IRC_CHAN_BUF];                  /* message history (flat string, NUL-terminated) */
    char users[IRC_MAX_USERS][IRC_NICK_MAX]; /* nick list (with mode prefix: @, +, %) */
    int  n_users;
    int  active;   /* 1 = joined / open                */
    int  unread;   /* 1 = has messages since last view */
} irc_chan_t;

/* ── Connection state ────────────────────────────────────────────────────── */
typedef struct {
    long fd;                /* TCP socket fd; -1 = disconnected */
    int  logged_in;         /* 1 after 001 RPL_WELCOME          */
    char nick[IRC_NICK_MAX];
    char rxbuf[4096];       /* partial-line accumulation        */
    int  rxpos;             /* bytes used in rxbuf              */
} irc_conn_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* Parse one IRC line (without trailing \r\n) into *out.
 * Returns 1 on success, 0 if line is empty or unparseable. */
int  irc_parse(const char *line, irc_msg_t *out);

/* Open TCP connection to host:port, send NICK + USER.
 * nick      : desired nick (max IRC_NICK_MAX-1 chars)
 * username  : IRC username field (safe to pass "aether")
 * Returns 0 on success, -1 on failure (DNS, connect, send errors). */
int  irc_connect(irc_conn_t *c, const char *host, unsigned short port,
                 const char *nick, const char *username);

/* Close the socket and reset state. Safe to call when already disconnected. */
void irc_disconnect(irc_conn_t *c);

/* Send a raw IRC command line (appends \r\n automatically).
 * Returns bytes sent, or -1 on error. */
int  irc_send_raw(irc_conn_t *c, const char *line);

/* Higher-level command senders — all return 0 on success, -1 on error. */
int  irc_join    (irc_conn_t *c, const char *chan);
int  irc_part    (irc_conn_t *c, const char *chan);
int  irc_privmsg (irc_conn_t *c, const char *target, const char *text);
int  irc_nick_cmd(irc_conn_t *c, const char *new_nick);
int  irc_quit    (irc_conn_t *c, const char *msg);

/* Receive pending data from the socket and process complete IRC lines.
 * Calls on_line(line, ud) for each complete line received.
 * PING → PONG is handled transparently before on_line is invoked.
 * Returns the number of lines dispatched, -1 on socket error.
 *
 * NOTE: sys_net_recv has a 5-second blocking timeout in the AetherOS kernel.
 * Call this only from user-triggered actions (send, join, explicit refresh)
 * to avoid UI freezes during idle periods. */
int  irc_recv(irc_conn_t *c, void (*on_line)(const char *line, void *ud), void *ud);

/* Non-blocking variant: uses sys_net_recv_nb (0-ms timeout).
 * Returns immediately — 0 if no data was available, otherwise the line count.
 * Safe to call from a per-frame timer without freezing the UI. */
int  irc_recv_nb(irc_conn_t *c, void (*on_line)(const char *line, void *ud), void *ud);

#endif /* AETHER_IRC_H */
