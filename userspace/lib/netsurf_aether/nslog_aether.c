/*
 * Phase 7.5.5 — nslog → UART bridge (corrected for actual libnslog 0.1.3 API)
 *
 * nslog_callback receives: (void *context, nslog_entry_context_t *ctx, fmt, args)
 * Field names in nslog_entry_context_t: filename/lineno (not file/line).
 * Level filtering uses nslog_filter_level_new() + nslog_filter_set_active().
 * Must call nslog_uncork() after registering to drain any queued messages.
 */

#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "nslog/nslog.h"
#include "netsurf_aether.h"
#include "sys.h"

/* ── UART write ──────────────────────────────────────────────────────────── */

static void uart_write(const char *s, int len)
{
    long r;
    __asm__ volatile(
        "mov x8, #34\n"
        "mov x0, #1\n"
        "mov x1, %1\n"
        "mov x2, %2\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(r) : "r"(s), "r"((long)len) : "x0","x1","x2","x8","memory"
    );
    (void)r;
}

/* ── log callback (matches nslog_callback typedef exactly) ───────────────── */

static void aether_log_cb(void                    *context,
                           nslog_entry_context_t  *ctx,
                           const char             *fmt,
                           va_list                 args)
{
    (void)context;

    char buf[512];
    int  pos = 0;
    int  rem = (int)sizeof(buf) - 2; /* reserve \n\0 */

    /* Level short name (4 chars, e.g. "INFO", "WARN") */
    const char *lvl = nslog_short_level_name(ctx->level);
    pos += snprintf(buf + pos, rem - pos, "[%s] ", lvl ? lvl : "????");

    /* Category */
    if (ctx->category && ctx->category->name)
        pos += snprintf(buf + pos, rem - pos, "%s ", ctx->category->name);

    /* Basename of source file */
    if (ctx->filename) {
        const char *base = ctx->filename;
        for (const char *p = base; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        pos += snprintf(buf + pos, rem - pos, "%s:%d ", base, ctx->lineno);
    }

    /* Message */
    pos += vsnprintf(buf + pos, rem - pos, fmt, args);

    buf[pos++] = '\n';
    uart_write(buf, pos);
}

/* ── public init ─────────────────────────────────────────────────────────── */

void nslog_aether_init(void)
{
    /* Register our UART callback (context = NULL, not used) */
    nslog_set_render_callback(aether_log_cb, NULL);

    /* Show INFO and above — use the filter API */
    nslog_filter_t *f = NULL;
    if (nslog_filter_level_new(NSLOG_LEVEL_INFO, &f) == NSLOG_NO_ERROR && f) {
        nslog_filter_set_active(f, NULL);
        nslog_filter_unref(f);
    }

    /* Release any messages that were queued before we registered */
    nslog_uncork();

    uart_write("[nslog] AetherOS UART logging active\n", 37);
}
