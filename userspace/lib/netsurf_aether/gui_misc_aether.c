/*
 * Phase 7.7 — gui_misc_table: AetherOS implementation
 *
 * Cooperative single-threaded scheduler using gettimeofday() for timing,
 * with cancel-before-add uniqueness — identical to the monkey/framebuffer
 * frontend pattern.  nsaether_schedule_drain() is called once per
 * event-loop iteration from the browser app; after each fired callback the
 * scan restarts from the head so that any newly-added entries are seen.
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "utils/errors.h"
#include "utils/sys_time.h"
#include "netsurf/misc.h"
#include "utils/log.h"
#include "netsurf_aether.h"

/* ── linked-list scheduler ───────────────────────────────────────────────── */

struct nscallback {
    struct timeval     tv;   /* absolute wall-clock time to fire */
    void             (*cb)(void *p);
    void              *p;
    struct nscallback *next;
};

static struct nscallback *g_sched_head = NULL;

static void schedule_remove(void (*cb)(void *p), void *p)
{
    struct nscallback **pp = &g_sched_head;
    while (*pp) {
        struct nscallback *e = *pp;
        if (e->cb == cb && e->p == p) {
            *pp = e->next;
            free(e);
        } else {
            pp = &e->next;
        }
    }
}

static nserror aether_schedule(int t, void (*callback)(void *p), void *p)
{
    /* t < 0 → cancel all pending entries for (callback, p) */
    if (t < 0) {
        schedule_remove(callback, p);
        return NSERROR_OK;
    }

    /* Uniqueness: remove any existing entry before adding the new one */
    schedule_remove(callback, p);

    struct nscallback *entry = malloc(sizeof(*entry));
    if (!entry) return NSERROR_NOMEM;

    struct timeval now, delay;
    gettimeofday(&now, NULL);
    delay.tv_sec  = (time_t)(t / 1000);
    delay.tv_usec = (long)((t % 1000) * 1000);
    timeradd(&now, &delay, &entry->tv);

    entry->cb   = callback;
    entry->p    = p;
    entry->next = g_sched_head;
    g_sched_head = entry;

    return NSERROR_OK;
}

static void sched_dbg(const char *s)
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

/* Call once per event-loop tick from the browser app */
void nsaether_schedule_drain(void)
{
    static unsigned s_fired = 0;
    struct timeval now;
    struct nscallback **pp;
    struct nscallback *e;

restart:
    gettimeofday(&now, NULL);
    pp = &g_sched_head;
    while (*pp) {
        e = *pp;
        if (!timercmp(&e->tv, &now, >)) {
            /* Unlink before firing so the callback can reschedule itself */
            *pp = e->next;
            void (*cb)(void *p) = e->cb;
            void *cbp = e->p;
            free(e);
            s_fired++;
            if (s_fired <= 30) {   /* first 30 only — avoid flood */
                char buf[32];
                snprintf(buf, sizeof(buf), "sched#%u\n", s_fired);
                sched_dbg(buf);
            }
            cb(cbp);
            goto restart; /* list may have changed — rescan from head */
        }
        pp = &e->next;
    }
}

/* ── quit ────────────────────────────────────────────────────────────────── */

static void aether_quit(void)
{
    __asm__ volatile("mov x8, #0\n mov x0, #0\n svc #0\n");
    __builtin_unreachable();
}

/* ── exported table ──────────────────────────────────────────────────────── */

struct gui_misc_table aether_misc_table = {
    .schedule = aether_schedule,
    .quit     = aether_quit,
};
