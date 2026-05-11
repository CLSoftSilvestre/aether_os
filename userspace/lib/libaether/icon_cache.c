/*
 * AetherOS — Icon cache implementation (Phase 6.2)
 * File: userspace/lib/libaether/icon_cache.c
 */

#include <icon_cache.h>
#include <gfx.h>
#include <string.h>
#include <stdio.h>

#define ICON_DIR "/icons/"

static icon_entry_t g_pool[ICON_CACHE_MAX];
static int          g_count;   /* zero-initialised by BSS */

void icon_cache_init(void)
{
    for (int i = 0; i < ICON_CACHE_MAX; i++)
        g_pool[i].valid = 0;
    g_count = 0;
}

const icon_entry_t *icon_cache_get(const char *name)
{
    if (!name || !name[0]) return (void *)0;

    /* Search existing slots (hit or previously-failed load) */
    for (int i = 0; i < g_count; i++) {
        if (strcmp(g_pool[i].name, name) == 0)
            return g_pool[i].valid ? &g_pool[i] : (void *)0;
    }

    /* Pool exhausted — fall through to procedural icons */
    if (g_count >= ICON_CACHE_MAX) return (void *)0;

    icon_entry_t *e = &g_pool[g_count++];

    /* Store key (truncate if needed) */
    int n = 0;
    while (name[n] && n < (int)sizeof(e->name) - 1) { e->name[n] = name[n]; n++; }
    e->name[n] = '\0';
    e->valid = 0;

    /* Build path: /icons/<name>.bmp */
    char path[64];
    snprintf(path, sizeof(path), "%s%s.bmp", ICON_DIR, name);

    unsigned w, h;
    if (gfx_bmp_load_icon(path, e->pixels, ICON_MAX_DIM * ICON_MAX_DIM, &w, &h) != 0)
        return (void *)0;   /* slot stays valid=0; future calls short-circuit */

    if (w == 0 || h == 0 || w > ICON_MAX_DIM || h > ICON_MAX_DIM)
        return (void *)0;

    e->width  = w;
    e->height = h;
    e->valid  = 1;
    return e;
}
