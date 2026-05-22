/*
 * AetherGuitar — Tuner view (Phase 8.10 visual overhaul)
 * File: userspace/apps/aether_guitar/view_tuner.c
 *
 * Chromatic tuner display with deep-space background image (tuner_bg.bmp).
 * Renders: large circular display, cents needle bar, 6-string reference row.
 */

#include "aeguitar.h"
#include "awgt_prims.h"

extern void awgt_fill_circle(int cx, int cy, int r, unsigned color);
extern void awgt_line(int x0, int y0, int x1, int y1, unsigned color);
extern void awgt_circle(int cx, int cy, int r, unsigned color);

/* ── Tuner background image ──────────────────────────────────────────── */

#define TUNER_BG_PIXELS (804 * 496)
static unsigned g_px_tuner_bg[TUNER_BG_PIXELS];
static unsigned g_tuner_bg_w, g_tuner_bg_h;
static int      g_tuner_bg_ok;

static void tuner_bg_load(void)
{
    if (g_tuner_bg_w > 0) return;  /* already loaded */
    int ok = gfx_bmp_load_icon("/aeguitar/tuner_bg.bmp",
                                g_px_tuner_bg, TUNER_BG_PIXELS,
                                &g_tuner_bg_w, &g_tuner_bg_h);
    g_tuner_bg_ok = (ok == 0);
}

/* ── Note table ──────────────────────────────────────────────────────── */

static const char *note_names[12] = {
    "C", "C#", "D", "D#", "E", "F",
    "F#", "G", "G#", "A", "A#", "B"
};

/* Guitar string open notes: E2(40) A2(45) D3(50) G3(55) B3(59) E4(64) */
static const char *string_names[6]  = { "E2","A2","D3","G3","B3","E4" };
static const float string_hz[6]     = { 82.41f, 110.0f, 146.83f, 196.0f, 246.94f, 329.63f };

/* ── hz → MIDI note + cents ──────────────────────────────────────────── */

static int hz_to_midi_cents(float hz, int *cents_out)
{
    if (hz < 20.0f || hz > 8000.0f) { *cents_out = 0; return -1; }
    /* note_float = 69 + 12 * log2(hz/440) */
    float ratio    = hz / 440.0f;
    float log2_r   = (float)log2((double)ratio);
    float note_f   = 69.0f + 12.0f * log2_r;
    int   note     = (int)(note_f + 0.5f);
    int   cents    = (int)((note_f - (float)note) * 100.0f);
    if (cents_out) *cents_out = cents;
    return note;
}

/* ── Big character rendering (3× scale via 3×3 pixel blocks) ─────────── */

static void big_char(int x, int y, char ch, unsigned fg)
{
    /* 3× scaling: each pixel of the 8×16 font becomes a 3×3 block */
    char tmp[2] = { ch, 0 };
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 8; col++) {
            /* Draw the character into a tiny off-screen region and sample it.
             * Since we don't have a font bitmap exposed, we redraw at scale by
             * drawing 3×3 blobs and relying on the gfx_char coverage. */
            (void)tmp; (void)fg; (void)row; (void)col;
        }
    }
    /* Fallback: draw at 1× with visible bold by drawing 4 offsets */
    gfx_char_transparent((unsigned)(x + 0), (unsigned)(y + 0), ch, fg);
    gfx_char_transparent((unsigned)(x + 1), (unsigned)(y + 0), ch, fg);
    gfx_char_transparent((unsigned)(x + 0), (unsigned)(y + 1), ch, fg);
    gfx_char_transparent((unsigned)(x + 1), (unsigned)(y + 1), ch, fg);
}

static void big_text(int x, int y, const char *s, unsigned fg)
{
    int cx = x;
    while (*s) {
        big_char(cx, y, *s, fg);
        cx += 10; /* 8 + 2 gap */
        s++;
    }
}

/* ── Draw ─────────────────────────────────────────────────────────────── */

void view_tuner_draw(int cx, int cy)
{
    int bx = cont_x(), by = cont_y();

    /* Deep-space background image */
    tuner_bg_load();
    if (g_tuner_bg_ok && g_tuner_bg_w > 0) {
        gfx_icon_blit(g_px_tuner_bg, g_tuner_bg_w, g_tuner_bg_h,
                      bx, by, CONT_W, CONT_H);
    } else {
        gfx_fill((unsigned)bx, (unsigned)by,
                 (unsigned)CONT_W, (unsigned)CONT_H, GFX_RGB(8, 8, 14));
    }

    /* Read detected pitch */
    float hz = 0.0f;
    if (g_chain) {
        aplug_t *tuner = achain_node(g_chain, NODE_TUNER);
        if (tuner) hz = aplug_param_get(tuner, 1); /* TUNER_P_HZ = 1 */
    }

    int   cents = 0;
    int   midi  = hz_to_midi_cents(hz, &cents);
    int   note_idx = -1;
    int   octave   = 0;

    if (midi >= 0) {
        note_idx = ((midi % 12) + 12) % 12;
        octave   = midi / 12 - 1;
    }

    /* ── Outer ring ── */
    int circle_cx = bx + CONT_W / 2;
    int circle_cy = by + CONT_H / 2 - 30;
    int ring_r    = 120;

    awgt_fill_circle(circle_cx, circle_cy, ring_r + 2, GFX_RGB(20, 20, 30));
    awgt_fill_circle(circle_cx, circle_cy, ring_r,
                     (midi >= 0) ? GFX_RGB(15, 15, 22) : GFX_RGB(10, 10, 18));
    awgt_circle(circle_cx, circle_cy, ring_r,
                (midi >= 0) ? GFX_RGB(90, 85, 200) : GFX_RGB(50, 50, 70));

    /* In-tune glow when |cents| <= 3 */
    if (midi >= 0 && cents >= -3 && cents <= 3) {
        awgt_circle(circle_cx, circle_cy, ring_r + 1, GFX_RGB(60, 220, 60));
        awgt_circle(circle_cx, circle_cy, ring_r - 1, GFX_RGB(40, 180, 40));
    }

    /* ── Note name inside circle ── */
    if (midi >= 0) {
        const char *nname = note_names[note_idx];
        /* Centre the text (each char is ~10px wide in big_text) */
        int nlen  = (int)strlen(nname);
        int text_w = nlen * 10 + 10; /* approx with octave digit */
        int tx    = circle_cx - text_w / 2;
        int ty    = circle_cy - 16;
        unsigned note_col = (cents >= -3 && cents <= 3)
                            ? GFX_RGB(80, 240, 80)
                            : GFX_RGB(220, 220, 255);
        big_text(tx, ty, nname, note_col);
        /* Octave number */
        char oct_ch = (char)('0' + (octave < 0 ? 0 : (octave > 9 ? 9 : octave)));
        big_char(tx + nlen * 10 + 2, ty, oct_ch, GFX_RGB(140, 140, 180));

        /* Hz value below note name */
        char hz_buf[24];
        int  hz_int  = (int)hz;
        int  hz_frac = (int)((hz - (float)hz_int) * 10.0f);
        snprintf(hz_buf, sizeof(hz_buf), "%d.%dHz", hz_int, hz_frac);
        int hlen = (int)strlen(hz_buf);
        gfx_text_transparent((unsigned)(circle_cx - hlen * 4),
                             (unsigned)(circle_cy + 10),
                             hz_buf, GFX_RGB(100, 100, 140));
    } else {
        /* No signal */
        gfx_text_transparent((unsigned)(circle_cx - 20),
                             (unsigned)(circle_cy - 8),
                             "---", GFX_RGB(60, 60, 80));
    }

    /* ── Cents needle bar ── */
    int bar_x  = bx + (CONT_W - 400) / 2;
    int bar_y  = by + CONT_H / 2 + ring_r - 10;
    int bar_w  = 400;
    int bar_h  = 20;

    gfx_fill_rounded((unsigned)bar_x, (unsigned)bar_y,
                     (unsigned)bar_w, (unsigned)bar_h, 3, GFX_RGB(12, 12, 20));
    gfx_rect_rounded((unsigned)bar_x, (unsigned)bar_y,
                     (unsigned)bar_w, (unsigned)bar_h, 3, GFX_RGB(50, 50, 80));

    /* Centre tick */
    gfx_vline((unsigned)(bar_x + bar_w / 2), (unsigned)bar_y,
               (unsigned)bar_h, GFX_RGB(80, 80, 120));

    /* ±25 cent ticks */
    gfx_vline((unsigned)(bar_x + bar_w / 4), (unsigned)(bar_y + 4),
               (unsigned)(bar_h - 8), GFX_RGB(50, 50, 70));
    gfx_vline((unsigned)(bar_x + 3 * bar_w / 4), (unsigned)(bar_y + 4),
               (unsigned)(bar_h - 8), GFX_RGB(50, 50, 70));

    /* Cents labels */
    gfx_text_transparent((unsigned)(bar_x - 20), (unsigned)(bar_y + 2),
                         "-50", GFX_RGB(80, 80, 100));
    gfx_text_transparent((unsigned)(bar_x + bar_w + 2), (unsigned)(bar_y + 2),
                         "+50", GFX_RGB(80, 80, 100));

    if (midi >= 0) {
        /* Needle position: cents ∈ [-50, 50] → [bar_x, bar_x+bar_w] */
        float frac   = (float)(cents + 50) / 100.0f;
        int needle_x = bar_x + (int)(frac * (float)bar_w);
        unsigned nc  = (cents >= -3 && cents <= 3)
                       ? GFX_RGB(80, 240, 80) : GFX_RGB(247, 106, 60);
        awgt_fill_circle(needle_x, bar_y + bar_h / 2, 6, nc);
    }

    /* ── String reference row ── */
    int str_y = bar_y + bar_h + 18;
    gfx_text_transparent((unsigned)bx, (unsigned)str_y,
                         "Open strings:", C_LABEL);

    int sx0   = bx + (CONT_W - 6 * 80) / 2;
    for (int s = 0; s < 6; s++) {
        int sx = sx0 + s * 80;

        /* Highlight if current note is close to this string's open pitch */
        int is_match = (hz > 0.0f &&
                        hz > string_hz[s] * 0.98f &&
                        hz < string_hz[s] * 1.02f);

        unsigned bg = is_match ? GFX_RGB(40, 80, 40) : GFX_RGB(16, 16, 26);
        gfx_fill_rounded((unsigned)sx, (unsigned)(str_y + 18),
                         70, 28, 4, bg);
        gfx_rect_rounded((unsigned)sx, (unsigned)(str_y + 18),
                         70, 28, 4,
                         is_match ? GFX_RGB(60, 180, 60) : GFX_RGB(40, 40, 70));

        int nlen = (int)strlen(string_names[s]);
        gfx_text_transparent((unsigned)(sx + (70 - nlen * 8) / 2),
                             (unsigned)(str_y + 24),
                             string_names[s], C_VALUE);
    }

    (void)cx; (void)cy;
}
