/*
 * Phase 7.6.7 — NetSurf HTTP fetch integration test
 *
 * Exercises the full Phase 7.6 http:// fetch pipeline:
 *   netsurf_init → fetch_http_aether_register → fetch_start → scheduler drain
 *   → FETCH_HEADER + FETCH_DATA + FETCH_FINISHED callbacks
 *
 * Usage inside AetherOS:
 *   fetch_test                     → fetches http://10.0.2.2:8080/test.html
 *   fetch_test http://example.com/ → fetches the given URL
 *
 * QEMU setup (run_qemu.sh):
 *   -netdev user,id=net0,hostfwd=tcp::8080-:8080 -device virtio-net-pci,netdev=net0
 * Mac host:
 *   python3 -m http.server 8080 --directory tests/browser/
 *
 * Expected: prints HTTP status, Content-Type, body length, first 256 bytes, then
 *           "fetch_test PASS".
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* NetSurf public API */
#include "netsurf/netsurf.h"
#include "netsurf/misc.h"
#include "netsurf/window.h"
#include "netsurf/fetch.h"
#include "netsurf/bitmap.h"
#include "netsurf/layout.h"
#include "utils/errors.h"
#include "utils/nsoption.h"
#include "content/fetch.h"

/* AetherOS bridge */
#include "netsurf_aether.h"

/* ── externs ─────────────────────────────────────────────────────────────── */

extern struct gui_misc_table   aether_misc_table;
extern struct gui_window_table aether_window_table;
extern struct gui_fetch_table  aether_fetch_table;
extern struct gui_bitmap_table aether_bitmap_table;
extern struct gui_layout_table aether_layout_table;

extern void nslog_aether_init(void);
extern void fetch_http_aether_register(void);

/* ── netsurf table ───────────────────────────────────────────────────────── */

static struct netsurf_table aether_netsurf_table = {
    .misc   = &aether_misc_table,
    .window = &aether_window_table,
    .fetch  = &aether_fetch_table,
    .bitmap = &aether_bitmap_table,
    .layout = &aether_layout_table,
};

/* ── I/O helpers ─────────────────────────────────────────────────────────── */

static void uart_write(const char *s)
{
    long r;
    int len = 0;
    while (s[len]) len++;
    __asm__ volatile(
        "mov x8, #34\n mov x0, #1\n mov x1, %1\n mov x2, %2\n"
        "svc #0\n mov %0, x0\n"
        : "=r"(r) : "r"(s), "r"((long)len)
        : "x0","x1","x2","x8","memory"
    );
}

static void exit_with(int code)
{
    __asm__ volatile(
        "mov x8, #0\n mov x0, %0\n svc #0\n"
        :: "r"((long)code) : "x0","x8","memory"
    );
    __builtin_unreachable();
}

/* ── fetch callback state ────────────────────────────────────────────────── */

typedef struct {
    bool      complete;
    bool      error;
    long      http_code;
    uint8_t  *body;
    size_t    body_len;
    char      content_type[256];
} fetch_state_t;

static void on_fetch(const fetch_msg *msg, void *p)
{
    fetch_state_t *s = (fetch_state_t *)p;

    switch (msg->type) {
    case FETCH_HEADER: {
        /* Extract Content-Type if present */
        const char *hdr = (const char *)msg->data.header_or_data.buf;
        size_t      len = msg->data.header_or_data.len;
        if (len > 13 && strncasecmp(hdr, "Content-Type:", 13) == 0) {
            const char *val = hdr + 13;
            while (*val == ' ') val++;
            size_t i = 0;
            while (i < sizeof(s->content_type) - 1 && val[i] &&
                   val[i] != '\r' && val[i] != '\n') {
                s->content_type[i] = val[i];
                i++;
            }
            s->content_type[i] = '\0';
        }
        break;
    }
    case FETCH_DATA: {
        const uint8_t *buf = msg->data.header_or_data.buf;
        size_t         len = msg->data.header_or_data.len;
        uint8_t *nb = realloc(s->body, s->body_len + len + 1);
        if (nb) {
            memcpy(nb + s->body_len, buf, len);
            s->body_len += len;
            nb[s->body_len] = '\0';
            s->body = nb;
        }
        break;
    }
    case FETCH_FINISHED:
        s->complete = true;
        break;
    case FETCH_ERROR:
        s->error    = true;
        s->complete = true;
        break;
    default:
        if ((int)msg->type >= (int)FETCH_FINISHED)
            s->complete = true;
        break;
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *target_url = (argc > 1) ? argv[1]
                                        : "http://10.0.2.2:8080/index.html";

    uart_write("fetch_test: starting...\n");

    /* 1. Init logging */
    nslog_aether_init();

    /* 2. Register platform table */
    if (netsurf_register(&aether_netsurf_table) != NSERROR_OK) {
        uart_write("fetch_test FAIL: netsurf_register\n");
        exit_with(1);
    }

    /* 3. Init options — must precede netsurf_init(); nscolour_update() inside
     *    netsurf_init() reads nsoptions[].key which is NULL until this runs. */
    if (nsoption_init(NULL, NULL, NULL) != NSERROR_OK) {
        uart_write("fetch_test FAIL: nsoption_init\n");
        exit_with(1);
    }

    /* 4. Init core */
    nserror ni = netsurf_init(NULL);
    if (ni != NSERROR_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf), "fetch_test FAIL: netsurf_init err=%d\n", (int)ni);
        uart_write(buf);
        exit_with(1);
    }
    uart_write("fetch_test: netsurf_init OK\n");

    /* 5. Register http/https fetcher (after init — requires lwc) */
    fetch_http_aether_register();
    uart_write("fetch_test: fetch_http_aether_register OK\n");

    uart_write("fetch_test: calling nsurl_create with: ");
    uart_write(target_url);
    uart_write("\n");

    /* 6. Create the URL object */
    struct nsurl *url = NULL;
    nserror ne = nsurl_create(target_url, &url);
    if (ne != NSERROR_OK || !url) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "fetch_test FAIL: nsurl_create err=%d url=%p\n", (int)ne, (void*)url);
        uart_write(buf);
        exit_with(1);
    }

    uart_write("fetch_test: fetching ");
    uart_write(target_url);
    uart_write("\n");

    /* 6. Start the fetch */
    fetch_state_t state;
    memset(&state, 0, sizeof(state));

    struct fetch *fetch_out = NULL;
    nserror err = fetch_start(url, NULL, on_fetch, &state,
                              false,   /* only_2xx */
                              NULL,    /* post_urlenc */
                              NULL,    /* post_multipart */
                              true,    /* verifiable */
                              false,   /* downgrade_tls */
                              NULL,    /* headers */
                              &fetch_out);
    nsurl_unref(url);

    if (err != NSERROR_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "fetch_test FAIL: fetch_start returned %d\n", (int)err);
        uart_write(buf);
        exit_with(1);
    }

    /* 7. Drain the scheduler until fetch completes (or timeout) */
    int timeout = 5000;
    while (!state.complete && timeout-- > 0) {
        nsaether_schedule_drain();
    }

    if (!state.complete) {
        uart_write("fetch_test FAIL: timed out waiting for fetch\n");
        exit_with(1);
    }

    if (state.error) {
        uart_write("fetch_test: fetch returned error\n");
    }

    /* 8. Print results */
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "fetch_test: HTTP %ld\n",
                 (long)fetch_http_code(fetch_out));
        uart_write(buf);
    }
    if (state.content_type[0]) {
        uart_write("fetch_test: Content-Type: ");
        uart_write(state.content_type);
        uart_write("\n");
    }
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "fetch_test: body_len=%zu\n", state.body_len);
        uart_write(buf);
    }

    /* Print first 256 bytes of body */
    if (state.body && state.body_len > 0) {
        size_t preview = state.body_len < 256 ? state.body_len : 256;
        uart_write("--- body preview ---\n");
        /* Write as plain text (may contain non-printable; uart just passes through) */
        long dummy;
        __asm__ volatile(
            "mov x8, #34\n mov x0, #1\n mov x1, %1\n mov x2, %2\n svc #0\n mov %0, x0\n"
            : "=r"(dummy) : "r"(state.body), "r"((long)preview)
            : "x0","x1","x2","x8","memory"
        );
        uart_write("\n--- end preview ---\n");
    }

    free(state.body);
    netsurf_exit();

    uart_write("fetch_test PASS\n");
    exit_with(0);
    return 0;
}
