/*
 * AetherOS — libAetherAudioWidget: LED indicator widget (Phase 8.9)
 * File: userspace/lib/libAetherAudioWidget/src/awgt_led.c
 */

#include "awgt.h"
#include "awgt_prims.h"
#include <gfx.h>
#include <string.h>

/* On/off/glow color tables indexed by AWGT_LED_* */
static const unsigned led_on[5] = {
    GFX_RGB(255,  60,  60),  /* RED   */
    GFX_RGB( 60, 230,  60),  /* GREEN */
    GFX_RGB(255, 180,   0),  /* AMBER */
    GFX_RGB( 80, 160, 255),  /* BLUE  */
    GFX_RGB(240, 240, 240),  /* WHITE */
};
static const unsigned led_off[5] = {
    GFX_RGB( 70,  15,  15),  /* RED   */
    GFX_RGB( 15,  55,  15),  /* GREEN */
    GFX_RGB( 70,  45,   0),  /* AMBER */
    GFX_RGB( 15,  35,  70),  /* BLUE  */
    GFX_RGB( 60,  60,  60),  /* WHITE */
};
static const unsigned led_glow[5] = {
    GFX_RGB(180,  30,  30),  /* RED   */
    GFX_RGB( 30, 160,  30),  /* GREEN */
    GFX_RGB(180, 110,   0),  /* AMBER */
    GFX_RGB( 40, 100, 200),  /* BLUE  */
    GFX_RGB(170, 170, 170),  /* WHITE */
};

void awgt_led_init(awgt_led_t *l, int cx, int cy, int size, int color)
{
    memset(l, 0, sizeof(*l));
    l->x = cx; l->y = cy;
    l->size  = size;
    l->color = color;
}

void awgt_led_draw(const awgt_led_t *l)
{
    int idx = l->color;
    if (idx < 0 || idx > 4) idx = 0;
    int r = l->size / 2;

    /* outer bezel (dark rim) */
    awgt_fill_circle(l->x, l->y, r + 1, GFX_RGB(20, 20, 25));

    if (l->on) {
        /* glow halo */
        awgt_fill_circle(l->x, l->y, r, led_glow[idx]);
        /* bright core */
        awgt_fill_circle(l->x, l->y, r - 2, led_on[idx]);
        /* specular dot */
        int hx = l->x - r / 3;
        int hy = l->y - r / 3;
        awgt_fill_circle(hx, hy, r / 4, GFX_RGB(255, 255, 255));
    } else {
        awgt_fill_circle(l->x, l->y, r, led_off[idx]);
        /* dim specular */
        int hx = l->x - r / 3;
        int hy = l->y - r / 3;
        awgt_fill_circle(hx, hy, r / 4, GFX_RGB(70, 70, 80));
    }
}

void awgt_led_set(awgt_led_t *l, int on)
{
    l->on = on ? 1 : 0;
}
