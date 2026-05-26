/*
 * downloads_aether.c — I5.4 Download manager: save HTTP responses to /downloads/
 *
 * Two-step model:
 *   1. downloads_cache_response() — called by fetch_http_aether after each 200;
 *      copies the decoded body into a 2 MB in-memory buffer.
 *   2. download_save_page() — called by main.c on Ctrl+S; derives a filename
 *      from the cached URL + MIME type and writes the buffer to FAT32.
 *
 * Filename derivation: last URL path segment, sanitized, with extension from
 * MIME type appended if the segment has no recognized extension.
 * Collision handling: append _1, _2, … up to _99 before overwriting.
 */

#include "downloads_aether.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DOWNLOAD_CACHE_MAX  (2u * 1024u * 1024u)   /* 2 MB */
#define DOWNLOAD_DIR        "/downloads"

static uint8_t *g_dl_buf  = NULL;
static size_t   g_dl_len  = 0;
static char     g_dl_url[512]  = "";
static char     g_dl_mime[256] = "";

/* ── Cache ───────────────────────────────────────────────────────────────── */

void downloads_cache_response(const char *url, const uint8_t *body,
                               size_t body_len, const char *mime)
{
    free(g_dl_buf);
    g_dl_buf     = NULL;
    g_dl_len     = 0;
    g_dl_url[0]  = '\0';
    g_dl_mime[0] = '\0';

    if (!url || !body || body_len == 0 || body_len > DOWNLOAD_CACHE_MAX)
        return;

    g_dl_buf = malloc(body_len);
    if (!g_dl_buf) return;

    memcpy(g_dl_buf, body, body_len);
    g_dl_len = body_len;

    int ul = (int)strlen(url);
    if (ul >= (int)sizeof(g_dl_url)) ul = (int)sizeof(g_dl_url) - 1;
    memcpy(g_dl_url, url, (size_t)ul); g_dl_url[ul] = '\0';

    if (mime) {
        int ml = (int)strlen(mime);
        if (ml >= (int)sizeof(g_dl_mime)) ml = (int)sizeof(g_dl_mime) - 1;
        memcpy(g_dl_mime, mime, (size_t)ml); g_dl_mime[ml] = '\0';
    }
}

/* ── Filename helpers ────────────────────────────────────────────────────── */

static const char *mime_to_ext(const char *mime)
{
    if (!mime || !mime[0])                          return ".bin";
    if (strncasecmp(mime, "text/html",        9)  == 0) return ".html";
    if (strncasecmp(mime, "text/plain",       10) == 0) return ".txt";
    if (strncasecmp(mime, "application/json", 16) == 0) return ".json";
    if (strncasecmp(mime, "image/png",        9)  == 0) return ".png";
    if (strncasecmp(mime, "image/jpeg",       10) == 0) return ".jpg";
    if (strncasecmp(mime, "application/pdf",  15) == 0) return ".pdf";
    if (strncasecmp(mime, "text/xml",         8)  == 0) return ".xml";
    if (strncasecmp(mime, "application/xml",  15) == 0) return ".xml";
    return ".bin";
}

static int has_known_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    dot++;
    static const char *known[] = {
        "html","htm","txt","json","xml","css","js",
        "png","jpg","jpeg","gif","pdf","svg", NULL
    };
    for (int i = 0; known[i]; i++)
        if (strcasecmp(dot, known[i]) == 0) return 1;
    return 0;
}

/* Build a safe filename (no path separator) from URL + MIME. */
static void filename_from_url(const char *url, const char *mime,
                               char *out, int out_max)
{
    /* Strip scheme */
    const char *p = url;
    if      (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://",  7) == 0) p += 7;

    /* Isolate path portion (after host) */
    const char *path = strchr(p, '/');
    if (!path || !path[0]) path = "/";

    /* Copy path, strip query + fragment */
    char pbuf[256];
    int plen = (int)strlen(path);
    if (plen >= (int)sizeof(pbuf)) plen = (int)sizeof(pbuf) - 1;
    memcpy(pbuf, path, (size_t)plen); pbuf[plen] = '\0';
    char *q = strchr(pbuf, '?'); if (q) *q = '\0';
    char *f = strchr(pbuf, '#'); if (f) *f = '\0';

    /* Remove trailing slashes */
    int blen = (int)strlen(pbuf);
    while (blen > 1 && pbuf[blen-1] == '/') pbuf[--blen] = '\0';

    /* Last segment */
    char *ls = strrchr(pbuf, '/');
    const char *seg = (ls && ls[1]) ? ls + 1 : NULL;

    /* Fall back to hostname when path is empty or root */
    char host_buf[64] = "page";
    if (!seg || !seg[0]) {
        const char *h = url;
        if      (strncmp(h, "https://", 8) == 0) h += 8;
        else if (strncmp(h, "http://",  7) == 0) h += 7;
        int hi = 0;
        while (h[hi] && h[hi] != '/' && hi < (int)sizeof(host_buf) - 1) {
            char c = h[hi];
            host_buf[hi++] = (c == '.' || c == ':') ? '_' : c;
        }
        host_buf[hi] = '\0';
        seg = host_buf;
    }

    /* Sanitize segment → keep alphanum + dot + dash + underscore */
    int oi = 0;
    for (int i = 0; seg[i] && oi < out_max - 12; i++) {
        unsigned char c = (unsigned char)seg[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')
            out[oi++] = (char)c;
        else
            out[oi++] = '_';
    }
    out[oi] = '\0';
    if (!out[0]) { memcpy(out, "page", 4); out[4] = '\0'; }

    /* Append MIME-derived extension if no recognized one is present */
    if (!has_known_ext(out)) {
        const char *ext = mime_to_ext(mime);
        int cur = (int)strlen(out);
        int el  = (int)strlen(ext);
        if (cur + el < out_max - 1) {
            memcpy(out + cur, ext, (size_t)el);
            out[cur + el] = '\0';
        }
    }
}

/* Find a non-existing path under DOWNLOAD_DIR for base filename. */
static void unique_download_path(const char *base, char *out, int out_max)
{
    snprintf(out, (size_t)out_max, "%s/%s", DOWNLOAD_DIR, base);
    FILE *fp = fopen(out, "r");
    if (!fp) return;      /* doesn't exist — use as-is */
    fclose(fp);

    /* Split base into stem + ext */
    char stem[64] = "", extb[16] = "";
    const char *dot = strrchr(base, '.');
    if (dot) {
        int sl = (int)(dot - base);
        if (sl > 63) sl = 63;
        memcpy(stem, base, (size_t)sl); stem[sl] = '\0';
        int el = (int)strlen(dot);
        if (el > 15) el = 15;
        memcpy(extb, dot, (size_t)el); extb[el] = '\0';
    } else {
        int sl = (int)strlen(base);
        if (sl > 63) sl = 63;
        memcpy(stem, base, (size_t)sl); stem[sl] = '\0';
    }

    for (int i = 1; i <= 99; i++) {
        snprintf(out, (size_t)out_max, "%s/%s_%d%s", DOWNLOAD_DIR, stem, i, extb);
        fp = fopen(out, "r");
        if (!fp) return;
        fclose(fp);
    }
    /* Give up — overwrite original */
    snprintf(out, (size_t)out_max, "%s/%s", DOWNLOAD_DIR, base);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int download_save_page(char *out_path, int out_max)
{
    if (!g_dl_buf || g_dl_len == 0 || !g_dl_url[0]) return 0;

    char fname[80];
    filename_from_url(g_dl_url, g_dl_mime, fname, (int)sizeof(fname));

    char fpath[128];
    unique_download_path(fname, fpath, (int)sizeof(fpath));

    FILE *fp = fopen(fpath, "w");
    if (!fp) return 0;

    size_t written = fwrite(g_dl_buf, 1, g_dl_len, fp);
    fclose(fp);

    if (written != g_dl_len) return 0;

    if (out_path && out_max > 0) {
        int pl = (int)strlen(fpath);
        if (pl >= out_max) pl = out_max - 1;
        memcpy(out_path, fpath, (size_t)pl);
        out_path[pl] = '\0';
    }
    return 1;
}
