/*
 * AetherGuitar — Settings view (Phase 8.10)
 * File: userspace/apps/aether_guitar/view_settings.c
 *
 * Audio settings: sample rate, period size, device info.
 * Applying settings requires restarting the audio client and chain.
 */

#include "aeguitar.h"

static const unsigned int sr_options[3]  = { 44100, 48000, 96000 };
static const unsigned int per_options[4] = {    64,   128,   256,  512 };
static const char *sr_labels[3]   = { "44100 Hz", "48000 Hz", "96000 Hz" };
static const char *per_labels[4]  = { "64", "128", "256", "512" };

static int g_sr_sel  = 1;    /* default: 48000 Hz   */
static int g_per_sel = 2;    /* default: 256 frames */
static int g_apply_btn_pressed = 0;

static char g_status[80];

static void apply_settings(void)
{
    unsigned int new_sr  = sr_options[g_sr_sel];
    unsigned int new_per = per_options[g_per_sel];

    if (g_audio) {
        audio_client_deactivate(g_audio);
        audio_client_close(g_audio);
        g_audio = (void *)0;
    }

    g_sr     = new_sr;
    g_period = new_per;

    /* Rebuild chain at new SR/period */
    if (g_chain) {
        achain_destroy(g_chain);
        g_chain = (void *)0;
    }
    /* Re-run chain init (simplified: reopen with new settings) */
    g_chain = achain_create((float)g_sr, (int)g_period);

    g_audio = audio_client_open(g_sr, 2, g_period);
    if (g_audio) {
        /* Re-register audio callback (defined in main.c) */
        extern void audio_cb(float *in, float *out,
                              unsigned int frames, void *ud);
        audio_client_set_process_callback(g_audio, audio_cb, (void *)0);
        audio_client_activate(g_audio);
        snprintf(g_status, sizeof(g_status),
                 "Applied: %u Hz / %u frames (~%.1fms)",
                 g_sr, g_period,
                 (float)g_period / (float)g_sr * 1000.0f);
    } else {
        snprintf(g_status, sizeof(g_status), "Audio client failed to open.");
    }
}

void view_settings_draw(int cx, int cy)
{
    (void)cx; (void)cy;
    int bx = cont_x(), by = cont_y();

    /* Section header */
    gfx_fill((unsigned)bx, (unsigned)by,
              (unsigned)CONT_W, 24, C_SECTION_HDR);
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)(by + 4),
                         "Audio Settings", C_VALUE);

    int row_y = by + 40;

    /* ── Sample rate ── */
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)row_y,
                         "Sample Rate:", C_LABEL);
    for (int i = 0; i < 3; i++) {
        int bx2 = bx + 120 + i * 110;
        unsigned bg = (g_sr_sel == i) ? GFX_RGB(55, 50, 120) : C_BTN_N;
        gfx_fill_rounded((unsigned)bx2, (unsigned)row_y,
                         100, 22, 3, bg);
        gfx_rect_rounded((unsigned)bx2, (unsigned)row_y,
                         100, 22, 3, C_BTN_BORDER);
        int nlen = (int)strlen(sr_labels[i]);
        gfx_text_transparent((unsigned)(bx2 + (100 - nlen * 8) / 2),
                             (unsigned)(row_y + 3), sr_labels[i], C_VALUE);
    }

    /* ── Period ── */
    row_y += 42;
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)row_y,
                         "Period (frames):", C_LABEL);
    for (int i = 0; i < 4; i++) {
        int bx2 = bx + 148 + i * 60;
        unsigned bg = (g_per_sel == i) ? GFX_RGB(55, 50, 120) : C_BTN_N;
        gfx_fill_rounded((unsigned)bx2, (unsigned)row_y,
                         52, 22, 3, bg);
        gfx_rect_rounded((unsigned)bx2, (unsigned)row_y,
                         52, 22, 3, C_BTN_BORDER);
        int nlen = (int)strlen(per_labels[i]);
        gfx_text_transparent((unsigned)(bx2 + (52 - nlen * 8) / 2),
                             (unsigned)(row_y + 3), per_labels[i], C_VALUE);
    }

    /* ── Latency estimate ── */
    row_y += 42;
    char lat_buf[60];
    float lat_ms = (float)per_options[g_per_sel] /
                   (float)sr_options[g_sr_sel] * 1000.0f;
    int   lat_int  = (int)lat_ms;
    int   lat_frac = (int)((lat_ms - (float)lat_int) * 10.0f);
    snprintf(lat_buf, sizeof(lat_buf),
             "Estimated latency: %d.%d ms (one period)", lat_int, lat_frac);
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)row_y,
                         lat_buf, GFX_RGB(120, 180, 120));

    /* ── Device info ── */
    row_y += 36;
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)row_y,
                         "Audio device: UAC2 (preferred) / I2S fallback",
                         C_LABEL);

    /* ── Apply button ── */
    row_y += 36;
    draw_btn(bx + 8, row_y, 120, 28, "Apply & Restart", g_apply_btn_pressed);
    g_apply_btn_pressed = 0;

    /* ── Current settings ── */
    row_y += 46;
    char cur_buf[80];
    snprintf(cur_buf, sizeof(cur_buf),
             "Current: %u Hz / %u frames", g_sr, g_period);
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)row_y,
                         cur_buf, GFX_RGB(100, 100, 140));

    /* ── Status ── */
    int st_y = by + CONT_H - 22;
    gfx_fill((unsigned)bx, (unsigned)st_y,
              (unsigned)CONT_W, 20, GFX_RGB(16, 16, 26));
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)(st_y + 2),
                         g_status, GFX_RGB(140, 200, 140));
}

void view_settings_mouse(int mx, int my, unsigned btn, unsigned prev_btn)
{
    int pressed = (btn & 1) && !(prev_btn & 1);
    if (!pressed) return;

    int bx = cont_x(), by = cont_y();
    int row_y = by + 40;

    /* SR buttons */
    for (int i = 0; i < 3; i++) {
        int bx2 = bx + 120 + i * 110;
        if (btn_hit(bx2, row_y, 100, 22, mx, my)) { g_sr_sel = i; return; }
    }

    /* Period buttons */
    row_y += 42;
    for (int i = 0; i < 4; i++) {
        int bx2 = bx + 148 + i * 60;
        if (btn_hit(bx2, row_y, 52, 22, mx, my)) { g_per_sel = i; return; }
    }

    /* Apply */
    row_y += 42 + 36 + 36;
    if (btn_hit(bx + 8, row_y, 120, 28, mx, my)) {
        g_apply_btn_pressed = 1;
        apply_settings();
    }
}
