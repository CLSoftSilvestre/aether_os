/*
 * AetherOS — AetherFiles
 * File: userspace/apps/files/main.c
 *
 * Phase 4.6: registers with the WM, takes focus on startup, and uses
 * sys_wm_key_recv() for focus-routed keyboard input.  Handles WM_EV_REDRAW
 * so the window can be repositioned via init's drag gesture.
 *
 * Also adds Ctrl+C handling so wait_foreground() in aether_term works
 * correctly when files is the foreground child.
 */

#include <gfx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys.h>
#include <input.h>

/* ── Layout constants (sizes only — position is runtime) ────────────────── */

#define TOPBAR_H   36
#define ACCENT_H    2
#define BOTBAR_Y  744

#define FONT_W  8
#define FONT_H  8

#define TERM_COLS  80
#define WIN_W     (TERM_COLS * FONT_W + 16)
#define TITLE_H    28
#define WIN_H     (TITLE_H + 8 + 70 * FONT_H + 8)

#define WIN_X_INIT  8
#define WIN_Y_INIT  (TOPBAR_H + ACCENT_H + \
                     (BOTBAR_Y - TOPBAR_H - ACCENT_H - WIN_H) / 2)

static int g_win_x = WIN_X_INIT;
static int g_win_y = WIN_Y_INIT;
static long g_win_id = -1;

/* Position macros */
#define WX  g_win_x
#define WY  g_win_y

/* Inner listing area (derived from WX/WY) */
#define LIST_X    (g_win_x + 12)
#define LIST_Y    (g_win_y + TITLE_H + 8)
#define LIST_W    (WIN_W - 24)
#define LIST_ROWS  68

/* ── File list ──────────────────────────────────────────────────────────── */

#define MAX_FILES  128
#define MAX_NAME    64

static char  file_names[MAX_FILES][MAX_NAME];
static int   file_count;

static void load_files(void)
{
    char buf[4096];
    long n = sys_initrd_ls(buf, sizeof(buf) - 1);
    if (n <= 0) { file_count = 0; return; }
    buf[n] = '\0';

    file_count = 0;
    char *line = buf;
    while (*line && file_count < MAX_FILES) {
        char *end = line;
        while (*end && *end != '\n') end++;

        int len = (int)(end - line);
        if (len > 0 && len < MAX_NAME) {
            for (int i = 0; i < len; i++) file_names[file_count][i] = line[i];
            file_names[file_count][len] = '\0';
            file_count++;
        }
        if (*end == '\n') end++;
        line = end;
    }
}

/* ── Drawing ────────────────────────────────────────────────────────────── */

static void draw_frame(void)
{
    gfx_fill(WX + 4, WY + 4, WIN_W, WIN_H, GFX_RGB(8, 8, 14));
    gfx_fill(WX, WY, WIN_W, WIN_H, C_WIN_BG);
    gfx_fill(WX, WY, WIN_W, TITLE_H, C_TITLEBAR);
    gfx_fill(WX + 10, WY + 8, 12, 12, C_RED);
    gfx_fill(WX + 26, WY + 8, 12, 12, C_YELLOW);
    gfx_fill(WX + 42, WY + 8, 12, 12, C_GREEN);
    gfx_text_center(WX, WIN_W, WY + 8,
                    "AetherFiles  --  initrd", C_TEXT, C_TITLEBAR);
    gfx_hline(WX, WY + TITLE_H, WIN_W, C_ACCENT);
    gfx_fill(WX, WY + TITLE_H + 1,
             WIN_W, WIN_H - TITLE_H - 1, C_TERM_BG);
    gfx_rect(WX, WY, WIN_W, WIN_H, C_SEP);

    /* Header row */
    gfx_fill(LIST_X - 4, LIST_Y, LIST_W + 8, FONT_H + 4, C_PANEL);
    gfx_text(LIST_X, LIST_Y + 2, "Name", C_TEXT_DIM, C_PANEL);
    gfx_hline(LIST_X - 4, LIST_Y + FONT_H + 4, LIST_W + 8, C_SEP);

    /* Help hint */
    int hint_y = WY + WIN_H - FONT_H - 6;
    gfx_fill(WX + 1, hint_y - 4, WIN_W - 2, FONT_H + 8, C_PANEL);
    gfx_hline(WX, hint_y - 4, WIN_W, C_SEP);
    gfx_text(LIST_X, hint_y,
             "j/k: navigate   v: view   q: quit", C_TEXT_DIM, C_PANEL);
}

static int list_start;
static int list_sel;

#define ENTRY_ROWS  (LIST_ROWS - 3)
#define ENTRY_Y0    (LIST_Y + FONT_H + 6)

static void draw_entry(int idx, int is_sel)
{
    int vis = idx - list_start;
    if (vis < 0 || vis >= ENTRY_ROWS) return;

    int y  = ENTRY_Y0 + vis * (FONT_H + 2);
    unsigned bg = is_sel ? C_ACCENT : C_TERM_BG;
    unsigned fg = is_sel ? C_TERM_BG : C_TEXT;

    gfx_fill(LIST_X - 4, y, LIST_W + 8, FONT_H + 2, bg);

    char line[MAX_NAME + 4];
    snprintf(line, sizeof(line), "  %s", file_names[idx]);
    gfx_text(LIST_X, y + 1, line, fg, bg);
}

static void draw_list(void)
{
    gfx_fill(LIST_X - 4, ENTRY_Y0,
             LIST_W + 8, ENTRY_ROWS * (FONT_H + 2), C_TERM_BG);

    int end = list_start + ENTRY_ROWS;
    if (end > file_count) end = file_count;
    for (int i = list_start; i < end; i++)
        draw_entry(i, i == list_sel);

    if (file_count > ENTRY_ROWS) {
        char scroll[24];
        snprintf(scroll, sizeof(scroll), "%d/%d", list_sel + 1, file_count);
        int sx = LIST_X + LIST_W - (int)strlen(scroll) * FONT_W - 4;
        gfx_text(sx, LIST_Y + 2, scroll, C_TEXT_DIM, C_PANEL);
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    load_files();

    list_start = 0;
    list_sel   = 0;

    draw_frame();
    draw_list();

    /* Register with WM and take keyboard focus */
    g_win_id = sys_wm_register(WX, WY, WIN_W, WIN_H, "AetherFiles");
    sys_wm_focus_set(sys_getpid());

    for (;;) {
        unsigned long long raw = sys_wm_key_recv();

        /* WM_EV_REDRAW: window was dragged — repaint at new position */
        if (wm_event_is_redraw(raw)) {
            g_win_x = wm_event_redraw_x(raw);
            g_win_y = wm_event_redraw_y(raw);
            draw_frame();
            draw_list();
            continue;
        }

        key_event_t ev = key_event_unpack(raw);
        if (!ev.is_press) continue;

        /* Ctrl+C — exit cleanly so aether_term's wait_foreground unblocks */
        if ((ev.modifiers & MOD_CTRL) && ev.keycode == KEY_C) {
            if (g_win_id >= 0) sys_wm_unregister(g_win_id);
            exit(130);
        }

        /* q / ESC — quit */
        if (ev.keycode == KEY_Q || ev.keycode == KEY_ESC) {
            if (g_win_id >= 0) sys_wm_unregister(g_win_id);
            return 0;
        }

        /* j / Down — move selection down */
        if ((ev.keycode == KEY_J || ev.keycode == KEY_DOWN) && file_count > 0) {
            int old = list_sel;
            list_sel++;
            if (list_sel >= file_count) list_sel = file_count - 1;
            if (list_sel >= list_start + ENTRY_ROWS) {
                list_start++;
                draw_list();
            } else {
                draw_entry(old, 0);
                draw_entry(list_sel, 1);
            }
            continue;
        }

        /* k / Up — move selection up */
        if ((ev.keycode == KEY_K || ev.keycode == KEY_UP) && file_count > 0) {
            int old = list_sel;
            list_sel--;
            if (list_sel < 0) list_sel = 0;
            if (list_sel < list_start) {
                list_start--;
                if (list_start < 0) list_start = 0;
                draw_list();
            } else {
                draw_entry(old, 0);
                draw_entry(list_sel, 1);
            }
            continue;
        }

        /* v / Enter — view selected file */
        if ((ev.keycode == KEY_V || ev.keycode == KEY_ENTER) && file_count > 0) {
            long child = sys_spawn("/textviewer");
            if (child > 0) {
                int status = 0;
                sys_waitpid(child, &status);
                draw_frame();
                draw_list();
            }
            continue;
        }
    }
}
