/*
 * AetherOS — AetherGuitar (Phase 8.10)
 * File: userspace/apps/aether_guitar/main.c
 *
 * Guitar signal-chain application.
 * Five views: BOARD · AMP · TUNER · PRESETS · SETTINGS
 *
 * Audio model: audio_client_t runs an RT callback that routes the mono
 * guitar input (left channel of stereo pair) through achain_process().
 * Parameter changes from the UI go through the lock-free SPSC queue.
 *
 * Window is registered with the compositor WM and drawn into a GPU BO
 * (fallback to malloc buffer) via gfx_begin_frame / gfx_end_frame.
 */

#include "aeguitar.h"

/* ── Globals ──────────────────────────────────────────────────────────── */

achain_t       *g_chain   = (void *)0;
audio_client_t *g_audio   = (void *)0;
int             g_view    = VIEW_BOARD;
int             g_win_x   = 60;
int             g_win_y   = 76;
int             g_win_id  = -1;
int             g_running = 1;
unsigned int    g_sr      = 48000;
unsigned int    g_period  = 256;
unsigned       *g_fb      = (void *)0;
gpu_bo_t        g_win_bo  = GPU_BO_INVALID;

/* ── Audio callback (RT thread) ───────────────────────────────────────── */

void audio_cb(float *in, float *out,
                     unsigned int frames, void *ud)
{
    (void)ud;
    /* in/out: deinterleaved, 2 channels × frames.
     * Guitar mono input on left channel (in[0..frames-1]).
     * Stereo output: out[0..frames-1]=L, out[frames..2*frames-1]=R. */
    achain_process(g_chain,
                   in,            /* mono guitar: left channel of input  */
                   out,           /* out_l */
                   out + frames,  /* out_r */
                   (int)frames);
}

/* ── Chain initialisation ─────────────────────────────────────────────── */

static void chain_init(void)
{
    aplug_registry_init();
    afx_register_all();

    g_chain = achain_create((float)g_sr, (int)g_period);
    if (!g_chain) return;

    /* Node 0 — Chromatic Tuner (always active, pass-through) */
    aplug_t *tuner = aplug_registry_create("os.aether.tuner");
    if (tuner) achain_append(g_chain, tuner);

    /* Node 1 — Noise Gate */
    aplug_t *gate = aplug_registry_create("os.aether.noisegate");
    if (gate) {
        int idx = achain_append(g_chain, gate);
        /* threshold = -50dB, hold = 80ms, release = 200ms */
        achain_param_post(g_chain, idx, 1, -50.0f);
        achain_param_post(g_chain, idx, 3,  80.0f);
        achain_param_post(g_chain, idx, 4, 200.0f);
    }

    /* Node 2 — Overdrive (drive=0.4, tone=0.5) */
    aplug_t *od = aplug_registry_create("os.aether.overdrive");
    if (od) {
        int idx = achain_append(g_chain, od);
        achain_param_post(g_chain, idx, 1, 0.4f);
        achain_param_post(g_chain, idx, 2, 0.5f);
        achain_param_post(g_chain, idx, 3, 0.7f);
    }

    /* Node 3 — Chorus (rate=0.5Hz, depth=0.3, mix=0.3) */
    aplug_t *chorus = aplug_registry_create("os.aether.chorus");
    if (chorus) {
        int idx = achain_append(g_chain, chorus);
        achain_param_post(g_chain, idx, 1, 0.5f);
        achain_param_post(g_chain, idx, 2, 0.3f);
        achain_param_post(g_chain, idx, 3, 0.3f);
    }

    /* Node 4 — Delay (time=400ms, feedback=0.35, mix=0.25) */
    aplug_t *delay = aplug_registry_create("os.aether.delay");
    if (delay) {
        int idx = achain_append(g_chain, delay);
        achain_param_post(g_chain, idx, 1, 400.0f);
        achain_param_post(g_chain, idx, 2, 0.35f);
        achain_param_post(g_chain, idx, 3, 0.25f);
    }

    /* Node 5 — Plate Reverb (decay=0.45, damp=0.5, mix=0.2) */
    aplug_t *plate = aplug_registry_create("os.aether.plate");
    if (plate) {
        int idx = achain_append(g_chain, plate);
        achain_param_post(g_chain, idx, 1, 0.45f);
        achain_param_post(g_chain, idx, 2, 0.50f);
        achain_param_post(g_chain, idx, 3, 0.20f);
    }
}

/* ── Draw window chrome + tab bar ─────────────────────────────────────── */

static const char *tab_names[VIEW_COUNT] = {
    "BOARD", "AMP", "TUNER", "PRESETS", "SETTINGS"
};

static void draw_chrome(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H,
                            TITLE_H, "AetherGuitar", 0);
}

static void draw_tabbar(void)
{
    int tab_w = CONT_W / VIEW_COUNT;
    int tx    = g_win_x + CONT_PAD;
    int ty    = g_win_y + TITLE_H;

    /* Background */
    gfx_fill((unsigned)tx, (unsigned)ty,
              (unsigned)CONT_W, (unsigned)TAB_H,
              C_TAB_BG);

    for (int i = 0; i < VIEW_COUNT; i++) {
        int tw = (i == VIEW_COUNT - 1) ? (CONT_W - i * tab_w) : tab_w;
        int bx = tx + i * tab_w;

        unsigned bg = (i == g_view) ? C_TAB_ACTIVE : C_TAB_BG;
        gfx_fill((unsigned)bx, (unsigned)ty, (unsigned)tw, (unsigned)TAB_H, bg);

        if (i == g_view) {
            gfx_hline((unsigned)bx, (unsigned)ty,
                      (unsigned)tw, C_TAB_BORDER);
        }

        const char *name = tab_names[i];
        int nlen  = (int)strlen(name);
        int nx    = bx + (tw - nlen * 8) / 2;
        int ny    = ty + (TAB_H - 16) / 2;
        gfx_text_transparent((unsigned)nx, (unsigned)ny,
                             name, C_TAB_TEXT);

        /* Vertical separator */
        if (i < VIEW_COUNT - 1)
            gfx_vline((unsigned)(bx + tw - 1), (unsigned)ty,
                      (unsigned)TAB_H,
                      GFX_RGB(50, 50, 80));
    }

    /* Bottom accent line */
    gfx_hline((unsigned)tx, (unsigned)(ty + TAB_H - 1),
               (unsigned)CONT_W, C_TAB_BORDER);
}

static void draw_content_bg(void)
{
    gfx_fill((unsigned)cont_x(), (unsigned)cont_y(),
              (unsigned)CONT_W, (unsigned)CONT_H, C_VIEW_BG);
}

/* ── Tab click hit-test ───────────────────────────────────────────────── */

static int tab_hit(int mx, int my)
{
    int ty = g_win_y + TITLE_H;
    if (my < ty || my >= ty + TAB_H) return -1;
    int tab_w = CONT_W / VIEW_COUNT;
    int tx    = g_win_x + CONT_PAD;
    if (mx < tx || mx >= tx + CONT_W) return -1;
    int idx = (mx - tx) / tab_w;
    if (idx >= VIEW_COUNT) idx = VIEW_COUNT - 1;
    return idx;
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    /* Register window with WM */
    g_win_id = (int)sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "AetherGuitar");

    /* Allocate GPU Buffer Object so the compositor can composite this window.
     * Falls back to malloc if the GPU allocator is unavailable. */
    {
        unsigned bo_bytes = (unsigned)WIN_W * (unsigned)WIN_H * 4u;
        g_win_bo = gpu_alloc(bo_bytes);
        if (g_win_bo != GPU_BO_INVALID) {
            void *bo_ptr = gpu_map(g_win_bo);
            if (bo_ptr) {
                g_fb = (unsigned *)bo_ptr;
                sys_wm_set_buffer(g_win_id, g_win_bo);
                gfx_set_damage_target(g_win_id);
            } else {
                gpu_free(g_win_bo);
                g_win_bo = GPU_BO_INVALID;
            }
        }
        if (g_win_bo == GPU_BO_INVALID) {
            g_fb = (unsigned *)malloc(bo_bytes);
            if (!g_fb) { sys_exit(1); return 1; }
        }
    }

    /* Initialise audio chain */
    chain_init();

    /* Initialise views */
    view_board_init();
    view_amp_init();
    view_presets_init();

    /* Open audio client (48000 Hz, stereo, 256-frame period) */
    g_audio = audio_client_open(g_sr, 2, g_period);
    if (g_audio) {
        audio_client_set_process_callback(g_audio, audio_cb, (void *)0);
        audio_client_activate(g_audio);
    }

    /* Initial draw — board has full coverage background, no need for draw_content_bg */
    gfx_begin_frame(g_fb, WIN_W, WIN_H, g_win_x, g_win_y);
    draw_chrome();
    draw_tabbar();
    view_board_draw(cont_x(), cont_y());
    gfx_end_frame();

    /* ── Main event loop ── */
    unsigned prev_btn  = 0;
    unsigned cur_btn   = 0;
    int      view_dirty = 1;   /* 1 = must redraw; cleared after each render */

    while (g_running) {
        /* Audio poll — runs the DSP callback when a full period of capture
         * data is ready.  run_once() returns immediately if the period hasn't
         * elapsed yet, so calling it every iteration is safe. */
        if (g_audio) audio_client_run_once(g_audio);

        int had_event = 0;
        unsigned long long raw;

        while ((raw = sys_wm_event_poll()) != 0) {
            had_event = 1;

            if ((raw >> 56) == WM_EV_CLOSE_REQUEST) {
                g_running = 0;
                break;
            }

            if (wm_event_is_redraw(raw)) {
                g_win_x    = wm_event_redraw_x(raw);
                g_win_y    = wm_event_redraw_y(raw);
                view_dirty = 1;   /* compositor uncovered/moved our window */
                continue;
            }

            if (wm_event_is_mouse(raw)) {
                mouse_event_t mev = wm_event_mouse_unpack(raw);
                int mx = (int)mev.x, my = (int)mev.y;
                prev_btn = cur_btn;
                cur_btn  = mev.buttons;

                /* Pure cursor movement with no button held needs no redraw.
                 * Any button activity (press, hold/drag, release) does. */
                if (cur_btn != prev_btn || (cur_btn & 1))
                    view_dirty = 1;

                /* Tab click */
                if ((cur_btn & 1) && !(prev_btn & 1)) {
                    int t = tab_hit(mx, my);
                    if (t >= 0 && t != g_view) {
                        g_view     = t;
                        view_dirty = 1;
                        continue;
                    }
                }

                /* Dispatch to current view */
                switch (g_view) {
                case VIEW_BOARD:
                    view_board_mouse(mx, my, cur_btn, prev_btn);   break;
                case VIEW_AMP:
                    view_amp_mouse(mx, my, cur_btn, prev_btn);     break;
                case VIEW_PRESETS:
                    view_presets_mouse(mx, my, cur_btn, prev_btn); break;
                case VIEW_SETTINGS:
                    view_settings_mouse(mx, my, cur_btn, prev_btn);break;
                default: break;
                }
                continue;
            }

            /* Key event */
            key_event_t kev = key_event_unpack(raw);
            if (!kev.is_press) continue;
            view_dirty = 1;
            if (kev.keycode == KEY_ESC) { g_running = 0; break; }
            if (g_view == VIEW_PRESETS)
                view_presets_key(kev.keycode, kev.modifiers);
        }

        /* Always begin a frame so gfx_end_frame can re-blit our window if the
         * compositor overwrote it (legacy FB mode).  gfx_end_frame is a no-op
         * when g_rt_dirty==0, so skipping draw functions costs almost nothing. */
        gfx_begin_frame(g_fb, WIN_W, WIN_H, g_win_x, g_win_y);

        int did_render = view_dirty || (g_view == VIEW_TUNER);
        if (did_render) {
            draw_chrome();
            draw_tabbar();
            /* Board and Tuner draw their own full-coverage backgrounds. */
            if (g_view != VIEW_BOARD && g_view != VIEW_TUNER)
                draw_content_bg();

            switch (g_view) {
            case VIEW_BOARD:    view_board_draw(cont_x(), cont_y());    break;
            case VIEW_AMP:      view_amp_draw(cont_x(), cont_y());      break;
            case VIEW_TUNER:    view_tuner_draw(cont_x(), cont_y());    break;
            case VIEW_PRESETS:  view_presets_draw(cont_x(), cont_y()); break;
            case VIEW_SETTINGS: view_settings_draw(cont_x(), cont_y());break;
            }

            view_dirty = 0;
        }

        gfx_end_frame();

        /* After a real render, sync to vsync so we don't render faster than
         * the display can show.  Otherwise yield briefly so the event queue
         * drains as fast as possible (pure mouse-move stays near zero cost). */
        if (did_render)
            sys_vsync_wait();
        else if (had_event)
            sys_sched_yield();
        else
            sys_vsync_wait();
    }

    /* Clean up */
    if (g_audio) {
        audio_client_deactivate(g_audio);
        audio_client_close(g_audio);
    }
    if (g_chain)  achain_destroy(g_chain);
    if (g_win_bo != GPU_BO_INVALID) gpu_free(g_win_bo);
    else if (g_fb) free(g_fb);
    if (g_win_id >= 0) sys_wm_unregister(g_win_id);
    sys_exit(0);
    return 0;
}
