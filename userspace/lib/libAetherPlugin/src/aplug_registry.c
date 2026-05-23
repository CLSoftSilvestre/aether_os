/*
 * AetherOS — libAetherPlugin: Registry (Phase 8.5)
 * File: userspace/lib/libAetherPlugin/src/aplug_registry.c
 *
 * Static plugin registry: up to APLUG_MAX_PLUGINS factories.
 * No dlopen; all plugins are statically linked and self-register via
 * aplug_registry_register() at startup.
 */

#include "aplug.h"
#include <stdlib.h>
#include <string.h>

#define APLUG_MAX_PLUGINS 128

static aplug_factory_fn g_factories[APLUG_MAX_PLUGINS];
static int              g_nfactories = 0;

/* Temporary: cache descriptor from each factory by calling it once,
 * grabbing the descriptor, then freeing.  Stored for enum/lookup.   */
static const aplug_descriptor_t *g_descs[APLUG_MAX_PLUGINS];

void aplug_registry_init(void)
{
    g_nfactories = 0;
    memset(g_factories, 0, sizeof(g_factories));
    memset(g_descs,     0, sizeof(g_descs));
}

int aplug_registry_register(aplug_factory_fn factory)
{
    if (!factory || g_nfactories >= APLUG_MAX_PLUGINS) return -1;

    int idx = g_nfactories++;
    g_factories[idx] = factory;

    /* Create a temporary instance to read the descriptor, then destroy. */
    aplug_t *tmp = factory();
    if (tmp) {
        g_descs[idx] = tmp->desc;
        aplug_destroy(tmp);
    }
    return 0;
}

aplug_t *aplug_registry_create(const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < g_nfactories; i++) {
        if (g_descs[i] && strcmp(g_descs[i]->id, id) == 0)
            return g_factories[i]();
    }
    return NULL;
}

aplug_t *aplug_registry_enum(int category, int *idx)
{
    if (!idx) return NULL;
    while (*idx < g_nfactories) {
        int i = (*idx)++;
        if (g_descs[i] && g_descs[i]->category == category)
            return g_factories[i]();
    }
    return NULL;
}

void aplug_destroy(aplug_t *p)
{
    if (!p) return;
    if (p->vtable && p->vtable->deactivate)
        p->vtable->deactivate(p);
    free(p);
}
