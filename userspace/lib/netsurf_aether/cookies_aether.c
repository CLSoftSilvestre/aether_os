/*
 * cookies_aether.c — In-memory cookie store with FAT32 persistence.
 *
 * Storage: /config/cookies.txt — "domain|path|name|value\n" per line.
 * Domain matching: host ends-with stored domain (leading dot stripped).
 * Path matching:   request path starts-with cookie path.
 * Attributes parsed: domain=, path= (expires/max-age/secure/httponly ignored).
 */

#include "cookies_aether.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef struct {
    char domain[256];
    char path[128];
    char name[128];
    char value[512];
} cookie_t;

static cookie_t g_cookies[COOKIE_MAX];
static int      g_cookie_count = 0;

/* ── Persistence ────────────────────────────────────────────────────────── */

static void cookies_save(void)
{
    FILE *f = fopen(COOKIE_FILE, "w");
    if (!f) return;
    for (int i = 0; i < g_cookie_count; i++) {
        fputs(g_cookies[i].domain, f); fputc('|', f);
        fputs(g_cookies[i].path,   f); fputc('|', f);
        fputs(g_cookies[i].name,   f); fputc('|', f);
        fputs(g_cookies[i].value,  f); fputc('\n', f);
    }
    fclose(f);
}

void cookies_init(void)
{
    FILE *f = fopen(COOKIE_FILE, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f) && g_cookie_count < COOKIE_MAX) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (!len) continue;

        /* Format: domain|path|name|value */
        char *p1 = strchr(line, '|'); if (!p1) continue; *p1 = '\0';
        char *p2 = strchr(p1+1, '|'); if (!p2) continue; *p2 = '\0';
        char *p3 = strchr(p2+1, '|'); if (!p3) continue; *p3 = '\0';

        cookie_t *c = &g_cookies[g_cookie_count];

#define COPY_FIELD(dst, src) do { \
    int n = (int)strlen(src); \
    if (n >= (int)sizeof(dst)) n = (int)sizeof(dst) - 1; \
    memcpy(dst, src, (size_t)n); dst[n] = '\0'; } while (0)

        COPY_FIELD(c->domain, line);
        COPY_FIELD(c->path,   p1+1);
        COPY_FIELD(c->name,   p2+1);
        COPY_FIELD(c->value,  p3+1);

#undef COPY_FIELD

        g_cookie_count++;
    }
    fclose(f);
}

/* ── Matching ────────────────────────────────────────────────────────────── */

static int domain_matches(const char *host, const char *domain)
{
    const char *d = domain;
    if (*d == '.') d++;                /* strip leading dot */
    size_t hlen = strlen(host);
    size_t dlen = strlen(d);
    if (!dlen) return 0;
    if (hlen == dlen) return (strcmp(host, d) == 0);
    /* host ends with ".domain"? */
    if (hlen > dlen && host[hlen - dlen - 1] == '.')
        return (strcmp(host + hlen - dlen, d) == 0);
    return 0;
}

static int path_matches(const char *req_path, const char *cookie_path)
{
    size_t clen = strlen(cookie_path);
    if (clen == 0 || (clen == 1 && cookie_path[0] == '/')) return 1;
    if (strncmp(req_path, cookie_path, clen) != 0) return 0;
    char next = req_path[clen];
    return (next == '/' || next == '\0' || next == '?');
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void cookie_set_from_header(const char *header_val,
                             const char *req_host,
                             const char *req_path)
{
    if (!header_val || !header_val[0]) return;

    char work[1024];
    int wlen = (int)strlen(header_val);
    if (wlen >= (int)sizeof(work)) wlen = (int)sizeof(work) - 1;
    memcpy(work, header_val, (size_t)wlen);
    work[wlen] = '\0';

    char name[128] = "", value[512] = "";
    char domain[256] = "", path[128] = "/";

    /* First token before first ';' is "name=value" */
    char *tok = work;
    char *sc  = strchr(tok, ';');
    if (sc) { *sc = '\0'; sc++; }

    char *eq = strchr(tok, '=');
    if (!eq) return;
    *eq = '\0';

    /* name */
    while (*tok == ' ') tok++;
    int nl = (int)strlen(tok);
    if (nl <= 0 || nl >= (int)sizeof(name)) return;
    memcpy(name, tok, (size_t)nl); name[nl] = '\0';

    /* value */
    const char *vp = eq + 1;
    while (*vp == ' ') vp++;
    int vl = (int)strlen(vp);
    if (vl >= (int)sizeof(value)) vl = (int)sizeof(value) - 1;
    memcpy(value, vp, (size_t)vl); value[vl] = '\0';

    /* Attributes */
    while (sc && *sc) {
        while (*sc == ' ') sc++;
        char *next_sc = strchr(sc, ';');
        if (next_sc) { *next_sc = '\0'; next_sc++; }

        if (strncasecmp(sc, "domain=", 7) == 0) {
            sc += 7;
            while (*sc == ' ') sc++;
            int dl = (int)strlen(sc);
            if (dl >= (int)sizeof(domain)) dl = (int)sizeof(domain) - 1;
            memcpy(domain, sc, (size_t)dl); domain[dl] = '\0';
        } else if (strncasecmp(sc, "path=", 5) == 0) {
            sc += 5;
            while (*sc == ' ') sc++;
            int pl = (int)strlen(sc);
            if (pl >= (int)sizeof(path)) pl = (int)sizeof(path) - 1;
            memcpy(path, sc, (size_t)pl); path[pl] = '\0';
        }
        /* Ignore expires, max-age, secure, httponly, samesite */
        sc = next_sc;
    }

    /* Default domain = request host (host-only, no leading dot) */
    if (!domain[0]) {
        int dl = (int)strlen(req_host);
        if (dl >= (int)sizeof(domain)) dl = (int)sizeof(domain) - 1;
        memcpy(domain, req_host, (size_t)dl); domain[dl] = '\0';
    }

    /* Default path = directory of request path */
    if (!path[0] || strcmp(path, "/") == 0) {
        path[0] = '/'; path[1] = '\0';
    }

    if (!name[0]) return;

    /* Update existing cookie with same domain + name */
    for (int i = 0; i < g_cookie_count; i++) {
        if (strcmp(g_cookies[i].name, name) == 0 &&
            strcmp(g_cookies[i].domain, domain) == 0) {
            int vl2 = (int)strlen(value);
            if (vl2 >= (int)sizeof(g_cookies[i].value))
                vl2 = (int)sizeof(g_cookies[i].value) - 1;
            memcpy(g_cookies[i].value, value, (size_t)vl2);
            g_cookies[i].value[vl2] = '\0';
            cookies_save();
            return;
        }
    }

    /* Add new cookie */
    if (g_cookie_count >= COOKIE_MAX) return;

    cookie_t *c = &g_cookies[g_cookie_count++];

#define COPY_FIELD(dst, src) do { \
    int n = (int)strlen(src); \
    if (n >= (int)sizeof(dst)) n = (int)sizeof(dst) - 1; \
    memcpy(dst, src, (size_t)n); dst[n] = '\0'; } while (0)

    COPY_FIELD(c->domain, domain);
    COPY_FIELD(c->path,   path);
    COPY_FIELD(c->name,   name);
    COPY_FIELD(c->value,  value);

#undef COPY_FIELD

    cookies_save();
}

void cookies_build_header(const char *host, const char *path,
                           char *out, size_t out_max)
{
    if (!host || !path || !out || out_max == 0) return;
    out[0] = '\0';
    if (out_max < 2) return;

    size_t pos = 0;
    for (int i = 0; i < g_cookie_count; i++) {
        if (!domain_matches(host, g_cookies[i].domain)) continue;
        if (!path_matches(path, g_cookies[i].path))     continue;

        size_t nlen = strlen(g_cookies[i].name);
        size_t vlen = strlen(g_cookies[i].value);
        size_t sep  = (pos > 0) ? 2 : 0;           /* "; " prefix */
        size_t need = sep + nlen + 1 + vlen;        /* sep + name + '=' + value */

        if (pos + need + 1 > out_max) break;

        if (pos > 0) { out[pos++] = ';'; out[pos++] = ' '; }
        memcpy(out + pos, g_cookies[i].name,  nlen); pos += nlen;
        out[pos++] = '=';
        memcpy(out + pos, g_cookies[i].value, vlen); pos += vlen;
        out[pos] = '\0';
    }
}
