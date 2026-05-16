/*
 * Phase 7.5 — gui_fetch_table: AetherOS implementation
 *
 * Corrected for actual netsurf/fetch.h API:
 *   - Mandatory field is "filetype" (extension → MIME), not "mimetype"
 *   - get_resource_url() returns struct nsurl * (via nsurl_create), not char *
 *
 * Internal resources live at /resources/ on the AetherOS FAT32 disk.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "utils/errors.h"
#include "netsurf/fetch.h"
#include "utils/nsurl.h"
#include "netsurf_aether.h"

/* ── extension → MIME table ──────────────────────────────────────────────── */

static const struct { const char *ext; const char *mime; } g_mime[] = {
    { "html", "text/html"              },
    { "htm",  "text/html"              },
    { "css",  "text/css"               },
    { "js",   "application/javascript" },
    { "png",  "image/png"              },
    { "jpg",  "image/jpeg"             },
    { "jpeg", "image/jpeg"             },
    { "gif",  "image/gif"              },
    { "bmp",  "image/bmp"              },
    { "ico",  "image/x-icon"           },
    { "svg",  "image/svg+xml"          },
    { "txt",  "text/plain"             },
    { "xml",  "text/xml"               },
    { "json", "application/json"       },
    { NULL,   NULL                     },
};

/* Mandatory: const char *filetype(const char *unix_path) */
static const char *aether_filetype(const char *path)
{
    if (!path) return "application/octet-stream";

    const char *dot = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '.') dot = p;
    if (!dot) return "application/octet-stream";
    dot++;

    for (int i = 0; g_mime[i].ext; i++) {
        const char *a = dot, *b = g_mime[i].ext;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
            if (ca != *b) break;
            a++; b++;
        }
        if (!*a && !*b) return g_mime[i].mime;
    }
    return "application/octet-stream";
}

/* ── embedded default.css ────────────────────────────────────────────────── */
/*
 * NetSurf requires resource:default.css to create a CSS selection context.
 * We embed the UA stylesheet so it works without a filesystem.
 */
static const uint8_t aether_default_css_data[] =
    "html { display: block; }\n"
    "head { display: none; }\n"
    "body { display: block; margin: 8px; line-height: 1.33; }\n"
    "div { display: block; }\n"
    "h1 { display: block; font-size: 2em; font-weight: bold; margin: .67em 0; }\n"
    "h2 { display: block; font-size: 1.5em; font-weight: bold; margin: .69em 0; }\n"
    "h3 { display: block; font-size: 1.17em; font-weight: bold; margin: .83em 0; }\n"
    "h4 { display: block; font-weight: bold; margin: 1.12em 0; }\n"
    "h5 { display: block; font-size: .83em; font-weight: bold; margin: 1.5em 0; }\n"
    "h6 { display: block; font-size: .75em; font-weight: bold; margin: 1.67em 0; }\n"
    "address { display: block; font-style: italic; }\n"
    "em { font-style: italic; }\n"
    "strong { font-weight: bold; }\n"
    "code, samp, kbd { font-family: monospace; }\n"
    "blockquote { display: block; margin: 1.12em 40px; }\n"
    "p { display: block; margin: 1.12em 0; }\n"
    "pre { display: block; font-family: monospace; white-space: pre; margin-bottom: 1em; }\n"
    "ul { display: block; padding-left: 40px; margin: 1.12em 0; list-style-type: disc; }\n"
    "ol { display: block; padding-left: 40px; margin: 1.12em 0; list-style-type: decimal; }\n"
    "li { display: list-item; }\n"
    "dl { display: block; padding-left: 1.5em; margin: 1em; }\n"
    "dt { display: block; font-weight: bold; }\n"
    "dd { display: block; padding-left: 1em; margin-bottom: 0.3em; }\n"
    "table { display: table; border-spacing: 2px; }\n"
    "caption { display: table-caption; }\n"
    "thead { display: table-header-group; vertical-align: middle; }\n"
    "tfoot { display: table-footer-group; vertical-align: middle; }\n"
    "tbody { display: table-row-group; vertical-align: middle; }\n"
    "colgroup { display: table-column-group; }\n"
    "col { display: table-column; }\n"
    "tr { display: table-row; vertical-align: inherit; }\n"
    "td, th { display: table-cell; vertical-align: inherit; padding: 1px; }\n"
    "th { font-weight: bold; text-align: center; }\n"
    "a:link { color: #00f; text-decoration: underline; }\n"
    "a:visited { color: #609; }\n"
    "img { color: #888; }\n"
    "center { display: block; }\n"
    "tt, i { font-style: italic; }\n"
    "b { font-weight: bold; }\n"
    "hr { display: block; margin: 0.5em auto; border: 1px inset #888; }\n"
    "form { display: block; }\n"
    "noembed, script, style, title { display: none; }\n"
    "article, aside, figcaption, figure, footer, header, main, nav, section { display: block; }\n"
    ;

/* ── resource data (served in-memory, no filesystem required) ────────────── */

static nserror aether_get_resource_data(const char *path,
                                         const uint8_t **data,
                                         size_t *data_len)
{
    if (path && strcmp(path, "default.css") == 0) {
        *data     = aether_default_css_data;
        *data_len = sizeof(aether_default_css_data) - 1; /* exclude NUL */
        return NSERROR_OK;
    }
    return NSERROR_NOT_FOUND;
}

static nserror aether_release_resource_data(const uint8_t *data)
{
    (void)data; /* static data — nothing to free */
    return NSERROR_OK;
}

/* ── resource URL mapping (fallback for non-embedded resources) ──────────── */

static struct nsurl *aether_get_resource_url(const char *path)
{
    static const char base[] = "file:///resources/";
    size_t blen = sizeof(base) - 1;
    size_t plen = path ? strlen(path) : 0;

    char *url_str = malloc(blen + plen + 1);
    if (!url_str) return NULL;

    memcpy(url_str, base, blen);
    if (path) memcpy(url_str + blen, path, plen);
    url_str[blen + plen] = '\0';

    struct nsurl *url = NULL;
    nserror err = nsurl_create(url_str, &url);
    free(url_str);

    return (err == NSERROR_OK) ? url : NULL;
}

/* ── exported table ──────────────────────────────────────────────────────── */

struct gui_fetch_table aether_fetch_table = {
    .filetype              = aether_filetype,
    .get_resource_url      = aether_get_resource_url,
    .get_resource_data     = aether_get_resource_data,
    .release_resource_data = aether_release_resource_data,
};
