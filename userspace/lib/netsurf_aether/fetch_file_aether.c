/*
 * Phase 7.6.1 — NetSurf file:// fetcher for AetherOS
 *
 * Provides fetch_file_register() — the symbol that NetSurf's fetcher_init()
 * calls during netsurf_init().  The built-in content/fetchers/file/file.c is
 * excluded from the build (requires scandir which libaether_posix lacks).
 *
 * Each file:// fetch is synchronous: start() opens the VFS file and reads the
 * entire body into a heap buffer; poll() delivers FETCH_HEADER + FETCH_DATA +
 * FETCH_FINISHED on the next scheduler tick.
 *
 * AetherOS VFS syscalls: SYS_FS_OPEN=800, SYS_FS_READ=801, SYS_FS_CLOSE=802
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <libwapcaplet/libwapcaplet.h>

#include "content/fetch.h"
#include "content/fetchers.h"
#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/log.h"
#include "netsurf_aether.h"

/* ── AetherOS VFS syscall helpers ────────────────────────────────────────── */

static inline long _sys1(long n, long a) {
    long r;
    __asm__ volatile("mov x8,%1; mov x0,%2; svc #0; mov %0,x0"
        : "=r"(r) : "r"(n), "r"(a) : "x0","x8","memory");
    return r;
}
static inline long _sys2(long n, long a, long b) {
    long r;
    __asm__ volatile("mov x8,%1; mov x0,%2; mov x1,%3; svc #0; mov %0,x0"
        : "=r"(r) : "r"(n), "r"(a), "r"(b) : "x0","x1","x8","memory");
    return r;
}
static inline long _sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("mov x8,%1; mov x0,%2; mov x1,%3; mov x2,%4; svc #0; mov %0,x0"
        : "=r"(r) : "r"(n), "r"(a), "r"(b), "r"(c) : "x0","x1","x2","x8","memory");
    return r;
}

#define SYS_FS_OPEN    800
#define SYS_FS_READ    801
#define SYS_FS_CLOSE   802

static inline long sys_fs_open(const char *p)  { return _sys1(SYS_FS_OPEN, (long)p); }
static inline long sys_fs_read(long fd, void *b, long n)
                                                { return _sys3(SYS_FS_READ, fd, (long)b, n); }
static inline long sys_fs_close(long fd)        { return _sys1(SYS_FS_CLOSE, fd); }

/* ── MIME helper (used by gui_fetch_aether.c too) ────────────────────────── */

const char *aether_mimetype_for_ext(const char *ext)
{
    static const struct { const char *e; const char *m; } t[] = {
        {"html","text/html"}, {"htm","text/html"}, {"css","text/css"},
        {"js","application/javascript"}, {"png","image/png"},
        {"jpg","image/jpeg"}, {"jpeg","image/jpeg"}, {"gif","image/gif"},
        {"bmp","image/bmp"}, {"ico","image/x-icon"}, {"svg","image/svg+xml"},
        {"txt","text/plain"}, {"xml","text/xml"}, {"json","application/json"},
        {"woff","font/woff"}, {"woff2","font/woff2"}, {"ttf","font/ttf"},
        {NULL, NULL}
    };
    if (!ext) return "application/octet-stream";
    for (int i = 0; t[i].e; i++) {
        if (strcasecmp(ext, t[i].e) == 0) return t[i].m;
    }
    return "application/octet-stream";
}

/* ── context ─────────────────────────────────────────────────────────────── */

typedef struct fetch_file_ctx {
    struct fetch          *parent;
    nsurl                 *url;
    bool                   aborted;
    bool                   ready;
    bool                   delivered;
    uint8_t               *body;
    size_t                 body_len;
    const char            *mime;        /* static string, no free needed */
    struct fetch_file_ctx *next;
} fetch_file_ctx_t;

static fetch_file_ctx_t *g_list = NULL;

/* ── operations ──────────────────────────────────────────────────────────── */

static bool fetch_file_initialise(lwc_string *scheme)
{
    (void)scheme;
    return true;
}

static void fetch_file_finalise(lwc_string *scheme)
{
    (void)scheme;
}

static bool fetch_file_acceptable(const nsurl *url)
{
    return nsurl_get_scheme_type(url) == NSURL_SCHEME_FILE;
}

static void *fetch_file_setup(struct fetch *parent_fetch,
                              nsurl         *url,
                              bool           only_2xx,
                              bool           downgrade_tls,
                              const char    *post_urlenc,
                              const struct fetch_multipart_data *post_multipart,
                              const char   **headers)
{
    (void)only_2xx; (void)downgrade_tls;
    (void)post_urlenc; (void)post_multipart; (void)headers;

    fetch_file_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->parent = parent_fetch;
    ctx->url    = nsurl_ref(url);
    ctx->mime   = "application/octet-stream";

    ctx->next = g_list;
    g_list    = ctx;

    return ctx;
}

static bool fetch_file_start(void *ctx_)
{
    fetch_file_ctx_t *ctx = ctx_;
    if (ctx->aborted) { ctx->ready = true; return true; }

    /* Extract path: "file:///path/to/file" → "/path/to/file" */
    const char *url_str = nsurl_access(ctx->url);
    const char *path    = NULL;
    if (strncmp(url_str, "file://", 7) == 0) {
        path = url_str + 7;  /* skip "file://" — AetherOS paths start with '/' */
        if (path[0] == '/' && path[1] == '/') path++;  /* triple-slash: skip one more */
    }

    if (!path || !path[0]) {
        NSLOG(netsurf, WARNING, "file fetcher: bad URL %s", url_str);
        ctx->ready = true;
        return true;
    }

    /* Detect MIME from extension */
    const char *dot = NULL;
    for (const char *p = path; *p; p++) if (*p == '.') dot = p;
    if (dot) ctx->mime = aether_mimetype_for_ext(dot + 1);

    /* Open file */
    long fd = sys_fs_open(path);
    if (fd < 0) {
        NSLOG(netsurf, WARNING, "file fetcher: open failed for %s", path);
        ctx->ready = true;
        return true;
    }

    /* Read entire file into heap buffer */
    size_t cap = 16384, used = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { sys_fs_close(fd); ctx->ready = true; return true; }

    while (1) {
        if (used + 4096 > cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }
        long n = sys_fs_read(fd, buf + used, (long)(cap - used - 1));
        if (n <= 0) break;
        used += (size_t)n;
    }
    sys_fs_close(fd);

    buf[used]    = '\0';
    ctx->body     = buf;
    ctx->body_len = used;
    ctx->ready    = true;

    return true;
}

static void fetch_file_abort(void *ctx_)
{
    fetch_file_ctx_t *ctx = ctx_;
    ctx->aborted = true;
}

static void fetch_file_free(void *ctx_)
{
    fetch_file_ctx_t *ctx = ctx_;

    fetch_file_ctx_t **pp = &g_list;
    while (*pp && *pp != ctx) pp = &(*pp)->next;
    if (*pp) *pp = ctx->next;

    nsurl_unref(ctx->url);
    free(ctx->body);
    free(ctx);
}

static void fetch_file_poll(lwc_string *scheme)
{
    (void)scheme;

    fetch_file_ctx_t *ctx = g_list;
    while (ctx) {
        fetch_file_ctx_t *next = ctx->next;

        if (ctx->ready && !ctx->delivered && !ctx->aborted) {
            ctx->delivered = true;

            if (!ctx->body) {
                /* File not found */
                fetch_set_http_code(ctx->parent, 404);
                fetch_msg msg = {0};
                msg.type       = FETCH_ERROR;
                msg.data.error = "File not found";
                fetch_send_callback(&msg, ctx->parent);
            } else {
                fetch_set_http_code(ctx->parent, 200);

                /* Content-Type header */
                char ctype_line[256];
                snprintf(ctype_line, sizeof(ctype_line),
                         "Content-Type: %s\r\n", ctx->mime);
                {
                    fetch_msg msg = {0};
                    msg.type = FETCH_HEADER;
                    msg.data.header_or_data.buf = (const uint8_t *)ctype_line;
                    msg.data.header_or_data.len = strlen(ctype_line);
                    fetch_send_callback(&msg, ctx->parent);
                }

                /* Content-Length header */
                char clen_line[64];
                snprintf(clen_line, sizeof(clen_line),
                         "Content-Length: %zu\r\n", ctx->body_len);
                {
                    fetch_msg msg = {0};
                    msg.type = FETCH_HEADER;
                    msg.data.header_or_data.buf = (const uint8_t *)clen_line;
                    msg.data.header_or_data.len = strlen(clen_line);
                    fetch_send_callback(&msg, ctx->parent);
                }

                /* Body */
                {
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
        }

        ctx = next;
    }
}

/* ── exported table ──────────────────────────────────────────────────────── */

static const struct fetcher_operation_table fetch_file_ops = {
    .initialise = fetch_file_initialise,
    .acceptable = fetch_file_acceptable,
    .setup      = fetch_file_setup,
    .start      = fetch_file_start,
    .abort      = fetch_file_abort,
    .free       = fetch_file_free,
    .poll       = fetch_file_poll,
    .fdset      = NULL,
    .finalise   = fetch_file_finalise,
};

/* Called by NetSurf's fetcher_init() during netsurf_init() */
nserror fetch_file_register(void)
{
    lwc_string *scheme = NULL;
    if (lwc_intern_string("file", 4, &scheme) != lwc_error_ok || !scheme) {
        NSLOG(netsurf, ERROR, "file fetcher: failed to intern scheme");
        return NSERROR_INIT_FAILED;
    }
    /* fetcher_add stores scheme; do NOT unref here — fetcher owns the ref */
    nserror err = fetcher_add(scheme, &fetch_file_ops);
    if (err != NSERROR_OK) {
        lwc_string_unref(scheme);
        NSLOG(netsurf, ERROR, "file fetcher: fetcher_add failed: %d", (int)err);
    }
    return err;
}
