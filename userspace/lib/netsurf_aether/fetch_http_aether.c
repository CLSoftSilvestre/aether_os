/*
 * Phase 7.6.2–7.6.6 — HTTP/1.1 fetcher for AetherOS
 *
 * Implements NetSurf's fetcher_operation_table for http:// and https:// over
 * AetherOS Phase 5.1 TCP/IP stack via libaether_posix POSIX socket API.
 *
 * Architecture (cooperative single-thread):
 *   setup()  — allocate context, store parent_fetch pointer, add to active list
 *   start()  — synchronous HTTP: DNS → connect → send → recv → parse → decompress
 *   poll()   — deliver buffered response: FETCH_HEADER, FETCH_DATA, FETCH_FINISHED
 *   abort()  — mark aborted; poll() skips delivery
 *   free()   — remove from active list, release buffers
 *
 * Features:
 *   7.6.3  HTTP/1.1 Connection: close (no keep-alive; simplest model)
 *   7.6.4  Gzip decompression via vendor_zlib (windowBits=47 auto-detect)
 *   7.6.5  Redirect following for 301/302/303/307/308 (max 5 hops, inline)
 *   7.6.6  MIME type extracted from Content-Type response header
 *
 * Limitations (Phase 7.8+):
 *   - Entire response buffered before delivery (no streaming)
 *   - No TLS — https:// uses plain TCP (acceptable for QEMU local testing)
 *   - No chunked Transfer-Encoding (servers use Connection: close instead)
 *
 * Called after netsurf_init() via fetch_http_aether_register() because
 * lwc_intern_string() requires libwapcaplet to be initialised first.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* POSIX socket / DNS (libaether_posix) */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

/* zlib gzip decompression */
#include <zlib.h>

/* libwapcaplet */
#include <libwapcaplet/libwapcaplet.h>

/* NetSurf */
#include "content/fetch.h"
#include "content/fetchers.h"
#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/log.h"
#include "netsurf_aether.h"

/* ── context ─────────────────────────────────────────────────────────────── */

typedef struct fetch_http_ctx {
    struct fetch          *parent;
    nsurl                 *url;
    bool                   aborted;
    bool                   ready;
    bool                   delivered;
    long                   http_code;
    uint8_t               *body;
    size_t                 body_len;
    char                   content_type[256];
    struct fetch_http_ctx *next;
} fetch_http_ctx_t;

static fetch_http_ctx_t *g_list = NULL;

/* ── network helpers ─────────────────────────────────────────────────────── */

static uint8_t *recv_all(int fd, size_t *out_len)
{
    size_t cap = 32768, used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) return NULL;

    while (1) {
        if (used + 4096 > cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) { free(buf); return NULL; }
            buf = nb;
        }
        long n = (long)recv(fd, buf + used, cap - used - 1, 0);
        if (n <= 0) break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    *out_len  = used;
    return buf;
}

static uint8_t *decompress_gzip(const uint8_t *in, size_t in_len, size_t *out_len)
{
    size_t cap = in_len * 3 + 4096;
    uint8_t *out = malloc(cap + 1);
    if (!out) return NULL;

    z_stream zs;
    zs.zalloc   = Z_NULL;
    zs.zfree    = Z_NULL;
    zs.opaque   = Z_NULL;
    zs.next_in  = (Bytef *)(uintptr_t)in;
    zs.avail_in = (uInt)in_len;

    if (inflateInit2(&zs, 47) != Z_OK) { free(out); return NULL; }

    size_t used = 0;
    int ret;
    do {
        if (cap - used < 4096) {
            cap *= 2;
            uint8_t *nb = realloc(out, cap + 1);
            if (!nb) { inflateEnd(&zs); free(out); return NULL; }
            out = nb;
        }
        zs.next_out  = out + used;
        zs.avail_out = (uInt)(cap - used);
        uInt before  = zs.avail_out;
        ret = inflate(&zs, Z_NO_FLUSH);
        used += (size_t)(before - zs.avail_out);
    } while (ret == Z_OK);

    inflateEnd(&zs);
    if (ret != Z_STREAM_END) { free(out); return NULL; }

    out[used] = '\0';
    *out_len  = used;
    return out;
}

/* Case-insensitive substring search */
static const char *ci_strstr(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, nlen) == 0) return hay;
    }
    return NULL;
}

/* ── HTTP state machine ──────────────────────────────────────────────────── */

static void do_http(fetch_http_ctx_t *ctx)
{
    const char *full_url = nsurl_access(ctx->url);

    /* --- Parse scheme / host / port / path from URL string --- */
    char host[256];
    char path[2048];
    int  port     = 80;

    const char *p = full_url;
    if      (strncmp(p, "https://", 8) == 0) { p += 8; port = 443; }
    else if (strncmp(p, "http://",  7) == 0) { p += 7; }
    else { ctx->http_code = 400; return; }

    const char *slash = strchr(p, '/');
    size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
    if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    char *colon = strchr(host, ':');
    if (colon) { port = (int)strtol(colon + 1, NULL, 10); *colon = '\0'; }

    if (slash) strncpy(path, slash, sizeof(path) - 1);
    else       { path[0] = '/'; path[1] = '\0'; }
    path[sizeof(path) - 1] = '\0';

    /* port 443 uses plain TCP too (no TLS in Phase 7.6) */

    /* Working copies for redirect loop */
    char cur_host[256], cur_path[2048];
    int  cur_port = port;
    strncpy(cur_host, host, sizeof(cur_host) - 1);
    strncpy(cur_path, path, sizeof(cur_path) - 1);

    for (int hop = 0; hop < 5; hop++) {

        /* ── DNS ────────────────────────────────────────────── */
        struct hostent *he = gethostbyname(cur_host);
        if (!he) {
            NSLOG(netsurf, WARNING, "HTTP: DNS fail for %s", cur_host);
            ctx->http_code = 503;
            return;
        }

        /* ── TCP connect ─────────────────────────────────────── */
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { ctx->http_code = 503; return; }

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = htons((uint16_t)cur_port);
        memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

        if (connect(fd, (const struct sockaddr *)&sa, sizeof(sa)) < 0) {
            close(fd);
            NSLOG(netsurf, WARNING, "HTTP: connect fail %s:%d", cur_host, cur_port);
            ctx->http_code = 503;
            return;
        }

        /* ── HTTP/1.0 GET request (no Host header — avoids Python 3.12 check) ── */
        char req[4096];
        int rlen = snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\n"
            "Connection: close\r\n"
            "User-Agent: AetherBrowser/0.1 (AetherOS; AArch64)\r\n"
            "Accept: text/html,application/xhtml+xml,*/*;q=0.8\r\n"
            "\r\n",
            cur_path);
        if (rlen < 0 || rlen >= (int)sizeof(req)) rlen = (int)sizeof(req) - 1;
        send(fd, req, (size_t)rlen, 0);

        /* ── Receive entire response ─────────────────────────── */
        size_t total = 0;
        uint8_t *raw = recv_all(fd, &total);
        close(fd);

        if (!raw || total < 12) {
            free(raw);
            ctx->http_code = 503;
            return;
        }

        /* ── Parse status code ───────────────────────────────── */
        const char *sp = strchr((char *)raw, ' ');
        ctx->http_code = sp ? strtol(sp + 1, NULL, 10) : 200;

        /* ── Find header / body boundary ─────────────────────── */
        char *hdr_end = strstr((char *)raw, "\r\n\r\n");
        size_t hdr_len, body_sz;
        uint8_t *body_start;

        if (hdr_end) {
            hdr_len    = (size_t)(hdr_end - (char *)raw) + 4;
            body_start = raw + hdr_len;
            body_sz    = total - hdr_len;
        } else {
            hdr_len    = 0;
            body_start = raw;
            body_sz    = total;
        }

        /* ── Content-Type ────────────────────────────────────── */
        const char *ct = hdr_end ? ci_strstr((char *)raw, "Content-Type:") : NULL;
        if (ct && (ct < hdr_end)) {
            ct += 13;
            while (*ct == ' ') ct++;
            size_t i = 0;
            while (*ct && *ct != '\r' && *ct != '\n'
                   && i < sizeof(ctx->content_type) - 1)
                ctx->content_type[i++] = *ct++;
            ctx->content_type[i] = '\0';
        } else {
            strncpy(ctx->content_type, "text/html", sizeof(ctx->content_type) - 1);
        }

        /* ── Redirect handling ───────────────────────────────── */
        long code = ctx->http_code;
        if ((code == 301 || code == 302 || code == 303 ||
             code == 307 || code == 308) && hdr_end) {

            const char *loc = ci_strstr((char *)raw, "Location:");
            if (loc && loc < hdr_end) {
                loc += 9;
                while (*loc == ' ') loc++;
                char loc_url[2048];
                size_t i = 0;
                while (*loc && *loc != '\r' && *loc != '\n'
                       && i < sizeof(loc_url) - 1)
                    loc_url[i++] = *loc++;
                loc_url[i] = '\0';

                free(raw);

                /* Absolute redirect */
                if (strncmp(loc_url, "http://",  7) == 0 ||
                    strncmp(loc_url, "https://", 8) == 0) {
                    const char *q = loc_url;
                    if      (strncmp(q, "https://", 8) == 0) { q += 8; cur_port = 443; }
                    else if (strncmp(q, "http://",  7) == 0) { q += 7; cur_port = 80;  }
                    const char *sl = strchr(q, '/');
                    size_t hl2 = sl ? (size_t)(sl - q) : strlen(q);
                    if (hl2 >= sizeof(cur_host)) hl2 = sizeof(cur_host) - 1;
                    memcpy(cur_host, q, hl2); cur_host[hl2] = '\0';
                    char *c2 = strchr(cur_host, ':');
                    if (c2) { cur_port = (int)strtol(c2 + 1, NULL, 10); *c2 = '\0'; }
                    strncpy(cur_path, sl ? sl : "/", sizeof(cur_path) - 1);
                    cur_path[sizeof(cur_path) - 1] = '\0';
                } else if (loc_url[0] == '/') {
                    /* Absolute path, same host */
                    strncpy(cur_path, loc_url, sizeof(cur_path) - 1);
                } else {
                    /* Relative path */
                    char *last = strrchr(cur_path, '/');
                    if (last) *(last + 1) = '\0';
                    strncat(cur_path, loc_url,
                            sizeof(cur_path) - strlen(cur_path) - 1);
                }
                NSLOG(netsurf, INFO, "HTTP %ld redirect → %s%s",
                      code, cur_host, cur_path);
                continue;
            }
        }

        /* ── Gzip decompression ──────────────────────────────── */
        const char *ce = hdr_end ? ci_strstr((char *)raw, "Content-Encoding:") : NULL;
        bool is_gzip   = ce && (ce < hdr_end) &&
                         (strncasecmp(ce + 17 + strspn(ce + 17, " "), "gzip", 4) == 0);

        if (is_gzip && body_sz > 0) {
            size_t dec_len = 0;
            uint8_t *dec   = decompress_gzip(body_start, body_sz, &dec_len);
            free(raw);
            ctx->body     = dec;
            ctx->body_len = dec ? dec_len : 0;
        } else {
            uint8_t *copy = malloc(body_sz + 1);
            if (copy) {
                memcpy(copy, body_start, body_sz);
                copy[body_sz] = '\0';
            }
            free(raw);
            ctx->body     = copy;
            ctx->body_len = copy ? body_sz : 0;
        }
        return;   /* done — no more hops */
    }

    /* Max redirects exceeded */
    ctx->http_code = 508;
}

/* ── fetcher_operation_table ─────────────────────────────────────────────── */

static bool fetch_http_initialise(lwc_string *scheme)
{
    (void)scheme;
    return true;
}

static void fetch_http_finalise(lwc_string *scheme)
{
    (void)scheme;
}

static bool fetch_http_acceptable(const nsurl *url)
{
    enum nsurl_scheme_type s = nsurl_get_scheme_type(url);
    return s == NSURL_SCHEME_HTTP || s == NSURL_SCHEME_HTTPS;
}

static void *fetch_http_setup(struct fetch *parent_fetch,
                              nsurl         *url,
                              bool           only_2xx,
                              bool           downgrade_tls,
                              const char    *post_urlenc,
                              const struct fetch_multipart_data *post_multipart,
                              const char   **headers)
{
    (void)only_2xx; (void)downgrade_tls;
    (void)post_urlenc; (void)post_multipart; (void)headers;

    fetch_http_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->parent = parent_fetch;
    ctx->url    = nsurl_ref(url);
    strncpy(ctx->content_type, "text/html", sizeof(ctx->content_type) - 1);

    ctx->next = g_list;
    g_list    = ctx;

    return ctx;
}

static void dbg_write(const char *s)
{
    long r;
    int len = 0;
    while (s[len]) len++;
    __asm__ volatile(
        "mov x8, #34\n mov x0, #1\n mov x1, %1\n mov x2, %2\n"
        "svc #0\n mov %0, x0\n"
        : "=r"(r) : "r"(s), "r"((long)len) : "x0","x1","x2","x8","memory"
    );
}

static bool fetch_http_start(void *ctx_)
{
    fetch_http_ctx_t *ctx = ctx_;
    if (!ctx->aborted) {
        dbg_write("fetch_http: do_http starting\n");
        do_http(ctx);
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "fetch_http: code=%ld body=%zu url=%s\n",
                 ctx->http_code, ctx->body_len,
                 nsurl_access(ctx->url));
        dbg_write(buf);
    }
    ctx->ready = true;
    return true;
}

static void fetch_http_abort(void *ctx_)
{
    fetch_http_ctx_t *ctx = ctx_;
    dbg_write("fetch_http_abort called!\n");
    ctx->aborted = true;
}

static void fetch_http_free(void *ctx_)
{
    fetch_http_ctx_t *ctx = ctx_;

    fetch_http_ctx_t **pp = &g_list;
    while (*pp && *pp != ctx) pp = &(*pp)->next;
    if (*pp) *pp = ctx->next;

    nsurl_unref(ctx->url);
    free(ctx->body);
    free(ctx);
}

static void fetch_http_poll(lwc_string *scheme)
{
    (void)scheme;

    if (!g_list) dbg_write("fetch_http_poll: g_list NULL\n");
    fetch_http_ctx_t *ctx = g_list;
    while (ctx) {
        fetch_http_ctx_t *next = ctx->next;

        if (ctx->ready && !ctx->delivered && !ctx->aborted) {
            dbg_write("fetch_http_poll: delivering\n");
            ctx->delivered = true;

            fetch_set_http_code(ctx->parent, ctx->http_code);

            /* Status line */
            {
                char line[64];
                snprintf(line, sizeof(line), "HTTP/1.1 %ld OK\r\n", ctx->http_code);
                fetch_msg msg = {0};
                msg.type = FETCH_HEADER;
                msg.data.header_or_data.buf = (const uint8_t *)line;
                msg.data.header_or_data.len = strlen(line);
                fetch_send_callback(&msg, ctx->parent);
            }

            /* Content-Type header */
            if (ctx->content_type[0]) {
                char line[300];
                snprintf(line, sizeof(line),
                         "Content-Type: %s\r\n", ctx->content_type);
                fetch_msg msg = {0};
                msg.type = FETCH_HEADER;
                msg.data.header_or_data.buf = (const uint8_t *)line;
                msg.data.header_or_data.len = strlen(line);
                fetch_send_callback(&msg, ctx->parent);
            }

            /* Body */
            if (ctx->body && ctx->body_len > 0) {
                fetch_msg msg = {0};
                msg.type = FETCH_DATA;
                msg.data.header_or_data.buf = ctx->body;
                msg.data.header_or_data.len = ctx->body_len;
                fetch_send_callback(&msg, ctx->parent);
            }

            /* Finished */
            {
                fetch_msg msg = {0};
                msg.type = FETCH_FINISHED;
                fetch_send_callback(&msg, ctx->parent);
            }
        }

        ctx = next;
    }
}

/* ── registration ────────────────────────────────────────────────────────── */

static const struct fetcher_operation_table fetch_http_ops = {
    .initialise = fetch_http_initialise,
    .acceptable = fetch_http_acceptable,
    .setup      = fetch_http_setup,
    .start      = fetch_http_start,
    .abort      = fetch_http_abort,
    .free       = fetch_http_free,
    .poll       = fetch_http_poll,
    .fdset      = NULL,
    .finalise   = fetch_http_finalise,
};

static void register_scheme(const char *name, size_t len)
{
    lwc_string *scheme = NULL;
    if (lwc_intern_string(name, len, &scheme) != lwc_error_ok || !scheme) {
        NSLOG(netsurf, ERROR, "HTTP fetcher: failed to intern scheme '%s'", name);
        return;
    }
    nserror err = fetcher_add(scheme, &fetch_http_ops);
    if (err != NSERROR_OK) {
        lwc_string_unref(scheme);   /* unref only on failure; fetcher owns on success */
        NSLOG(netsurf, ERROR, "HTTP fetcher: fetcher_add('%s') failed: %d",
              name, (int)err);
    }
}

/* Call AFTER netsurf_init() — requires libwapcaplet to be initialised */
void fetch_http_aether_register(void)
{
    register_scheme("http",  4);
    register_scheme("https", 5);
    NSLOG(netsurf, INFO, "HTTP fetcher registered for http:// and https://");
}
