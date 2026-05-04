/*
 * AetherOS — aether_http
 * File: userspace/apps/aether_http/main.c
 *
 * HTTP/1.1 client.
 *
 * Usage:
 *   aether_http <url>
 *   aether_http <METHOD> <url>
 *
 * URL forms accepted:
 *   http://hostname[:port][/path]
 *   hostname[:port][/path]
 *   a.b.c.d[:port][/path]
 *
 * Compliance:
 *   - HTTP/1.1 request with mandatory Host: header
 *   - Connection: close  (no keep-alive)
 *   - Parses response status line  HTTP/1.1 NNN reason
 *   - Framing via Content-Length  (stops early; no 5-second timeout wait)
 *   - Chunked transfer-encoding decoding
 *   - Read-until-close fallback when neither header is present
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys.h>

/* ── Tunables ───────────────────────────────────────────────────────────────── */

#define RX_CAP     (64 * 1024)   /* maximum response buffer                    */
#define RECV_CHUNK  2048          /* bytes requested per sys_net_recv() call    */
#define REQ_CAP     1024          /* request line + headers                     */

/* ── Static storage (off user stack) ────────────────────────────────────────── */

static char g_rx [RX_CAP + 1];
static char g_req[REQ_CAP];

/* ── Small helpers ───────────────────────────────────────────────────────────── */

static void emit(const char *s, long n)
{
    if (n > 0) sys_write(STDOUT_FILENO, s, n);
}

/* Fold ASCII letter to lower-case for comparison only. */
static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c | 0x20) : c;
}

/* Case-insensitive strstr (ASCII). Returns pointer into hay or NULL. */
static const char *istrstr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    if (!nl) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && lc(hay[i]) == lc(needle[i])) i++;
        if (i == nl) return hay;
    }
    return NULL;
}

/* Skip leading spaces/tabs then parse decimal digits. */
static long parse_dec(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return v;
}

/* Skip leading spaces then parse hex digits (chunk-size lines). */
static long parse_hex(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    long v = 0;
    for (;;) {
        char c = *s++;
        if      (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (c - 'A' + 10);
        else break;
    }
    return v;
}

/* Parse dotted-decimal IPv4 string → host-order uint32.
 * Returns 0 if the string is not a valid dotted-decimal address. */
static unsigned int parse_dotted(const char *s)
{
    /* Quick sanity: must contain exactly 3 dots and only digits/dots. */
    int dots = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '.') { dots++; }
        else if (*p < '0' || *p > '9') return 0;
    }
    if (dots != 3) return 0;

    unsigned int a = 0, b = 0, c = 0, d = 0;
    while (*s >= '0' && *s <= '9') a = a*10 + (*s++ - '0'); if (*s == '.') s++;
    while (*s >= '0' && *s <= '9') b = b*10 + (*s++ - '0'); if (*s == '.') s++;
    while (*s >= '0' && *s <= '9') c = c*10 + (*s++ - '0'); if (*s == '.') s++;
    while (*s >= '0' && *s <= '9') d = d*10 + (*s++ - '0');
    return (a << 24) | (b << 16) | (c << 8) | d;
}

/* ── URL parser ──────────────────────────────────────────────────────────────── */

typedef struct {
    char           host[128];
    unsigned short port;
    char           path[512];
} url_t;

/* Parse http://host[:port][/path] or host[:port][/path] into *out.
 * Returns 0 on success, -1 on error. */
static int parse_url(const char *raw, url_t *out)
{
    const char *p = raw;

    /* Reject HTTPS; strip any other scheme prefix before "//".
     * Also tolerates keyboard layout glitches like "http|//" → "http://". */
    if (strncmp(p, "https://", 8) == 0 || strncmp(p, "https|//", 8) == 0) {
        printf("aether_http: HTTPS not supported\n");
        return -1;
    }
    const char *dslash = strstr(p, "//");
    if (dslash != NULL && (size_t)(dslash - p) < 10)
        p = dslash + 2;

    /* Host part: up to ':' or '/' */
    int i = 0;
    while (*p && *p != ':' && *p != '/' && i < 127)
        out->host[i++] = *p++;
    out->host[i] = '\0';
    if (i == 0) return -1;

    /* Optional port */
    out->port = 80;
    if (*p == ':') {
        p++;
        out->port = (unsigned short)atoi(p);
        while (*p && *p != '/') p++;
    }

    /* Path (default to "/") */
    if (*p == '/') {
        int j = 0;
        while (*p && j < 511) out->path[j++] = *p++;
        out->path[j] = '\0';
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }

    return 0;
}

/* ── Chunked transfer-encoding decoder ───────────────────────────────────────── */

/*
 * RFC 7230 §4.1 chunked body format:
 *   chunk        = chunk-size [ chunk-ext ] CRLF chunk-data CRLF
 *   last-chunk   = 1*("0") [ chunk-ext ] CRLF
 *   chunked-body = *chunk last-chunk trailer-part CRLF
 */
static void decode_chunked(const char *data, long len)
{
    const char *p   = data;
    const char *end = data + len;

    while (p < end) {
        /* Locate end of chunk-size line */
        const char *nl = p;
        while (nl < end && *nl != '\r' && *nl != '\n') nl++;
        if (nl >= end) break;

        long chunk_sz = parse_hex(p);

        /* Skip past CRLF */
        p = nl;
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;

        if (chunk_sz == 0) break;   /* last-chunk */

        /* Clamp to what we actually have buffered */
        long avail = (long)(end - p);
        long out_n = (chunk_sz < avail) ? chunk_sz : avail;
        emit(p, out_n);
        p += chunk_sz;

        /* Skip trailing CRLF after chunk data */
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;
    }
}

/* ── Response receiver ───────────────────────────────────────────────────────── */

/*
 * Read data from fd into buf (capacity = RX_CAP) until one of:
 *   a) Connection closes / error  (sys_net_recv returns ≤ 0)
 *   b) Buffer full
 *   c) We have received Content-Length bytes of body  (avoids 5-second timeout)
 *
 * Returns total bytes received.
 */
static long recv_response(long fd, char *buf, long capacity)
{
    long total       = 0;
    long hdr_end_off = -1;   /* byte offset just past \r\n\r\n */
    long body_need   = -1;   /* Content-Length value, or -1 if unknown */

    while (total < capacity) {
        long want = capacity - total;
        if (want > RECV_CHUNK) want = RECV_CHUNK;

        long n = sys_net_recv(fd, buf + total, want);
        if (n <= 0) break;   /* EOF or 5-second timeout → done */
        total += n;

        /* Once we have headers, parse Content-Length to enable early exit. */
        if (hdr_end_off < 0) {
            buf[total] = '\0';
            const char *sep = strstr(buf, "\r\n\r\n");
            if (sep) {
                hdr_end_off = (long)(sep - buf) + 4;

                const char *cl = istrstr(buf, "\r\ncontent-length:");
                if (!cl) cl = istrstr(buf, "\ncontent-length:");
                if (cl) {
                    const char *colon = strchr(cl, ':');
                    if (colon) body_need = parse_dec(colon + 1);
                }
            }
        }

        /* Stop as soon as the full body has arrived. */
        if (hdr_end_off >= 0 && body_need >= 0 &&
            total >= hdr_end_off + body_need)
            break;
    }

    return total;
}

/* ── Entry point ─────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: aether_http <url>\n");
        printf("       aether_http <METHOD> <url>\n");
        printf("  e.g. aether_http http://10.0.2.2:8080/\n");
        printf("       aether_http GET http://example.com/index.html\n");
        return 1;
    }

    static char method_buf[16];
    const char *method;
    const char *raw_url;

    if (argc >= 3) {
        /* Uppercase the method so "get" works the same as "GET" */
        int mi = 0;
        for (; argv[1][mi] && mi < 15; mi++)
            method_buf[mi] = (argv[1][mi] >= 'a' && argv[1][mi] <= 'z')
                           ? (char)(argv[1][mi] - 32) : argv[1][mi];
        method_buf[mi] = '\0';
        method  = method_buf;
        raw_url = argv[2];
    } else {
        method  = "GET";
        raw_url = argv[1];
    }

    /* ── Parse URL ── */
    url_t url;
    if (parse_url(raw_url, &url) < 0) {
        printf("http: cannot parse URL '%s'\n", raw_url);
        return 1;
    }

    /* ── Resolve host → IP ── */
    unsigned int ip = parse_dotted(url.host);
    if (!ip) {
        printf("Resolving %s...\n", url.host);
        ip = sys_net_dns(url.host);
        if (!ip) {
            printf("aether_http: cannot resolve host '%s'\n", url.host);
            return 1;
        }
        printf("Resolved: %u.%u.%u.%u\n",
               (ip >> 24) & 0xff, (ip >> 16) & 0xff,
               (ip >>  8) & 0xff,  ip        & 0xff);
    }

    /* ── Build HTTP/1.1 request ── */
    int reqlen = snprintf(g_req, REQ_CAP,
        "%s %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: AetherOS/0.1\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        method, url.path, url.host);

    /* ── Connect ── */
    long fd = sys_socket(SOCK_TCP);
    if (fd < 0) { printf("http: socket() failed\n"); return 1; }

    printf("Connecting to %s:%u...\n", url.host, (unsigned)url.port);
    if (sys_connect(fd, ip, url.port) < 0) {
        printf("http: connect failed\n");
        sys_net_close(fd);
        return 1;
    }

    /* ── Send request ── */
    if (sys_net_send(fd, g_req, reqlen) < 0) {
        printf("http: send failed\n");
        sys_net_close(fd);
        return 1;
    }

    /* ── Receive response ── */
    long total = recv_response(fd, g_rx, RX_CAP);
    sys_net_close(fd);

    if (total <= 0) {
        printf("http: no data received\n");
        return 1;
    }
    g_rx[total] = '\0';

    /* ── Locate header/body boundary ── */
    const char *sep = strstr(g_rx, "\r\n\r\n");
    if (!sep) {
        /* Malformed response: dump raw bytes and exit */
        emit(g_rx, total);
        return 1;
    }

    long        hdr_len  = (long)(sep - g_rx) + 4;
    const char *body     = g_rx + hdr_len;
    long        body_len = total - hdr_len;

    /* ── Parse status line: HTTP/1.x NNN reason ── */
    int status_code = 0;
    {
        /* First space separates version from status code */
        const char *sp = (const char *)memchr(g_rx, ' ', hdr_len < 20 ? hdr_len : 20);
        if (sp) {
            status_code = (int)parse_dec(sp + 1);
            const char *reason = sp + 1;
            while (*reason && *reason != ' ') reason++;
            if (*reason == ' ') reason++;
            const char *rend = reason;
            while (*rend && *rend != '\r' && *rend != '\n') rend++;
            printf("< HTTP/1.1 %d ", status_code);
            emit(reason, (long)(rend - reason));
            printf("\n");
        }
    }

    /* ── Detect Transfer-Encoding: chunked ── */
    int chunked = 0;
    {
        const char *te = istrstr(g_rx, "\r\ntransfer-encoding:");
        if (!te) te = istrstr(g_rx, "\ntransfer-encoding:");
        if (te) {
            const char *v = strchr(te, ':');
            if (v && istrstr(v + 1, "chunked")) chunked = 1;
        }
    }

    /* ── Parse Content-Length ── */
    long content_length = -1;
    {
        const char *cl = istrstr(g_rx, "\r\ncontent-length:");
        if (!cl) cl = istrstr(g_rx, "\ncontent-length:");
        if (cl) {
            const char *v = strchr(cl, ':');
            if (v) content_length = parse_dec(v + 1);
        }
    }

    /* Clamp body_len to Content-Length if available */
    if (content_length >= 0 && body_len > content_length)
        body_len = content_length;

    /* ── Print Content-Type for context ── */
    {
        const char *ct = istrstr(g_rx, "\r\ncontent-type:");
        if (!ct) ct = istrstr(g_rx, "\ncontent-type:");
        if (ct) {
            const char *v = strchr(ct, ':');
            if (v) {
                v++;
                while (*v == ' ') v++;
                const char *vend = v;
                while (*vend && *vend != '\r' && *vend != '\n') vend++;
                printf("< Content-Type: ");
                emit(v, (long)(vend - v));
                printf("\n");
            }
        }
    }
    printf("\n");   /* blank line separating headers from body */

    /* ── Output body ── */
    if (chunked) {
        decode_chunked(body, body_len);
    } else {
        emit(body, body_len);
    }

    /* Ensure terminal is left on a fresh line */
    if (body_len > 0 && body[body_len - 1] != '\n')
        emit("\n", 1);

    return (status_code >= 200 && status_code < 300) ? 0 : 1;
}
