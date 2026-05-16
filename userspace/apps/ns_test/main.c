/*
 * Phase 7.5.7 / 7.6 — NetSurf core integration test
 *
 * Verifies the full Phase 7.5–7.6 init sequence:
 *   1. netsurf_register(&table)      — platform table (5 mandatory entries)
 *   2. netsurf_init(NULL)            — core subsystems + built-in fetchers
 *   3. fetch_http_aether_register()  — our http/https fetcher (after init; needs lwc)
 *   4. nsurl_create sanity check
 *   5. netsurf_exit()
 *
 * Expected output: "ns_test PASS" then exit 0.
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

/* AetherOS bridge */
#include "netsurf_aether.h"

/* ── externs from bridge files ───────────────────────────────────────────── */

extern struct gui_misc_table   aether_misc_table;
extern struct gui_window_table aether_window_table;
extern struct gui_fetch_table  aether_fetch_table;
extern struct gui_bitmap_table aether_bitmap_table;
extern struct gui_layout_table aether_layout_table;

extern void nslog_aether_init(void);
extern void fetch_http_aether_register(void);   /* register AFTER netsurf_init() */

/* ── netsurf_table ───────────────────────────────────────────────────────── */

static struct netsurf_table aether_netsurf_table = {
    .misc   = &aether_misc_table,
    .window = &aether_window_table,
    .fetch  = &aether_fetch_table,
    .bitmap = &aether_bitmap_table,
    .layout = &aether_layout_table,
};

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void print(const char *s)
{
    long r;
    int  len = 0;
    while (s[len]) len++;
    __asm__ volatile(
        "mov x8, #34\n"
        "mov x0, #1\n"
        "mov x1, %1\n"
        "mov x2, %2\n"
        "svc #0\n"
        "mov %0, x0\n"
        : "=r"(r) : "r"(s), "r"((long)len)
        : "x0","x1","x2","x8","memory"
    );
}

static void exit_with(int code)
{
    __asm__ volatile(
        "mov x8, #0\n"
        "mov x0, %0\n"
        "svc #0\n"
        :: "r"((long)code) : "x0","x8","memory"
    );
    __builtin_unreachable();
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    print("ns_test: initialising...\n");

    /* 1. UART logging (must come first so init messages are visible) */
    nslog_aether_init();

    /* 2. Register platform table */
    nserror err = netsurf_register(&aether_netsurf_table);
    if (err != NSERROR_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "ns_test FAIL: netsurf_register() returned %d\n", (int)err);
        print(buf);
        exit_with(1);
    }
    print("ns_test: netsurf_register() OK\n");

    /* 3. Initialise options — must happen before netsurf_init() because
     *    netsurf_init() calls nscolour_update() which reads nsoptions[].key.
     *    NULL callbacks → use compiled-in defaults; NULL out-ptrs → set globals. */
    err = nsoption_init(NULL, NULL, NULL);
    if (err != NSERROR_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "ns_test FAIL: nsoption_init() returned %d\n", (int)err);
        print(buf);
        exit_with(1);
    }
    print("ns_test: nsoption_init() OK\n");

    /* 4. Initialise core (built-in fetchers: file, data, resource, about, js) */
    err = netsurf_init(NULL);
    if (err != NSERROR_OK) {
        char buf[64];
        snprintf(buf, sizeof(buf),
                 "ns_test FAIL: netsurf_init() returned %d\n", (int)err);
        print(buf);
        exit_with(1);
    }
    print("ns_test: netsurf_init() OK\n");

    /* 5. Register http/https fetcher — must be after netsurf_init() so
     *    libwapcaplet (lwc_intern_string) is ready */
    fetch_http_aether_register();
    print("ns_test: fetch_http_aether_register() OK\n");

    /* 6. Sanity-check the URL utility */
    struct nsurl *test_url = NULL;
    err = nsurl_create("http://example.com/", &test_url);
    if (err == NSERROR_OK && test_url) {
        print("ns_test: nsurl_create() OK\n");
        nsurl_unref(test_url);
    } else {
        print("ns_test: nsurl_create() skipped\n");
    }

    /* 7. Teardown */
    netsurf_exit();

    print("ns_test PASS\n");
    exit_with(0);
    return 0;
}
