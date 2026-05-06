/*
 * AetherOS libwidget — WIDGET_BUTTON (Phase 5.3)
 * File: userspace/lib/libwidget/button.c
 *
 * Three visual states: normal (C_ACCENT fill), hovered (lighter), pressed (darker).
 * on_click fires on MOUSE_UP inside bounds or KEY_DOWN(ENTER) when focused.
 */

#include <widget.h>
#include <gfx.h>
#include <string.h>

/* Button colour scheme */
#define C_BTN_NORMAL  C_ACCENT                     /* purple  */
#define C_BTN_HOVER   GFX_RGB(144, 126, 255)       /* lighter */
#define C_BTN_PRESS   GFX_RGB(100,  85, 200)       /* darker  */
#define C_BTN_TEXT    GFX_RGB(255, 255, 255)       /* white   */
#define C_BTN_DIS     GFX_RGB( 60,  60,  90)       /* disabled */

static void button_draw(widget_t *w, int ax, int ay)
{
    unsigned bg;
    switch (w->state) {
    case WS_HOVERED:  bg = C_BTN_HOVER; break;
    case WS_PRESSED:  bg = C_BTN_PRESS; break;
    case WS_DISABLED: bg = C_BTN_DIS;   break;
    default:          bg = C_BTN_NORMAL; break;
    }

    /* Rounded glass fill */
    gfx_fill_rounded((unsigned)ax, (unsigned)ay,
                     (unsigned)w->bounds.w, (unsigned)w->bounds.h,
                     GFX_WIDGET_R, bg);

    /* Glass specular — 1-px bright line on top edge of button */
    gfx_hline((unsigned)(ax + GFX_WIDGET_R), (unsigned)ay,
              (unsigned)(w->bounds.w - 2 * GFX_WIDGET_R),
              GFX_RGB(200, 185, 255));

    /* Rounded glass border rim */
    gfx_rect_rounded((unsigned)ax, (unsigned)ay,
                     (unsigned)w->bounds.w, (unsigned)w->bounds.h,
                     GFX_WIDGET_R, GFX_RGB(160, 145, 230));

    /* Centred label — uses the fill bg so char backgrounds match */
    gfx_text_center((unsigned)ax, (unsigned)w->bounds.w,
                    (unsigned)(ay + (w->bounds.h - WGT_FONT_H) / 2),
                    w->data.button.text, C_BTN_TEXT, bg);
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
    w->data.button.text[i]   = '\0';
    w->data.button.on_click  = on_click;
}
