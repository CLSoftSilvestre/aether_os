/*
 * AetherOS libwidget — WIDGET_BUTTON (Phase 5.3)
 * File: userspace/lib/libwidget/button.c
 *
 * Glass gradient pill with pearl rim border — Aero/Liquid Glass style.
 * on_click fires on MOUSE_UP inside bounds or KEY_DOWN(ENTER) when focused.
 */

#include <widget.h>
#include <gfx.h>
#include <string.h>

/* Glass button gradient stops per state */
#define C_BTN_N_TOP  GFX_RGB(108,  98, 210)   /* normal top    */
#define C_BTN_N_BOT  GFX_RGB( 78,  70, 162)   /* normal bottom */
#define C_BTN_H_TOP  GFX_RGB(140, 128, 252)   /* hover top     */
#define C_BTN_H_BOT  GFX_RGB(106,  96, 215)   /* hover bottom  */
#define C_BTN_P_TOP  GFX_RGB( 70,  62, 145)   /* pressed top   */
#define C_BTN_P_BOT  GFX_RGB( 55,  49, 112)   /* pressed bot   */
#define C_BTN_D_TOP  GFX_RGB( 44,  42,  72)   /* disabled top  */
#define C_BTN_D_BOT  GFX_RGB( 36,  34,  58)   /* disabled bot  */
#define C_BTN_TEXT   GFX_RGB(255, 255, 255)

static void button_draw(widget_t *w, int ax, int ay)
{
    unsigned gtop, gbot, rim, spec;
    switch (w->state) {
    case WS_HOVERED:
        gtop = C_BTN_H_TOP; gbot = C_BTN_H_BOT;
        rim  = GFX_RGB(185, 175, 255);
        spec = GFX_RGB(215, 208, 255);
        break;
    case WS_PRESSED:
        gtop = C_BTN_P_TOP; gbot = C_BTN_P_BOT;
        rim  = GFX_RGB(130, 122, 210);
        spec = GFX_RGB(175, 165, 240);
        break;
    case WS_DISABLED:
        gtop = C_BTN_D_TOP; gbot = C_BTN_D_BOT;
        rim  = GFX_RGB( 55,  52,  82);
        spec = GFX_RGB( 60,  57,  90);
        break;
    default: /* WS_NORMAL / WS_FOCUSED */
        gtop = C_BTN_N_TOP; gbot = C_BTN_N_BOT;
        rim  = C_GLASS_RIM;
        spec = GFX_RGB(200, 192, 255);
        break;
    }

    /* Glass gradient fill clipped to rounded corners */
    gfx_gradient_v_rounded((unsigned)ax, (unsigned)ay,
                            (unsigned)w->bounds.w, (unsigned)w->bounds.h,
                            GFX_WIDGET_R, gtop, gbot);

    /* Specular sheen — 1-px near-white line on the very top edge */
    gfx_hline((unsigned)(ax + GFX_WIDGET_R), (unsigned)ay,
              (unsigned)(w->bounds.w - 2 * GFX_WIDGET_R), spec);

    /* Pearl rim border */
    gfx_rect_rounded((unsigned)ax, (unsigned)ay,
                     (unsigned)w->bounds.w, (unsigned)w->bounds.h,
                     GFX_WIDGET_R, rim);

    /* Inner depth line — slightly darker, adds frosted depth */
    if (GFX_WIDGET_R > 1u) {
        gfx_rect_rounded((unsigned)(ax + 1), (unsigned)(ay + 1),
                         (unsigned)(w->bounds.w - 2), (unsigned)(w->bounds.h - 2),
                         GFX_WIDGET_R - 1u, C_GLASS_EDGE);
    }

    /* Content: icon or text */
    if (w->data.button.icon_id != ICON_BTN_NONE) {
        int cx = ax + ((int)w->bounds.w - 14) / 2;
        int cy = ay + ((int)w->bounds.h - 14) / 2;
        gfx_toolbar_icon(cx, cy, w->data.button.icon_id);
    } else {
        /* Text bg = per-channel average of top/bot colors for FT AA accuracy */
        unsigned mid_bg = ((gtop & 0xFEFEFEu) + (gbot & 0xFEFEFEu)) >> 1u;
        gfx_text_center((unsigned)ax, (unsigned)w->bounds.w,
                        (unsigned)(ay + (w->bounds.h - WGT_FONT_H) / 2),
                        w->data.button.text, C_BTN_TEXT, mid_bg);
    }
}

static int button_event(widget_t *w, const widget_event_t *ev)
{
    if (w->state == WS_DISABLED) return 0;

    if (ev->type == WEV_MOUSE_DOWN) {
        w->state = WS_PRESSED;
        w->dirty = 1;
        return 1;
    }

    if (ev->type == WEV_MOUSE_UP) {
        w->state = WS_FOCUSED;
        w->dirty = 1;
        if (w->data.button.on_click)
            w->data.button.on_click(w);
        return 1;
    }

    if (ev->type == WEV_KEY_DOWN && ev->keycode == KEY_ENTER) {
        /* Flash pressed then call */
        w->state = WS_FOCUSED;
        w->dirty = 1;
        if (w->data.button.on_click)
            w->data.button.on_click(w);
        return 1;
    }

    if (ev->type == WEV_FOCUS_IN || ev->type == WEV_FOCUS_OUT) {
        w->dirty = 1;
        return 0;
    }

    return 0;
}

void widget_init_button(widget_t *w, int x, int y, int width, int height,
                        const char *text,
                        void (*on_click)(widget_t *w))
{
    widget_init(w, WIDGET_BUTTON, x, y, width, height);
    w->draw_fn   = button_draw;
    w->event_fn  = button_event;
    w->focusable = 1;

    int i = 0;
    while (text && text[i] && i < 127) {
        w->data.button.text[i] = text[i];
        i++;
    }
    w->data.button.text[i]  = '\0';
    w->data.button.icon_id  = ICON_BTN_NONE;
    w->data.button.on_click = on_click;
}

void widget_init_icon_button(widget_t *w, int x, int y, int width, int height,
                             unsigned char icon_id,
                             void (*on_click)(widget_t *w))
{
    widget_init(w, WIDGET_BUTTON, x, y, width, height);
    w->draw_fn   = button_draw;
    w->event_fn  = button_event;
    w->focusable = 1;

    w->data.button.text[0]  = '\0';
    w->data.button.icon_id  = icon_id;
    w->data.button.on_click = on_click;
}
