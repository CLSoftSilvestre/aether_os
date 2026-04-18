/*
 * AetherOS — AetherFiles
 * File: userspace/apps/files/main.c
 *
 * Phase 4.4: Graphical file browser over the terminal window area.
 * Launched by aether_term (spawn + waitpid), so it has exclusive use
 * of the terminal area while running.
 *
 * Controls:
 *   j / down  — move selection down
 *   k / up    — move selection up
 *   v         — view selected file in textviewer (spawns child)
 *   q / ESC   — quit, return to aether_term
 */

#include <gfx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys.h>

/* ── Layout (terminal window area) ─────────────────────────────────────── */

#define TOPBAR_H   36
#define ACCENT_H    2
#define BOTBAR_Y  744

#define FONT_W  8
#define FONT_H  8

#define TERM_COLS  80
#define WIN_W     (TERM_COLS * FONT_W + 16)
#define TITLE_H    28
#define WIN_H     (TITLE_H + 8 + 70 * FONT_H + 8)
#define WIN_X      8
#define WIN_Y     (TOPBAR_H + ACCENT_H + \
                   (BOTBAR_Y - TOPBAR_H - ACCENT_H - WIN_H) / 2)

/* Inner listing area */
#define LIST_X    (WIN_X + 12)
#define LIST_Y    (WIN_Y + TITLE_H + 8)
#define LIST_W    (WIN_W - 24)
#define LIST_ROWS  68         /* visible rows */

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
    /* Shadow */
    gfx_fill(WIN_X + 4, WIN_Y + 4, WIN_W, WIN_H, GFX_RGB(8, 8, 14));
    /* Window body */
    gfx_fill(WIN_X, WIN_Y, WIN_W, WIN_H, C_WIN_BG);
    /* Title bar */
    gfx_fill(WIN_X, WIN_Y, WIN_W, TITLE_H, C_TITLEBAR);
    gfx_fill(WIN_X + 10, WIN_Y + 8, 12, 12, C_RED);
    gfx_fill(WIN_X + 26, WIN_Y + 8, 12, 12, C_YELLOW);
    gfx_fill(WIN_X + 42, WIN_Y + 8, 12, 12, C_GREEN);
    gfx_text_center(WIN_X, WIN_W, WIN_Y + 8,
                    "AetherFiles  --  initrd", C_TEXT, C_TITLEBAR);
    gfx_hline(WIN_X, WIN_Y + TITLE_H, WIN_W, C_ACCENT);
    gfx_fill(WIN_X, WIN_Y + TITLE_H + 1,
             WIN_W, WIN_H - TITLE_H - 1, C_TERM_BG);
    gfx_rect(WIN_X, WIN_Y, WIN_W, WIN_H, C_SEP);

    /* Header row */
    gfx_fill(LIST_X - 4, LIST_Y, LIST_W + 8, FONT_H + 4, C_PANEL);
    gfx_text(LIST_X, LIST_Y + 2, "Name", C_TEXT_DIM, C_PANEL);
    gfx_hline(LIST_X - 4, LIST_Y + FONT_H + 4, LIST_W + 8, C_SEP);

    /* Help hint at bottom */
    int hint_y = WIN_Y + WIN_H - FONT_H - 6;
    gfx_fill(WIN_X + 1, hint_y - 4, WIN_W - 2, FONT_H + 8, C_PANEL);
    gfx_hline(WIN_X, hint_y - 4, WIN_W, C_SEP);
    gfx_text(LIST_X, hint_y,
             "j/k: navigate   v: view   q: quit", C_TEXT_DIM, C_PANEL);
}

static int list_start;    /* first visible row index */
static int list_sel;      /* selected row index */

#define ENTRY_ROWS  (LIST_ROWS - 3)   /* leave room for header + hint */
#define ENTRY_Y0    (LIST_Y + FONT_H + 6)

static void draw_entry(int idx, int is_sel)
{
    int vis = idx - list_start;
    if (vis < 0 || vis >= ENTRY_ROWS) return;

    int y = ENTRY_Y0 + vis * (FONT_H + 2);
    unsigned bg = is_sel ? C_ACCENT : C_TERM_BG;
    unsigned fg = is_sel ? C_TERM_BG : C_TEXT;

    gfx_fill(LIST_X - 4, y, LIST_W + 8, FONT_H + 2, bg);

    /* File icon hint */
    char line[MAX_NAME + 4];
    snprintf(line, sizeof(line), "  %s", file_names[idx]);
    gfx_text(LIST_X, y + 1, line, fg, bg);
}

static void draw_list(void)
{
    /* Clear listing area */
    gfx_fill(LIST_X - 4, ENTRY_Y0,
             LIST_W + 8, ENTRY_ROWS * (FONT_H + 2), C_TERM_BG);

    int end = list_start + ENTRY_ROWS;
    if (end > file_count) end = file_count;
    for (int i = list_start; i < end; i++)
        draw_entry(i, i == list_sel);

    /* Scroll indicator */
    if (file_count > ENTRY_ROWS) {
        char scroll[24];
        snprintf(scroll, sizeof(scroll), "%d/%d", list_sel + 1, file_count);
        int sx = LIST_X + LIST_W - (int)strlen(scroll) * FONT_W - 4;
        int sy = LIST_Y + 2;
        gfx_text(sx, sy, scroll, C_TEXT_DIM, C_PANEL);
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

    for (;;) {
        char c;
        sys_read(STDIN_FILENO, &c, 1);

        if (c == 'q' || c == 27 /* ESC */) break;

        if (c == 'j') {
            if (file_count > 0) {
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
            }
        } else if (c == 'k') {
            if (file_count > 0) {
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
            }
        } else if ((c == '\r' || c == '\n' || c == 'v') && file_count > 0) {
            /* View selected file — spawn textviewer; it will ask for a filename */
            long child = sys_spawn("/textviewer");
            if (child > 0) {
                int status = 0;
                sys_waitpid(child, &status);
                /* Redraw files UI after textviewer exits */
                draw_frame();
                draw_list();
            }
        }
    }

    return 0;
}
