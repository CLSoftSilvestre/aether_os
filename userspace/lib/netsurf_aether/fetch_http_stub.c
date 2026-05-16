/*
 * Phase 7.5 — NetSurf http:// fetcher stub
 *
 * This is a placeholder fetcher for the "http" and "https" schemes.
 * It accepts the registration so NetSurf doesn't crash during init, but
 * returns an error for every actual request.
 *
 * Phase 7.6 replaces the internals with a real implementation over
 * AetherOS socket syscalls 700-707.
 *
 * TODO-7.6: implement fetch_http_setup/start/poll using sys_socket(),
 *           sys_connect(), sys_send(), sys_recv() from sys.h
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

#include "content/fetch.h"
#include "utils/errors.h"
#include "utils/nsurl.h"
#include "utils/log.h"
#include "netsurf_aether.h"

/* ── stub context ────────────────────────────────────────────────────────── */

typedef struct fetch_http_ctx {
    nsurl  *url;
    bool    done;
} fetch_http_ctx_t;

static void *fetch_http_setup(struct fetch *pf,
                              nsurl         *url,
                              bool           only_2xx,
                              bool           downgrade_tls,
                              const char    *post_urlenc,
                              struct fetch_multipart_data *post_mp,
                              const char   **headers)
{
    (void)pf; (void)only_2xx; (void)downgrade_tls;
    (void)post_urlenc; (void)post_mp; (void)headers;

    fetch_http_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->url = nsurl_ref(url);
    return ctx;
}

static bool fetch_http_start(void *ctx_) { (void)ctx_; return true; }

static void fetch_http_abort(void *ctx_)
{
    fetch_http_ctx_t *ctx = ctx_;
    ctx->done = true;
}

static void fetch_http_free(void *ctx_)
{
    fetch_http_ctx_t *ctx = ctx_;
    nsurl_unref(ctx->url);
    free(ctx);
}

static void fetch_http_poll(lwc_string *scheme_)
{
    (void)scheme_;
    /* TODO-7.6: drain in-progress HTTP socket state machines */
}

/* ── registration ────────────────────────────────────────────────────────── */

static struct fetcher_operation_table fetch_http_ops = {
    .initialise = NULL,
    .acceptable = NULL,
    .setup      = fetch_http_setup,
    .start      = fetch_http_start,
    .abort      = fetch_http_abort,
    .free       = fetch_http_free,
    .poll       = fetch_http_poll,
    .fdset      = NULL,
    .query      = NULL,
};

void fetch_http_stub_register(void)
{
    /* Register stub for http and https — real impl comes in Phase 7.6 */
    fetcher_add("http",  &fetch_http_ops);
    fetcher_add("https", &fetch_http_ops);
}
