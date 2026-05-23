/*
 * AetherOS — libAetherAudioWidget: Skeuomorphic audio widgets (Phase 8.9)
 * File: userspace/lib/libAetherAudioWidget/include/awgt.h
 *
 * Seven self-contained audio UI widgets rendered with gfx primitives:
 *
 *   awgt_knob_t      — Davies 1900H-inspired rotary knob, 270° sweep
 *   awgt_led_t       — Round LED indicator (red/green/amber/blue)
 *   awgt_vu_t        — Segmented VU bar (green/yellow/red) with peak hold
 *   awgt_spectrum_t  — Spectrum analyzer bars with peak hold
 *   awgt_stomp_t     — Foot-switch button with embedded LED
 *   awgt_fader_t     — Vertical or horizontal fader with notched handle
 *   awgt_panel_t     — Pedal/amp housing background with screws + title strip
 *
 * Drawing model:
 *   Each widget is a plain struct.  Call awgt_<type>_draw() to paint it
 *   into the active gfx render target (frame buffer or gfx_begin_frame buf).
 *   Input functions return 1 if the event was consumed.
 *
 * No heap allocation — all structs are value-typed and stack/global safe.
 */

#ifndef AETHER_AWGT_H
#define AETHER_AWGT_H

/* ── Knob ─────────────────────────────────────────────────────────────── */

#define AWGT_KNOB_STYLE_DAVIES   0   /* large dark body, white tick line  */
#define AWGT_KNOB_STYLE_POINTER  1   /* small with arrow pointer           */
#define AWGT_KNOB_STYLE_FLAT     2   /* flat circle, accent-coloured       */

typedef struct {
    int   x, y;          /* top-left corner of bounding box              */
    int   size;          /* diameter in pixels (recommended: 48-72)       */
    float value;         /* normalised 0.0 .. 1.0                         */
    int   style;         /* AWGT_KNOB_STYLE_*                             */
    const char *label;   /* optional label drawn below; NULL = none       */

    /* Drag state (set by awgt_knob_mouse_down/move) */
    int   dragging;
    int   drag_start_y;
    float drag_start_val;

    void (*on_change)(float value, void *ctx);
    void *ctx;
} awgt_knob_t;

void awgt_knob_init(awgt_knob_t *k, int x, int y, int size,
                    int style, const char *label);
void awgt_knob_draw(const awgt_knob_t *k);
int  awgt_knob_mouse_down(awgt_knob_t *k, int mx, int my);
int  awgt_knob_mouse_move(awgt_knob_t *k, int mx, int my);
void awgt_knob_mouse_up(awgt_knob_t *k);

/* ── LED ──────────────────────────────────────────────────────────────── */

#define AWGT_LED_RED     0
#define AWGT_LED_GREEN   1
#define AWGT_LED_AMBER   2
#define AWGT_LED_BLUE    3
#define AWGT_LED_WHITE   4

typedef struct {
    int x, y;    /* centre coordinates */
    int size;    /* diameter in pixels (recommended: 8-16)  */
    int color;   /* AWGT_LED_* */
    int on;      /* 0 = off, 1 = on */
} awgt_led_t;

void awgt_led_init(awgt_led_t *l, int cx, int cy, int size, int color);
void awgt_led_draw(const awgt_led_t *l);
void awgt_led_set(awgt_led_t *l, int on);

/* ── VU meter ─────────────────────────────────────────────────────────── */

#define AWGT_VU_VERTICAL    0
#define AWGT_VU_HORIZONTAL  1

#define AWGT_VU_SEGS        20   /* number of bar segments */
#define AWGT_VU_PEAK_HOLD   60   /* frames to hold peak segment */

typedef struct {
    int   x, y, w, h;
    int   orientation;            /* AWGT_VU_VERTICAL / HORIZONTAL   */
    float level;                  /* current normalised level 0..1    */
    float peak;                   /* peak-hold normalised level        */
    int   peak_timer;             /* countdown in frames               */
    const char *label;            /* optional label; NULL = none       */
} awgt_vu_t;

void awgt_vu_init(awgt_vu_t *v, int x, int y, int w, int h,
                  int orientation, const char *label);
void awgt_vu_feed(awgt_vu_t *v, float level_0_to_1);  /* update level + peak */
void awgt_vu_draw(const awgt_vu_t *v);

/* ── Spectrum analyzer ────────────────────────────────────────────────── */

#define AWGT_SPECTRUM_MAX_BINS  64
#define AWGT_SPEC_PEAK_HOLD     90

typedef struct {
    int   x, y, w, h;
    int   n_bins;
    float bins[AWGT_SPECTRUM_MAX_BINS];
    float peaks[AWGT_SPECTRUM_MAX_BINS];
    int   peak_timers[AWGT_SPECTRUM_MAX_BINS];
} awgt_spectrum_t;

void awgt_spectrum_init(awgt_spectrum_t *s, int x, int y,
                        int w, int h, int n_bins);
void awgt_spectrum_feed(awgt_spectrum_t *s, const float *magnitudes);
void awgt_spectrum_draw(const awgt_spectrum_t *s);

/* ── Stomp switch ─────────────────────────────────────────────────────── */

typedef struct {
    int x, y, w, h;
    const char *label;
    int   active;        /* 0 = off/bypassed, 1 = on/engaged */
    awgt_led_t led;      /* LED embedded above the switch     */

    void (*on_toggle)(int active, void *ctx);
    void *ctx;
} awgt_stomp_t;

void awgt_stomp_init(awgt_stomp_t *s, int x, int y, int w, int h,
                     const char *label);
void awgt_stomp_draw(const awgt_stomp_t *s);
int  awgt_stomp_click(awgt_stomp_t *s, int mx, int my); /* 1 = consumed */

/* ── Fader ─────────────────────────────────────────────────────────────── */

#define AWGT_FADER_VERTICAL    0
#define AWGT_FADER_HORIZONTAL  1

typedef struct {
    int   x, y, w, h;
    int   orientation;
    float value;         /* normalised 0.0 .. 1.0 */
    const char *label;

    int   dragging;
    int   drag_start;    /* pixel coordinate at drag start */
    float drag_start_val;

    void (*on_change)(float value, void *ctx);
    void *ctx;
} awgt_fader_t;

void awgt_fader_init(awgt_fader_t *f, int x, int y, int w, int h,
                     int orientation, const char *label);
void awgt_fader_draw(const awgt_fader_t *f);
int  awgt_fader_mouse_down(awgt_fader_t *f, int mx, int my);
int  awgt_fader_mouse_move(awgt_fader_t *f, int mx, int my);
void awgt_fader_mouse_up(awgt_fader_t *f);

/* ── Panel / housing ──────────────────────────────────────────────────── */

#define AWGT_PANEL_PEDAL  0   /* stomp box housing: dark metal, corner screws */
#define AWGT_PANEL_AMP    1   /* amp head face: wider, leather-look border    */

typedef struct {
    int x, y, w, h;
    int   style;         /* AWGT_PANEL_PEDAL / AWGT_PANEL_AMP */
    const char *title;   /* brand/effect name; NULL = none     */
    unsigned int accent; /* accent colour for title strip      */
} awgt_panel_t;

void awgt_panel_draw(const awgt_panel_t *p);

#endif /* AETHER_AWGT_H */
