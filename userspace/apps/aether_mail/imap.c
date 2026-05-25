/*
 * AetherOS — AetherMail IMAP4rev1 client
 * File: userspace/apps/aether_mail/imap.c
 *
 * Minimal IMAP4rev1 implementation sufficient for daily-use inbox management.
 * All I/O is synchronous (blocking).  TLS is wrapped via tls_aether when
 * HAVE_MBEDTLS is defined at compile time.
 */

#include "imap.h"
#include <sys.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

#ifdef HAVE_MBEDTLS
#include "tls_aether.h"
#endif

/* Case-insensitive prefix compare — replaces POSIX strncasecmp */
static int imap_ncasecmp(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        int ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
    return 0;
}

/* ── Raw I/O wrappers ────────────────────────────────────────────────────── */

static long raw_send(imap_conn_t *c, const void *buf, long len)
{
#ifdef HAVE_MBEDTLS
    if (c->tls)
        return (long)tls_write((tls_conn_t *)c->tls, buf, (size_t)len);
#endif
    return sys_net_send(c->fd, buf, len);
}

static long raw_recv(imap_conn_t *c, void *buf, long len)
{
#ifdef HAVE_MBEDTLS
    if (c->tls)
        return (long)tls_read((tls_conn_t *)c->tls, buf, (size_t)len);
#endif
    return sys_net_recv(c->fd, buf, len);
}

/* ── Receive ring buffer ─────────────────────────────────────────────────── */

static int rbuf_getc(imap_conn_t *c)
{
    if (c->rlen == 0) {
        long n = raw_recv(c, c->rbuf, IMAP_RECV_CAP);
        if (n <= 0) return -1;
        c->rstart = 0;
        c->rlen   = (int)n;
    }
    int ch = (unsigned char)c->rbuf[c->rstart++];
    c->rlen--;
    return ch;
}

/* Read exactly n bytes; returns n on success, -1 on partial read. */
static int rbuf_readn(imap_conn_t *c, char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        int ch = rbuf_getc(c);
        if (ch < 0) return -1;
        buf[i] = (char)ch;
    }
    return n;
}

/* Discard exactly n bytes from the receive stream. */
static void rbuf_skip(imap_conn_t *c, int n)
{
    for (int i = 0; i < n; i++)
        rbuf_getc(c);
}

/* Read one CRLF-terminated line, stripping \r\n.  Returns length or -1. */
static int imap_readline(imap_conn_t *c, char *line, int maxlen)
{
    int n = 0;
    for (;;) {
        int ch = rbuf_getc(c);
        if (ch < 0) return -1;
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (n < maxlen - 1) line[n++] = (char)ch;
    }
    line[n] = '\0';
    return n;
}

/* ── Protocol helpers ────────────────────────────────────────────────────── */

/* If the line ends with {N}, return N; else return 0. */
static int literal_size(const char *line)
{
    int len = (int)strlen(line);
    if (len < 3 || line[len - 1] != '}') return 0;
    int i;
    for (i = len - 2; i >= 0; i--) {
        if (line[i] == '{') return atoi(line + i + 1);
    }
    return 0;
}

/* Send a string over the connection. */
static int imap_send(imap_conn_t *c, const char *data)
{
    long len = (long)strlen(data);
    return (raw_send(c, data, len) == len) ? 0 : -1;
}

/* Build and send a tagged command, store the tag in tag_out[8]. */
static int imap_cmd(imap_conn_t *c, char *tag_out, const char *fmt, ...)
{
    char tag[8];
    snprintf(tag, sizeof(tag), "A%03d", ++c->tag_seq);
    if (tag_out) { tag_out[0] = '\0'; strncat(tag_out, tag, 7); }

    char body[IMAP_LINE_MAX];
    va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    __builtin_va_end(ap);

    char line[IMAP_LINE_MAX + 12];
    snprintf(line, sizeof(line), "%s %s\r\n", tag, body);
    return imap_send(c, line);
}

/*
 * Read responses until <tag> OK / NO / BAD.
 * For each untagged line, call line_cb(line, litbuf, litlen, ud) if non-NULL.
 * Returns 1 = OK, 0 = NO/BAD, -1 = I/O error.
 */
typedef void (*imap_line_cb)(const char *line,
                              const char *lit, int litlen, void *ud);

static int imap_await(imap_conn_t *c, const char *tag,
                      imap_line_cb cb, void *ud)
{
    static char line[IMAP_LINE_MAX];
    static char litbuf[IMAP_LIT_CAP];
    int  taglen = (int)strlen(tag);

    for (;;) {
        if (imap_readline(c, line, sizeof(line)) < 0) return -1;

        /* Check for IMAP literal at end of this line */
        int litsize = literal_size(line);
        int litread = 0;
        if (litsize > 0) {
            litread = litsize < IMAP_LIT_CAP - 1 ? litsize : IMAP_LIT_CAP - 1;
            if (rbuf_readn(c, litbuf, litread) < 0) return -1;
            litbuf[litread] = '\0';
            if (litsize > litread) rbuf_skip(c, litsize - litread);
        }

        /* Invoke callback for every untagged line */
        if (cb) cb(line, litbuf, litread, ud);

        /* Tagged response? */
        if (strncmp(line, tag, (unsigned)taglen) == 0 && line[taglen] == ' ') {
            const char *resp = line + taglen + 1;
            if (strncmp(resp, "OK", 2) == 0) return 1;
            return 0;   /* NO or BAD */
        }
    }
}

/* ── Header parsing ──────────────────────────────────────────────────────── */

static void parse_header_fields(const char *lit, int litlen, mail_msg_t *msg)
{
    const char *p   = lit;
    const char *end = lit + litlen;

    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (unsigned)(end - p));
        int llen = nl ? (int)(nl - p) : (int)(end - p);

        /* Handle folded headers (lines starting with whitespace) */
        if (llen > 0 && (p[0] == ' ' || p[0] == '\t')) {
            p = nl ? nl + 1 : end;
            continue;
        }

        char lbuf[IMAP_LINE_MAX];
        int  n = llen < (int)sizeof(lbuf) - 1 ? llen : (int)sizeof(lbuf) - 1;
        memcpy(lbuf, p, (unsigned)n);
        if (n > 0 && lbuf[n - 1] == '\r') n--;
        lbuf[n] = '\0';

        const char *val;

#define FIELD(prefix, field, maxlen) \
        if (imap_ncasecmp(lbuf, prefix, (int)(sizeof(prefix)-1)) == 0) { \
            val = lbuf + sizeof(prefix) - 1; \
            while (*val == ' ') val++; \
            strncpy(field, val, (unsigned)(maxlen - 1)); \
            field[maxlen - 1] = '\0'; \
        }

        FIELD("From:",    msg->from,    MAIL_FROM_MAX)
        else FIELD("Subject:", msg->subject, MAIL_SUBJECT_MAX)
        else FIELD("Date:",    msg->date,    MAIL_DATE_MAX)

#undef FIELD

        p = nl ? nl + 1 : end;
    }
}

/* ── Callback state structs ──────────────────────────────────────────────── */

typedef struct {
    mail_folder_t *folders;
    int            max;
    int            count;
} list_cb_state_t;

typedef struct {
    int *seqnums;
    int  max;
    int  count;
} search_cb_state_t;

typedef struct {
    mail_msg_t *msgs;
    int         max;
    int         count;
    int         cur_seq;
    int         cur_seen;
} fetch_hdr_state_t;

typedef struct {
    char *body;
    int   body_max;
    int   body_len;
} fetch_body_state_t;

typedef struct {
    int *exists_out;
    int *unseen_out;
} select_cb_state_t;

/* ── Per-command callbacks ───────────────────────────────────────────────── */

static void cb_list(const char *line, const char *lit, int litlen, void *ud)
{
    (void)lit; (void)litlen;
    list_cb_state_t *s = (list_cb_state_t *)ud;
    /* * LIST (\flags) "/" "INBOX" */
    if (strncmp(line, "* LIST ", 7) != 0) return;
    if (s->count >= s->max) return;

    /* Find mailbox name: last token (after closing ')' and delimiter) */
    const char *p = strrchr(line, '"');
    if (p) {
        /* Quoted name */
        p++;
        const char *end = strchr(p, '"');
        int n = end ? (int)(end - p) : (int)strlen(p);
        if (n >= MAIL_FOLDER_NAME) n = MAIL_FOLDER_NAME - 1;
        strncpy(s->folders[s->count].name, p, (unsigned)n);
        s->folders[s->count].name[n] = '\0';
    } else {
        /* Unquoted name (e.g. INBOX) */
        const char *sp = strrchr(line, ' ');
        if (!sp) return;
        strncpy(s->folders[s->count].name, sp + 1, MAIL_FOLDER_NAME - 1);
    }
    s->count++;
}

static void cb_search(const char *line, const char *lit, int litlen, void *ud)
{
    (void)lit; (void)litlen;
    search_cb_state_t *s = (search_cb_state_t *)ud;
    if (strncmp(line, "* SEARCH", 8) != 0) return;
    const char *p = line + 8;
    while (*p) {
        while (*p == ' ') p++;
        if (*p < '0' || *p > '9') break;
        if (s->count < s->max)
            s->seqnums[s->count++] = atoi(p);
        while (*p >= '0' && *p <= '9') p++;
    }
}

static void cb_fetch_hdr(const char *line, const char *lit, int litlen, void *ud)
{
    fetch_hdr_state_t *s = (fetch_hdr_state_t *)ud;

    /* * N FETCH (FLAGS (...) BODY[...] {size} */
    if (strncmp(line, "* ", 2) == 0 && strstr(line, " FETCH ")) {
        int seq = 0;
        seq = atoi(line + 2);
        s->cur_seq  = seq;
        s->cur_seen = (strstr(line, "\\Seen") != NULL) ? 1 : 0;

        if (litlen > 0 && s->count < s->max) {
            mail_msg_t *msg = &s->msgs[s->count];
            memset(msg, 0, sizeof(*msg));
            msg->seq_num = s->cur_seq;
            msg->seen    = (unsigned char)s->cur_seen;
            parse_header_fields(lit, litlen, msg);
            s->count++;
        }
    }
}

static void cb_fetch_body(const char *line, const char *lit, int litlen, void *ud)
{
    fetch_body_state_t *s = (fetch_body_state_t *)ud;
    if (strncmp(line, "* ", 2) == 0 && strstr(line, " FETCH ") && litlen > 0) {
        int to_copy = litlen < s->body_max - 1 ? litlen : s->body_max - 1;
        memcpy(s->body, lit, (unsigned)to_copy);
        s->body[to_copy] = '\0';
        s->body_len      = to_copy;
    }
}

static void cb_select(const char *line, const char *lit, int litlen, void *ud)
{
    (void)lit; (void)litlen;
    select_cb_state_t *s = (select_cb_state_t *)ud;
    /* "* N EXISTS" / "* N UNSEEN" */
    if (strncmp(line, "* ", 2) == 0) {
        int n = atoi(line + 2);
        if (strstr(line, " EXISTS") && s->exists_out) *s->exists_out = n;
        if (strstr(line, " UNSEEN") && s->unseen_out) *s->unseen_out = n;
    }
    /* "[UNSEEN N]" inside OK response */
    const char *p = strstr(line, "[UNSEEN ");
    if (p && s->unseen_out) *s->unseen_out = atoi(p + 8);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int imap_connect(imap_conn_t *c, const char *host,
                 unsigned short port, int use_tls)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;

    /* Resolve host */
    unsigned int ip = sys_net_dns(host);
    if (!ip) return -1;

    /* Create TCP socket */
    long fd = sys_socket(SOCK_TCP);
    if (fd < 0) return -1;

    if (sys_connect(fd, ip, port) < 0) {
        sys_net_close(fd);
        return -1;
    }
    c->fd = fd;

#ifdef HAVE_MBEDTLS
    if (use_tls) {
        tls_global_init();
        tls_conn_t *tls = tls_connect((int)fd, host);
        if (!tls) {
            sys_net_close(fd);
            c->fd = -1;
            return -1;
        }
        c->tls = tls;
    }
#else
    (void)use_tls;
#endif

    /* Read server greeting (e.g. "* OK Dovecot ready.") */
    static char greet[IMAP_LINE_MAX];
    if (imap_readline(c, greet, sizeof(greet)) < 0) {
        imap_logout(c);
        return -1;
    }
    return 0;
}

int imap_login(imap_conn_t *c, const char *user, const char *pass)
{
    char tag[8];
    /* Credentials are sent as quoted strings — escape " and \ in them */
    char cmd[IMAP_LINE_MAX];
    snprintf(cmd, sizeof(cmd), "LOGIN \"%s\" \"%s\"", user, pass);
    if (imap_cmd(c, tag, "%s", cmd) < 0) return -1;
    return imap_await(c, tag, NULL, NULL);
}

int imap_list(imap_conn_t *c, mail_folder_t *folders, int max, int *count_out)
{
    list_cb_state_t s = { folders, max, 0 };
    char tag[8];
    if (imap_cmd(c, tag, "LIST \"\" \"*\"") < 0) return -1;
    int r = imap_await(c, tag, cb_list, &s);
    *count_out = s.count;
    return (r >= 0) ? 0 : -1;
}

int imap_select(imap_conn_t *c, const char *mailbox)
{
    select_cb_state_t s = { &c->selected_exists, &c->selected_unseen };
    c->selected_exists = 0;
    c->selected_unseen = 0;
    char tag[8];
    if (imap_cmd(c, tag, "SELECT \"%s\"", mailbox) < 0) return -1;
    return imap_await(c, tag, cb_select, &s);
}

int imap_search_all(imap_conn_t *c, int *seqnums, int max, int *count_out)
{
    search_cb_state_t s = { seqnums, max, 0 };
    char tag[8];
    if (imap_cmd(c, tag, "SEARCH ALL") < 0) return -1;
    int r = imap_await(c, tag, cb_search, &s);
    *count_out = s.count;
    return (r >= 0) ? 0 : -1;
}

int imap_fetch_headers(imap_conn_t *c, int first, int last,
                       mail_msg_t *msgs, int max, int *count_out)
{
    fetch_hdr_state_t s = { msgs, max, 0, 0, 0 };
    char tag[8];
    if (imap_cmd(c, tag,
                 "FETCH %d:%d (FLAGS BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)])",
                 first, last) < 0)
        return -1;
    int r = imap_await(c, tag, cb_fetch_hdr, &s);
    *count_out = s.count;
    return (r >= 0) ? 0 : -1;
}

int imap_fetch_body(imap_conn_t *c, int seq_num, char *body, int body_max)
{
    fetch_body_state_t s = { body, body_max, 0 };
    body[0] = '\0';
    char tag[8];
    if (imap_cmd(c, tag, "FETCH %d BODY.PEEK[TEXT]", seq_num) < 0) return -1;
    int r = imap_await(c, tag, cb_fetch_body, &s);
    return (r >= 0) ? 0 : -1;
}

int imap_delete_and_expunge(imap_conn_t *c, int seq_num)
{
    char tag[8];
    if (imap_cmd(c, tag, "STORE %d +FLAGS (\\Deleted)", seq_num) < 0) return -1;
    if (imap_await(c, tag, NULL, NULL) <= 0) return -1;

    if (imap_cmd(c, tag, "EXPUNGE") < 0) return -1;
    return (imap_await(c, tag, NULL, NULL) > 0) ? 0 : -1;
}

void imap_logout(imap_conn_t *c)
{
    if (c->fd < 0) return;
    char tag[8];
    imap_cmd(c, tag, "LOGOUT");
    imap_await(c, tag, NULL, NULL);   /* consume BYE + OK */

#ifdef HAVE_MBEDTLS
    if (c->tls) {
        tls_close((tls_conn_t *)c->tls);
        c->tls = NULL;
    }
#endif
    sys_net_close(c->fd);
    c->fd = -1;
}
