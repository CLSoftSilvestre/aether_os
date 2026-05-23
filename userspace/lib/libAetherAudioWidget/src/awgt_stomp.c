/*
 * AetherOS — libAetherAudioWidget: stomp switch widget (Phase 8.9)
 * File: userspace/lib/libAetherAudioWidget/src/awgt_stomp.c
 *
 * A large foot-switch button with an embedded LED above it and a label.
 * Clicking toggles the active state and calls on_toggle().
 */

#include "awgt.h"
#include "awgt_prims.h"
#include <gfx.h>
#include <string.h>

#define C_STOMP_BG       GFX_RGB( 48,  48,  58)
#define C_STOMP_RIM      GFX_RGB( 90,  90, 105)
#define C_STOMP_ACTIVE   GFX_RGB( 60,  60,  75)
#define C_STOMP_SHADOW   GFX_RGB( 20,  20,  25)
#define C_STOMP_LABEL    GFX_RGB(200, 200, 220)

void awgt_stomp_init(awgt_stomp_t *s, int x, int y, int w, int h,
                     const char *label)
{
    memset(s, 0, sizeof(*s));
    s->x = x; s->y = y; s->w = w; s->h = h;
    s->label  = label;
    s->active = 0;

    /* LED centred above the button, 10 px diameter */
    int led_cx = x + w / 2;
    int led_cy = y + 10;
    awgt_led_init(&s->led, led_cx, led_cy, 10, AWGT_LED_GREEN);
}

void awgt_stomp_draw(const awgt_stomp_t *s)
{
    int btn_y  = s->y + 22;          /* below LED */
    int btn_h  = s->h - 22;

    /* Drop shadow */
    gfx_fill_rounded((unsigned)(s->x + 2), (unsigned)(btn_y + 2),
                     (unsigned)s->w, (unsigned)btn_h,
                     6, C_STOMP_SHADOW);

    /* Button rim */
    gfx_fill_rounded((unsigned)s->x, (unsigned)btn_y,
                     (unsigned)s->w, (unsigned)btn_h,
                     6, C_STOMP_RIM);

    /* Button face */
    unsigned face_c = s->active ? C_STOMP_ACTIVE : C_STOMP_BG;
    gfx_fill_rounded((unsigned)(s->x + 1), (unsigned)(btn_y + 1),
                     (unsigned)(s->w - 2), (unsigned)(btn_h - 2),
                     5, face_c);

    /* Embossed circle in the centre */
    int cx   = s->x + s->w / 2;
    int cy   = btn_y + btn_h / 2;
    int cr   = (s->w < btn_h ? s->w : btn_h) / 2 - 6;
    awgt_circle(cx, cy, cr,     GFX_RGB(70, 70, 85));
    awgt_circle(cx, cy, cr - 1, GFX_RGB(30, 30, 40));

    /* LED */
    awgt_led_set((awgt_led_t *)&s->led, s->active);
    awgt_led_draw(&s->led);

    /* Label below button */
    if (s->label) {
        int llen = (int)strlen(s->label);
        int lx   = s->x + (s->w - llen * 8) / 2;
        int ly   = s->y + s->h + 2;
        gfx_text((unsigned)lx, (unsigned)ly,
                 s->label, C_STOMP_LABEL, 0);
    }
}

int awgt_stomp_click(awgt_stomp_t *s, int mx, int my)
{
    if (mx < s->x || mx >= s->x + s->w) return 0;
    if (my < s->y || my >= s->y + s->h) return 0;
    s->active = s->active ? 0 : 1;
    if (s->on_toggle) s->on_toggle(s->active, s->ctx);
    return 1;
}
