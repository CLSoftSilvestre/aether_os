/*
 * AetherGuitar — shared state (Phase 8.10)
 * File: userspace/apps/aether_guitar/aeguitar.h
 */
#ifndef AEGUITAR_H
#define AEGUITAR_H

#include <gfx.h>
#include <gpu.h>
#include <sys.h>
#include <input.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "achain.h"
#include "aplug.h"
#include "afx.h"
#include "awgt.h"
#include "aetheraudio.h"

/* ── Window geometry ─────────────────────────────────────────────────── */

#define WIN_W      820
#define WIN_H      570
#define TITLE_H     28
#define TAB_H       30
#define CONT_PAD     8
#define CONT_W     (WIN_W - 2 * CONT_PAD)              /* 804 */
#define CONT_H     (WIN_H - TITLE_H - TAB_H - 2 * CONT_PAD) /* 496 */

/* ── Views ─────────────────────────────────────────────────────────── */

#define VIEW_BOARD    0
#define VIEW_AMP      1
#define VIEW_TUNER    2
#define VIEW_PRESETS  3
#define VIEW_SETTINGS 4
#define VIEW_COUNT    5

/* ── Colours ─────────────────────────────────────────────────────────── */

#define C_TAB_BG      GFX_RGB( 22,  22,  32)
#define C_TAB_ACTIVE  GFX_RGB( 40,  38,  80)
#define C_TAB_HOVER   GFX_RGB( 30,  30,  50)
#define C_TAB_TEXT    GFX_RGB(180, 180, 210)
#define C_TAB_BORDER  GFX_RGB( 80,  75, 160)
#define C_BOARD_BG    GFX_RGB( 22,  18,  14)   /* dark pedalboard wood */
#define C_BOARD_STRIP GFX_RGB( 35,  28,  20)
#define C_VIEW_BG     GFX_RGB( 20,  20,  30)
#define C_SECTION_HDR GFX_RGB( 50,  48,  90)
#define C_LABEL       GFX_RGB(160, 160, 185)
#define C_VALUE       GFX_RGB(200, 200, 230)
#define C_BTN_N       GFX_RGB( 48,  46,  78)
#define C_BTN_H       GFX_RGB( 68,  66, 105)
#define C_BTN_P       GFX_RGB( 35,  33,  58)
#define C_BTN_BORDER  GFX_RGB( 80,  78, 140)
#define C_ACCENT      GFX_RGB(124, 106, 247)

/* ── Chain node indices (fixed at startup) ───────────────────────────── */

#define NODE_TUNER    0
#define NODE_GATE     1
#define NODE_DRIVE    2
#define NODE_MOD      3
#define NODE_DELAY    4
#define NODE_REVERB   5

/* ── Shared globals (defined in main.c) ──────────────────────────────── */

extern achain_t       *g_chain;
extern audio_client_t *g_audio;
extern int             g_view;
extern int             g_win_x;
extern int             g_win_y;
extern int             g_win_id;
extern int             g_running;
extern unsigned int    g_sr;
extern unsigned int    g_period;

/* ── Frame double-buffering ──────────────────────────────────────────── */

extern unsigned       *g_fb;     /* pixel buffer WIN_W × WIN_H */

static inline int cont_x(void) { return g_win_x + CONT_PAD; }
static inline int cont_y(void) { return g_win_y + TITLE_H + TAB_H + CONT_PAD; }

/* ── Helper: draw a simple push button ──────────────────────────────── */

static inline void draw_btn(int x, int y, int w, int h,
                             const char *label, int pressed)
{
    unsigned bg = pressed ? C_BTN_P : C_BTN_N;
    gfx_fill_rounded((unsigned)x, (unsigned)y,
                     (unsigned)w, (unsigned)h, 3, bg);
    gfx_rect_rounded((unsigned)x, (unsigned)y,
                     (unsigned)w, (unsigned)h, 3, C_BTN_BORDER);
    int llen = (int)strlen(label);
    int lx   = x + (w - llen * 8) / 2;
    int ly   = y + (h - 16) / 2;
    gfx_text_transparent((unsigned)lx, (unsigned)ly, label, C_BTN_BORDER);
}

static inline int btn_hit(int x, int y, int w, int h, int mx, int my)
{
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

/* ── View entry points ───────────────────────────────────────────────── */

void view_board_init(void);
void view_board_draw(int cx, int cy);
void view_board_mouse(int mx, int my, unsigned btn, unsigned prev_btn);

void view_amp_init(void);
void view_amp_draw(int cx, int cy);
void view_amp_mouse(int mx, int my, unsigned btn, unsigned prev_btn);

void view_tuner_draw(int cx, int cy);

void view_presets_init(void);
void view_presets_draw(int cx, int cy);
void view_presets_mouse(int mx, int my, unsigned btn, unsigned prev_btn);
void view_presets_key(keycode_t k, unsigned mods);

void view_settings_draw(int cx, int cy);
void view_settings_mouse(int mx, int my, unsigned btn, unsigned prev_btn);

#endif /* AEGUITAR_H */
