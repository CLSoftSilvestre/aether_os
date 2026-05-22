/*
 * AetherGuitar — Preset browser (Phase 8.10)
 * File: userspace/apps/aether_guitar/view_presets.c
 *
 * Lists .aepre files from /presets/, allows load/save/delete.
 * Preset blobs are saved via sys_fs_create + sys_fs_write.
 */

#include "aeguitar.h"

#define MAX_PRESETS   32
#define NAME_BUF      64
#define PRESET_DIR    "/presets"

/* ── Preset list ─────────────────────────────────────────────────────── */

static char g_preset_names[MAX_PRESETS][NAME_BUF];
static int  g_preset_count = 0;
static int  g_selected     = -1;

/* Text input for save name */
static char g_save_name[NAME_BUF];
static int  g_save_len    = 0;
static int  g_save_focus  = 0;

/* Status message (shown 1-3 seconds) */
static char g_status[80];

/* ── Directory scan ───────────────────────────────────────────────────── */

static void scan_presets(void)
{
    g_preset_count = 0;
    char dirbuf[4096];
    long n = sys_fs_readdir(PRESET_DIR, dirbuf, sizeof(dirbuf) - 1);
    if (n <= 0) return;
    dirbuf[n] = '\0';

    /* dirbuf contains null-terminated filenames packed end-to-end */
    char *p = dirbuf;
    while (p < dirbuf + n && g_preset_count < MAX_PRESETS) {
        int len = (int)strlen(p);
        if (len > 6 && strcmp(p + len - 6, ".aepre") == 0) {
            /* Strip extension for display */
            int dlen = len - 6;
            if (dlen >= NAME_BUF) dlen = NAME_BUF - 1;
            memcpy(g_preset_names[g_preset_count], p, (unsigned)dlen);
            g_preset_names[g_preset_count][dlen] = '\0';
            g_preset_count++;
        }
        p += len + 1;
    }
}

/* ── Load selected preset ────────────────────────────────────────────── */

static void do_load(void)
{
    if (g_selected < 0 || g_selected >= g_preset_count) {
        snprintf(g_status, sizeof(g_status), "No preset selected.");
        return;
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.aepre",
             PRESET_DIR, g_preset_names[g_selected]);

    long fd = sys_fs_open(path);
    if (fd < 0) {
        snprintf(g_status, sizeof(g_status), "Open failed: %s", path);
        return;
    }
    unsigned char blob[8192];
    long sz = sys_fs_read(fd, blob, (long)sizeof(blob));
    sys_fs_close(fd);
    if (sz <= 0) {
        snprintf(g_status, sizeof(g_status), "Read failed.");
        return;
    }
    int rc = achain_preset_load(g_chain, blob, (int)sz);
    if (rc == 0)
        snprintf(g_status, sizeof(g_status), "Loaded: %s",
                 g_preset_names[g_selected]);
    else
        snprintf(g_status, sizeof(g_status), "Load error (format?)");
}

/* ── Save current chain as preset ────────────────────────────────────── */

static void do_save(void)
{
    if (g_save_len == 0) {
        snprintf(g_status, sizeof(g_status), "Enter a preset name first.");
        return;
    }
    int out_len = 0;
    unsigned char *blob = achain_preset_save(g_chain, &out_len);
    if (!blob) {
        snprintf(g_status, sizeof(g_status), "Save failed (OOM?)");
        return;
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.aepre", PRESET_DIR, g_save_name);

    long fd = sys_fs_create(path);
    if (fd < 0) {
        achain_preset_free_blob(blob);
        snprintf(g_status, sizeof(g_status), "Create failed.");
        return;
    }
    sys_fs_write(fd, blob, (long)out_len);
    sys_fs_close(fd);
    achain_preset_free_blob(blob);

    snprintf(g_status, sizeof(g_status), "Saved: %s", g_save_name);
    scan_presets();
}

/* ── Init ─────────────────────────────────────────────────────────────── */

void view_presets_init(void)
{
    memset(g_save_name, 0, sizeof(g_save_name));
    snprintf(g_status, sizeof(g_status), "Ready.");
    scan_presets();
}

/* ── Draw ─────────────────────────────────────────────────────────────── */

#define LIST_W     320
#define LIST_ITEM_H 22

void view_presets_draw(int cx, int cy)
{
    (void)cx; (void)cy;
    int bx = cont_x(), by = cont_y();

    /* Section header */
    gfx_fill((unsigned)bx, (unsigned)by,
              (unsigned)CONT_W, 24, C_SECTION_HDR);
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)(by + 4),
                         "Preset Browser", C_VALUE);

    /* ── Left: preset list ── */
    int lx = bx + 8, ly = by + 30;
    int lh = CONT_H - 30 - 40;    /* leave room for status bar */

    gfx_fill_rounded((unsigned)lx, (unsigned)ly,
                     (unsigned)LIST_W, (unsigned)lh, 4, GFX_RGB(12, 12, 20));
    gfx_rect_rounded((unsigned)lx, (unsigned)ly,
                     (unsigned)LIST_W, (unsigned)lh, 4, GFX_RGB(50, 50, 80));

    if (g_preset_count == 0) {
        gfx_text_transparent((unsigned)(lx + 8), (unsigned)(ly + 8),
                             "(No presets found)", GFX_RGB(60, 60, 80));
    } else {
        int max_vis = lh / LIST_ITEM_H;
        for (int i = 0; i < g_preset_count && i < max_vis; i++) {
            int iy  = ly + i * LIST_ITEM_H;
            if (i == g_selected)
                gfx_fill((unsigned)lx, (unsigned)iy,
                          (unsigned)LIST_W, (unsigned)LIST_ITEM_H,
                          GFX_RGB(45, 42, 90));
            gfx_text_transparent((unsigned)(lx + 8),
                                 (unsigned)(iy + 3),
                                 g_preset_names[i], C_VALUE);
        }
    }

    /* ── Right: buttons + name input ── */
    int rx = bx + LIST_W + 24;
    int ry = by + 30;

    gfx_text_transparent((unsigned)rx, (unsigned)ry, "Actions:", C_LABEL);

    draw_btn(rx, ry + 22, 120, 26, "Load Preset", 0);
    draw_btn(rx, ry + 58, 120, 26, "Refresh List", 0);
    draw_btn(rx, ry + 94, 120, 26, "Delete", 0);

    /* Save section */
    gfx_text_transparent((unsigned)rx, (unsigned)(ry + 140),
                         "Save as:", C_LABEL);
    unsigned inp_bg = g_save_focus ? GFX_RGB(18, 18, 36)
                                   : GFX_RGB(12, 12, 22);
    gfx_fill_rounded((unsigned)rx, (unsigned)(ry + 158),
                     (unsigned)(CONT_W - LIST_W - 40), 22, 3, inp_bg);
    gfx_rect_rounded((unsigned)rx, (unsigned)(ry + 158),
                     (unsigned)(CONT_W - LIST_W - 40), 22, 3,
                     g_save_focus ? C_ACCENT : GFX_RGB(50, 50, 80));
    gfx_text_transparent((unsigned)(rx + 4), (unsigned)(ry + 161),
                         g_save_name, C_VALUE);
    /* Cursor */
    if (g_save_focus) {
        int curs_x = rx + 4 + g_save_len * 8;
        gfx_vline((unsigned)curs_x, (unsigned)(ry + 160), 16,
                  GFX_RGB(180, 180, 220));
    }

    draw_btn(rx, ry + 188, 120, 26, "Save Preset", 0);

    /* ── Status bar ── */
    int st_y = by + CONT_H - 22;
    gfx_fill((unsigned)bx, (unsigned)st_y,
              (unsigned)CONT_W, 20, GFX_RGB(16, 16, 26));
    gfx_text_transparent((unsigned)(bx + 8), (unsigned)(st_y + 2),
                         g_status, GFX_RGB(140, 200, 140));
}

/* ── Mouse ────────────────────────────────────────────────────────────── */

void view_presets_mouse(int mx, int my, unsigned btn, unsigned prev_btn)
{
    int pressed = (btn & 1) && !(prev_btn & 1);
    if (!pressed) return;

    int bx = cont_x(), by = cont_y();
    int lx = bx + 8, ly = by + 30;
    int rx = bx + LIST_W + 24, ry = by + 30;

    /* List item click */
    if (mx >= lx && mx < lx + LIST_W) {
        int item = (my - ly) / LIST_ITEM_H;
        if (item >= 0 && item < g_preset_count) {
            g_selected  = item;
            g_save_focus = 0;
        }
    }

    /* Button clicks */
    if (btn_hit(rx, ry + 22, 120, 26, mx, my)) do_load();
    if (btn_hit(rx, ry + 58, 120, 26, mx, my)) scan_presets();
    if (btn_hit(rx, ry + 188, 120, 26, mx, my)) do_save();

    /* Save name input focus */
    int inp_w = CONT_W - LIST_W - 40;
    if (btn_hit(rx, ry + 158, inp_w, 22, mx, my)) g_save_focus = 1;
    else g_save_focus = 0;
}

/* ── Key input (for save name textbox) ──────────────────────────────── */

void view_presets_key(keycode_t k, unsigned mods)
{
    (void)mods;
    if (!g_save_focus) return;

    if (k == KEY_BACKSPACE) {
        if (g_save_len > 0) g_save_name[--g_save_len] = '\0';
        return;
    }

    /* Map keycode to ASCII (simplified: A-Z, 0-9, space, minus) */
    char c = 0;
    if (k >= KEY_A && k <= KEY_Z)
        c = (char)('a' + (k - KEY_A));
    else if (k >= KEY_0 && k <= KEY_9)
        c = (char)('0' + (k - KEY_0));
    else if (k == KEY_SPACE)   c = '_';
    else if (k == KEY_MINUS)   c = '-';

    if (c && g_save_len < NAME_BUF - 2) {
        g_save_name[g_save_len++] = c;
        g_save_name[g_save_len]   = '\0';
    }
}
