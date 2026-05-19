/*
 * Phase 7.5 — gui_bitmap_table: AetherOS implementation
 *
 * NetSurf uses this table to create/destroy platform-native bitmaps for
 * decoded images (PNG, JPEG, GIF, …).  For AetherOS we use plain
 * heap-allocated 32-bpp RGBA buffers — the same format our framebuffer
 * uses — so Phase 7.7's plot_bitmap can blit them directly.
 *
 * All mandatory functions are implemented; render() is a stub
 * (not needed until browser_window_create is called).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/errors.h"
#include "netsurf/bitmap.h"
#include "bitmap_aether.h"
#include "netsurf_aether.h"

/* ── operations ──────────────────────────────────────────────────────────── */

static void *bitmap_create(int width, int height, enum gui_bitmap_flags flags)
{
    size_t nbytes = (size_t)width * (size_t)height * 4;
    aether_bitmap_t *bm = malloc(sizeof(aether_bitmap_t) + nbytes);
    if (!bm) return NULL;

    bm->width  = width;
    bm->height = height;
    bm->opaque = (flags & BITMAP_OPAQUE) != 0;

    if (flags & BITMAP_CLEAR)
        memset(bm->data, 0, nbytes);

    return bm;
}

static void bitmap_destroy(void *bm_)
{
    free(bm_);
}

static void bitmap_set_opaque(void *bm_, bool opaque)
{
    aether_bitmap_t *bm = bm_;
    if (bm) bm->opaque = opaque;
}

static bool bitmap_get_opaque(void *bm_)
{
    aether_bitmap_t *bm = bm_;
    return bm ? bm->opaque : false;
}

static unsigned char *bitmap_get_buffer(void *bm_)
{
    aether_bitmap_t *bm = bm_;
    return bm ? bm->data : NULL;
}

static size_t bitmap_get_rowstride(void *bm_)
{
    aether_bitmap_t *bm = bm_;
    return bm ? (size_t)bm->width * 4 : 0;
}

static int bitmap_get_width(void *bm_)
{
    aether_bitmap_t *bm = bm_;
    return bm ? bm->width : 0;
}

static int bitmap_get_height(void *bm_)
{
    aether_bitmap_t *bm = bm_;
    return bm ? bm->height : 0;
}

static void bitmap_modified(void *bm_)
{
    (void)bm_;
    /* No caching layer to invalidate for MVP */
}

static nserror bitmap_render(struct bitmap *bm_, struct hlcache_handle *content)
{
    (void)bm_; (void)content;
    /* TODO-7.7: render content into bitmap for thumbnail / favicon */
    return NSERROR_NOT_IMPLEMENTED;
}

/* ── exported table ──────────────────────────────────────────────────────── */

struct gui_bitmap_table aether_bitmap_table = {
    .create       = bitmap_create,
    .destroy      = bitmap_destroy,
    .set_opaque   = bitmap_set_opaque,
    .get_opaque   = bitmap_get_opaque,
    .get_buffer   = bitmap_get_buffer,
    .get_rowstride = bitmap_get_rowstride,
    .get_width    = bitmap_get_width,
    .get_height   = bitmap_get_height,
    .modified     = bitmap_modified,
    .render       = bitmap_render,
};
