/*
 * bitmap_aether.h — shared AetherOS bitmap struct
 *
 * Used by both gui_bitmap_aether.c (creation/management) and
 * plot_aether.c (blitting to the off-screen render buffer).
 * Data layout: width×height × 4 bytes, RGBA byte order
 * (R at byte 0, G at byte 1, B at byte 2, A at byte 3).
 */

#ifndef BITMAP_AETHER_H
#define BITMAP_AETHER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct aether_bitmap {
    int     width;
    int     height;
    bool    opaque;
    uint8_t data[];   /* width * height * 4 bytes (RGBA) */
} aether_bitmap_t;

#endif /* BITMAP_AETHER_H */
