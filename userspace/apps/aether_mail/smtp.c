/*
 * AetherOS — AetherMail SMTP client
 * File: userspace/apps/aether_mail/smtp.c
 *
 * Minimal SMTP implementation:
 *   1. Connect TCP (+ optional TLS)
 *   2. Read 220 greeting
 *   3. EHLO aetheros
 *   4. AUTH LOGIN (base64-encode credentials)
 *   5. MAIL FROM, RCPT TO, DATA
 *   6. Send headers + body + ".\r\n"
 *   7. QUIT
 */

#include "smtp.h"
#include <sys.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_MBEDTLS
#include "tls_aether.h"
#endif

/* ── Base64 encoder ──────────────────────────────────────────────────────── */

static const char b64t[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const char *in, int inlen, char *out, int outmax)
{
    int i, j = 0;
    for (i = 0; i < inlen && j + 4 < outmax - 1; i += 3) {
        unsigned a = (unsigned char)in[i];
        unsigned b = (i + 1 < inlen) ? (unsigned char)in[i + 1] : 0u;
        unsigned cc = (i + 2 < inlen) ? (unsigned char)in[i + 2] : 0u;
        unsigned n  = (a << 16) | (b << 8) | cc;
        out[j++] = b64t[(n >> 18) & 63u];
        out[j++] = b64t[(n >> 12) & 63u];
        out[j++] = (i + 1 < inlen) ? b64t[(n >> 6) & 63u] : '=';
        out[j++] = (i + 2 < inlen) ? b64t[n & 63u]        : '=';
    }
    out[j] = '\0';
    return j;
}

/* ── SMTP connection state ───────────────────────────────────────────────── */

typedef struct {
    long  fd;
    void *tls;
    char  rbuf[4096];
    int   rstart;
    int   rlen;
} smtp_conn_t;

/* Forward declaration — defined at the end of this file. */
static void smtp_close(smtp_conn_t *c);

/* ── Low-level I/O ───────────────────────────────────────────────────────── */

static long smtp_send_raw(smtp_conn_t *c, const void *buf, long len)
{
#ifdef HAVE_MBEDTLS
    if (c->tls)
        return (long)tls_write((tls_conn_t *)c->tls, buf, (size_t)len);
#endif
    return sys_net_send(c->fd, buf, len);
}

static long smtp_recv_raw(smtp_conn_t *c, void *buf, long len)
{
#ifdef HAVE_MBEDTLS
    if (c->tls)
        return (long)tls_read((tls_conn_t *)c->tls, buf, (size_t)len);
#endif
    return sys_net_recv(c->fd, buf, len);
}

static int smtp_getc(smtp_conn_t *c)
{
    if (c->rlen == 0) {
        long n = smtp_recv_raw(c, c->rbuf, (long)sizeof(c->rbuf));
        if (n <= 0) return -1;
        c->rstart = 0;
        c->rlen   = (int)n;
    }
    int ch = (unsigned char)c->rbuf[c->rstart++];
    c->rlen--;
    return ch;
}

/* Read one SMTP response line (strips \r\n). Returns length or -1. */
static int smtp_readline(smtp_conn_t *c, char *line, int maxlen)
{
    int n = 0;
    for (;;) {
        int ch = smtp_getc(c);
        if (ch < 0) return -1;
        if (ch == '\n') break;
        if (ch == '\r') continue;
        if (n < maxlen - 1) line[n++] = (char)ch;
    }
    line[n] = '\0';
    return n;
}

/*
 * Read all continuation lines for a multi-line SMTP response.
 * SMTP multi-line: "NNN-text" (continue), "NNN text" (final line).
 * Returns the 3-digit status code, or -1 on error.
 */
static int smtp_read_response(smtp_conn_t *c, char *last_line, int maxlen)
{
    static char line[512];
    int code = -1;
    for (;;) {
        if (smtp_readline(c, line, sizeof(line)) < 0) return -1;
        if (strlen(line) >= 3) code = atoi(line);
        if (last_line) {
            strncpy(last_line, line, (unsigned)(maxlen - 1));
            last_line[maxlen - 1] = '\0';
        }
        /* Continuation: "NNN-" vs final: "NNN " or "NNN" */
        if (strlen(line) < 4 || line[3] != '-') break;
    }
    return code;
}

/* Send a CRLF-terminated line. */
static int smtp_sendline(smtp_conn_t *c, const char *line)
{
    static char buf[1024];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", line);
    return (smtp_send_raw(c, buf, (long)n) == (long)n) ? 0 : -1;
}

/* ── smtp_send ───────────────────────────────────────────────────────────── */

int smtp_send(const mail_account_t *acc,
              const char *to,
              const char *subject,
              const char *body,
              char *err_out, int err_max)
{
    smtp_conn_t conn;
    memset(&conn, 0, sizeof(conn));
    conn.fd = -1;

#define FAIL(msg) do { \
    if (err_out) strncpy(err_out, (msg), (unsigned)(err_max - 1)); \
    smtp_close(&conn); \
    return -1; \
} while (0)

    /* ── Resolve and connect ─────────────────────────────────────────────── */
    unsigned int ip = sys_net_dns(acc->smtp_host);
    if (!ip) FAIL("DNS lookup failed for SMTP host");

    long fd = sys_socket(SOCK_TCP);
    if (fd < 0) FAIL("Failed to create TCP socket");
    conn.fd = fd;

    if (sys_connect(fd, ip, acc->smtp_port) < 0) FAIL("SMTP connection refused");

#ifdef HAVE_MBEDTLS
    if (acc->smtp_tls) {
        tls_global_init();
        tls_conn_t *tls = tls_connect((int)fd, acc->smtp_host);
        if (!tls) FAIL("TLS handshake failed");
        conn.tls = tls;
    }
#endif

    static char resp[512];
    int code;

    /* ── 220 greeting ─────────────────────────────────────────────────────── */
    code = smtp_read_response(&conn, resp, sizeof(resp));
    if (code != 220) FAIL("No 220 greeting from SMTP server");

    /* ── EHLO ─────────────────────────────────────────────────────────────── */
    smtp_sendline(&conn, "EHLO aetheros");
    code = smtp_read_response(&conn, resp, sizeof(resp));
    if (code != 250) FAIL("EHLO rejected");

    /* ── AUTH LOGIN ───────────────────────────────────────────────────────── */
    smtp_sendline(&conn, "AUTH LOGIN");
    code = smtp_read_response(&conn, resp, sizeof(resp));
    if (code != 334) FAIL("AUTH LOGIN not supported");

    /* Username (base64) */
    static char b64buf[512];
    b64_encode(acc->smtp_user, (int)strlen(acc->smtp_user), b64buf, sizeof(b64buf));
    smtp_sendline(&conn, b64buf);
    code = smtp_read_response(&conn, resp, sizeof(resp));
    if (code != 334) FAIL("Username rejected by SMTP");

    /* Password (base64) */
    b64_encode(acc->smtp_pass, (int)strlen(acc->smtp_pass), b64buf, sizeof(b64buf));
    smtp_sendline(&conn, b64buf);
    code = smtp_read_response(&conn, resp, sizeof(resp));
    if (code != 235) FAIL("SMTP authentication failed");

    /* ── MAIL FROM ────────────────────────────────────────────────────────── */
    static char cmd[256];
    snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>", acc->email);
    smtp_sendline(&conn, cmd);
    code = smtp_read_response(&conn, resp, sizeof(resp));
    if (code != 250) FAIL("MAIL FROM rejected");

    /* ── RCPT TO ──────────────────────────────────────────────────────────── */
    snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>", to);
    smtp_sendline(&conn, cmd);
    code = smtp_read_response(&conn, resp, sizeof(resp));
    if (code != 250) FAIL("RCPT TO rejected");

    /* ── DATA ─────────────────────────────────────────────────────────────── */
    smtp_sendline(&conn, "DATA");
    code = smtp_read_response(&conn, resp, sizeof(resp));
    if (code != 354) FAIL("DATA command rejected");

    /* Headers */
    snprintf(cmd, sizeof(cmd), "From: %s <%s>", acc->display_name, acc->email);
    smtp_sendline(&conn, cmd);
    snprintf(cmd, sizeof(cmd), "To: %s", to);
    smtp_sendline(&conn, cmd);
    snprintf(cmd, sizeof(cmd), "Subject: %s", subject);
    smtp_sendline(&conn, cmd);
    smtp_sendline(&conn, "MIME-Version: 1.0");
    smtp_sendline(&conn, "Content-Type: text/plain; charset=UTF-8");
    smtp_sendline(&conn, "X-Mailer: AetherMail/1.0 (AetherOS)");
    smtp_sendline(&conn, "");  /* blank line = end of headers */

    /* Body — dot-stuff: prefix lines starting with '.' with another '.' */
    const char *p = body;
    while (*p) {
        const char *nl = strchr(p, '\n');
        int llen = nl ? (int)(nl - p) : (int)strlen(p);
        static char lbuf[1024];
        int olen = 0;
        if (llen > 0 && p[0] == '.') lbuf[olen++] = '.'; /* dot-stuff */
        int to_copy = llen < 1020 ? llen : 1020;
        memcpy(lbuf + olen, p, (unsigned)to_copy);
        olen += to_copy;
        lbuf[olen] = '\0';
        smtp_sendline(&conn, lbuf);
        p = nl ? nl + 1 : p + llen;
    }

    /* End of DATA */
    smtp_sendline(&conn, ".");
    code = smtp_read_response(&conn, resp, sizeof(resp));
    if (code != 250) FAIL("Message not accepted by server");

    /* ── QUIT ─────────────────────────────────────────────────────────────── */
    smtp_sendline(&conn, "QUIT");
    smtp_read_response(&conn, NULL, 0);

    smtp_close(&conn);
    if (err_out && err_max > 0) err_out[0] = '\0';
    return 0;

#undef FAIL
}

/* smtp_close is referenced by the FAIL macro — define it here. */
static void smtp_close(smtp_conn_t *c)
{
    if (!c) return;
#ifdef HAVE_MBEDTLS
    if (c->tls) { tls_close((tls_conn_t *)c->tls); c->tls = NULL; }
#endif
    if (c->fd >= 0) { sys_net_close(c->fd); c->fd = -1; }
}
