/*
 * AetherOS — libAetherAudioWidget: fader widget (Phase 8.9)
 * File: userspace/lib/libAetherAudioWidget/src/awgt_fader.c
 *
 * Vertical: 0 = bottom, 1 = top.  Horizontal: 0 = left, 1 = right.
 * Handle: 24×12 px (vertical) or 12×24 px (horizontal).
 */

#include "awgt.h"
#include <gfx.h>
#include <string.h>

#define C_FADE_TRACK   GFX_RGB( 18,  18,  28)
#define C_FADE_HANDLE  GFX_RGB(155, 155, 175)
#define C_FADE_NOTCH   GFX_RGB( 80,  80,  95)
#define C_FADE_RIM     GFX_RGB( 90,  90, 110)
#define C_FADE_MARK    GFX_RGB( 55,  55,  70)
#define C_FADE_LABEL   GFX_RGB(130, 130, 155)

#define HANDLE_LONG  24
#define HANDLE_SHORT 12

/* Returns the pixel position (along track axis) for value 0..1 */
static int fader_pos_from_val(const awgt_fader_t *f)
{
    if (f->orientation == AWGT_FADER_VERTICAL) {
        int travel = f->h - HANDLE_LONG;
        /* value=1 → top, value=0 → bottom */
        return f->y + f->h - HANDLE_LONG - (int)(f->value * (float)travel);
    } else {
        int travel = f->w - HANDLE_LONG;
        return f->x + (int)(f->value * (float)travel);
    }
}

static float fader_val_from_pos(const awgt_fader_t *f, int pos)
{
    float v;
    if (f->orientation == AWGT_FADER_VERTICAL) {
        int travel = f->h - HANDLE_LONG;
        if (travel <= 0) return 0.5f;
        /* pos is handle top-left y */
        int from_top = pos - f->y;
        v = 1.0f - (float)from_top / (float)travel;
    } else {
        int travel = f->w - HANDLE_LONG;
        if (travel <= 0) return 0.5f;
        v = (float)(pos - f->x) / (float)travel;
    }
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

void awgt_fader_init(awgt_fader_t *f, int x, int y, int w, int h,
                     int orientation, const char *label)
{
    memset(f, 0, sizeof(*f));
    f->x = x; f->y = y; f->w = w; f->h = h;
    f->orientation = orientation;
    f->value = 0.75f;
    f->label = label;
}

void awgt_fader_draw(const awgt_fader_t *f)
{
    if (f->orientation == AWGT_FADER_VERTICAL) {
        int cx = f->x + f->w / 2;

        /* Track groove */
        int track_x = cx - 2;
        gfx_fill_rounded((unsigned)track_x, (unsigned)f->y,
                         4, (unsigned)f->h, 2, C_FADE_TRACK);
        gfx_rect_rounded((unsigned)track_x, (unsigned)f->y,
                         4, (unsigned)f->h, 2, C_FADE_RIM);

        /* Tick marks every 25% */
        for (int t = 0; t <= 4; t++) {
            float frac   = (float)t / 4.0f;
            int travel   = f->h - HANDLE_LONG;
            int ty       = f->y + f->h - HANDLE_LONG / 2
                           - (int)(frac * (float)travel);
            gfx_hline((unsigned)(cx - 5), (unsigned)ty, 10, C_FADE_MARK);
        }

        /* Handle */
        int handle_pos = fader_pos_from_val(f);
        int hx = f->x;
        int hw = f->w;
        gfx_fill_rounded((unsigned)hx, (unsigned)handle_pos,
                         (unsigned)hw, HANDLE_LONG, 3, C_FADE_RIM);
        gfx_fill_rounded((unsigned)(hx + 1), (unsigned)(handle_pos + 1),
                         (unsigned)(hw - 2), HANDLE_LONG - 2, 2, C_FADE_HANDLE);
        /* Centre notch */
        int nc = handle_pos + HANDLE_LONG / 2;
        gfx_hline((unsigned)(hx + 2), (unsigned)(nc - 1),
                  (unsigned)(hw - 4), C_FADE_NOTCH);
        gfx_hline((unsigned)(hx + 2), (unsigned)nc,
                  (unsigned)(hw - 4), GFX_RGB(200, 200, 220));
        gfx_hline((unsigned)(hx + 2), (unsigned)(nc + 1),
                  (unsigned)(hw - 4), C_FADE_NOTCH);

    } else {
        int cy = f->y + f->h / 2;

        /* Track */
        int track_y = cy - 2;
        gfx_fill_rounded((unsigned)f->x, (unsigned)track_y,
                         (unsigned)f->w, 4, 2, C_FADE_TRACK);
        gfx_rect_rounded((unsigned)f->x, (unsigned)track_y,
                         (unsigned)f->w, 4, 2, C_FADE_RIM);

        /* Tick marks */
        for (int t = 0; t <= 4; t++) {
            float frac = (float)t / 4.0f;
            int travel = f->w - HANDLE_LONG;
            int tx     = f->x + HANDLE_LONG / 2
                         + (int)(frac * (float)travel);
            gfx_vline((unsigned)tx, (unsigned)(cy - 5), 10, C_FADE_MARK);
        }

        /* Handle */
        int handle_pos = fader_pos_from_val(f);
        int hy = f->y;
        int hh = f->h;
        gfx_fill_rounded((unsigned)handle_pos, (unsigned)hy,
                         HANDLE_LONG, (unsigned)hh, 3, C_FADE_RIM);
        gfx_fill_rounded((unsigned)(handle_pos + 1), (unsigned)(hy + 1),
                         HANDLE_LONG - 2, (unsigned)(hh - 2), 2, C_FADE_HANDLE);
        int nc = handle_pos + HANDLE_LONG / 2;
        gfx_vline((unsigned)(nc - 1), (unsigned)(hy + 2),
                  (unsigned)(hh - 4), C_FADE_NOTCH);
        gfx_vline((unsigned)nc,       (unsigned)(hy + 2),
                  (unsigned)(hh - 4), GFX_RGB(200, 200, 220));
        gfx_vline((unsigned)(nc + 1), (unsigned)(hy + 2),
                  (unsigned)(hh - 4), C_FADE_NOTCH);
    }

    if (f->label) {
        int llen = (int)strlen(f->label);
        int lx, ly;
        if (f->orientation == AWGT_FADER_VERTICAL) {
            lx = f->x + (f->w - llen * 8) / 2;
            ly = f->y + f->h + 3;
        } else {
            lx = f->x;
            ly = f->y + f->h + 3;
        }
        gfx_text((unsigned)lx, (unsigned)ly, f->label, C_FADE_LABEL, 0);
    }
}

int awgt_fader_mouse_down(awgt_fader_t *f, int mx, int my)
{
    if (mx < f->x || mx >= f->x + f->w) return 0;
    if (my < f->y || my >= f->y + f->h) return 0;
    f->dragging       = 1;
    f->drag_start     = (f->orientation == AWGT_FADER_VERTICAL) ? my : mx;
    f->drag_start_val = f->value;
    return 1;
}

int awgt_fader_mouse_move(awgt_fader_t *f, int mx, int my)
{
    if (!f->dragging) return 0;

    int cur   = (f->orientation == AWGT_FADER_VERTICAL) ? my : mx;
    int delta = cur - f->drag_start;
    int travel;
    if (f->orientation == AWGT_FADER_VERTICAL) {
        travel = f->h - HANDLE_LONG;
        float v = f->drag_start_val - (float)delta / (float)travel;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        f->value = v;
    } else {
        travel = f->w - HANDLE_LONG;
        int new_pos = fader_pos_from_val(f) + delta;
        f->value = fader_val_from_pos(f, new_pos);
    }
    (void)fader_val_from_pos; /* suppress unused warning in V branch */
    if (f->on_change) f->on_change(f->value, f->ctx);
    return 1;
}

void awgt_fader_mouse_up(awgt_fader_t *f)
{
    f->dragging = 0;
}
