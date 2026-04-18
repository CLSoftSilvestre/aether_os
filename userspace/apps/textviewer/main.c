/*
 * AetherOS — TextViewer
 * File: userspace/apps/textviewer/main.c
 *
 * Phase 4.4: Simple text file pager for initrd files.
 * Prompts the user for a filename, reads it from initrd, and
 * displays it in the terminal window area with j/k/q controls.
 *
 * Controls:
 *   j / Enter  — scroll down one line
 *   k          — scroll up one line
 *   d          — scroll down half page
 *   u          — scroll up half page
 *   q / ESC    — quit
 */

#include <gfx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys.h>

/* ── Layout ─────────────────────────────────────────────────────────────── */

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

#define TEXT_X    (WIN_X + 10)
#define TEXT_Y    (WIN_Y + TITLE_H + 8)
#define TEXT_COLS  78
#define TEXT_ROWS  67

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static char title_buf[80];

static void draw_frame(void)
{
    gfx_fill(WIN_X + 4, WIN_Y + 4, WIN_W, WIN_H, GFX_RGB(8, 8, 14));
    gfx_fill(WIN_X, WIN_Y, WIN_W, WIN_H, C_WIN_BG);
    gfx_fill(WIN_X, WIN_Y, WIN_W, TITLE_H, C_TITLEBAR);
    gfx_fill(WIN_X + 10, WIN_Y + 8, 12, 12, C_RED);
    gfx_fill(WIN_X + 26, WIN_Y + 8, 12, 12, C_YELLOW);
    gfx_fill(WIN_X + 42, WIN_Y + 8, 12, 12, C_GREEN);
    gfx_text_center(WIN_X, WIN_W, WIN_Y + 8, title_buf, C_TEXT, C_TITLEBAR);
    gfx_hline(WIN_X, WIN_Y + TITLE_H, WIN_W, C_ACCENT);
    gfx_fill(WIN_X, WIN_Y + TITLE_H + 1,
             WIN_W, WIN_H - TITLE_H - 1, C_TERM_BG);
    gfx_rect(WIN_X, WIN_Y, WIN_W, WIN_H, C_SEP);
}

/* ── Line index ─────────────────────────────────────────────────────────── */

#define MAX_LINES  1024

static const char *lines[MAX_LINES];
static int         nlines;
static char        filebuf[16384];

/* Split filebuf into line pointers in-place */
static void index_lines(void)
{
    nlines = 0;
    char *p = filebuf;
    lines[nlines++] = p;
    while (*p && nlines < MAX_LINES) {
        if (*p == '\n') {
            *p = '\0';
            if (*(p + 1)) lines[nlines++] = p + 1;
        }
        p++;
    }
}

/* ── Rendering ──────────────────────────────────────────────────────────── */

static int top_line;   /* index of the first visible line */

static void draw_line(int line_idx, int vis_row)
{
    int y = TEXT_Y + vis_row * FONT_H;
    gfx_fill(TEXT_X, y, TEXT_COLS * FONT_W, FONT_H, C_TERM_BG);
    if (line_idx >= nlines) return;

    const char *s = lines[line_idx];
    int col = 0;
    while (*s && col < TEXT_COLS) {
        gfx_char(TEXT_X + col * FONT_W, y, *s, C_TEXT, C_TERM_BG);
        s++;
        col++;
    }
}

static void draw_page(void)
{
    for (int r = 0; r < TEXT_ROWS; r++)
        draw_line(top_line + r, r);

    /* Status bar */
    int sb_y = TEXT_Y + TEXT_ROWS * FONT_H + 2;
    gfx_fill(WIN_X + 1, sb_y, WIN_W - 2, FONT_H + 4, C_PANEL);
    gfx_hline(WIN_X, sb_y, WIN_W, C_SEP);
    char status[64];
    int total = nlines > 0 ? nlines : 1;
    int pct = (top_line + TEXT_ROWS) * 100 / total;
    if (pct > 100) pct = 100;
    snprintf(status, sizeof(status),
             "line %d/%d  %d%%   j/k: scroll   q: quit",
             top_line + 1, nlines, pct);
    gfx_text(TEXT_X, sb_y + 2, status, C_TEXT_DIM, C_PANEL);
}

/* ── Prompt helpers ─────────────────────────────────────────────────────── */

static void prompt_draw(const char *label, const char *input)
{
    int y = TEXT_Y + 2;
    gfx_fill(WIN_X + 1, WIN_Y + TITLE_H + 1,
             WIN_W - 2, WIN_H - TITLE_H - 2, C_TERM_BG);
    gfx_text(TEXT_X, y, label, C_TEXT_DIM, C_TERM_BG);
    gfx_text(TEXT_X + (int)strlen(label) * FONT_W + 4, y,
             input, C_TEXT, C_TERM_BG);
    /* cursor */
    int cx = TEXT_X + ((int)strlen(label) + (int)strlen(input) + 1) * FONT_W + 4;
    gfx_fill(cx, y, FONT_W, FONT_H, C_ACCENT);
}

static int readline_prompt(const char *label, char *buf, int max)
{
    int n = 0;
    buf[0] = '\0';
    prompt_draw(label, buf);

    while (n < max - 1) {
        char c;
        sys_read(STDIN_FILENO, &c, 1);
        if (c == '\r' || c == '\n') break;
        if ((c == '\b' || c == 127) && n > 0) {
            buf[--n] = '\0';
        } else if ((unsigned char)c >= 32) {
            buf[n++] = c;
            buf[n]   = '\0';
        }
        prompt_draw(label, buf);
    }
    return n;
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    /* Ask for filename */
    snprintf(title_buf, sizeof(title_buf), "TextViewer");
    draw_frame();

    char fname[64];
    int flen = readline_prompt("File: ", fname, sizeof(fname));
    if (flen == 0) return 0;

    /* Load from initrd */
    long n = sys_initrd_read(fname, filebuf, (long)sizeof(filebuf) - 1);
    if (n < 0) {
        snprintf(title_buf, sizeof(title_buf), "TextViewer  --  not found");
        draw_frame();
        gfx_text(TEXT_X, TEXT_Y, "File not found in initrd.", C_RED, C_TERM_BG);
        gfx_text(TEXT_X, TEXT_Y + FONT_H + 4,
                 "Press any key to close.", C_TEXT_DIM, C_TERM_BG);
        char c; sys_read(STDIN_FILENO, &c, 1);
        return 1;
    }
    filebuf[n] = '\0';

    snprintf(title_buf, sizeof(title_buf), "TextViewer  --  %s", fname);
    draw_frame();
    index_lines();

    top_line = 0;
    draw_page();

    for (;;) {
        char c;
        sys_read(STDIN_FILENO, &c, 1);

        if (c == 'q' || c == 27) break;

        if (c == 'j' || c == '\r' || c == '\n') {
            if (top_line + TEXT_ROWS < nlines) {
                top_line++;
                /* Shift: scroll one line */
                draw_page();
            }
        } else if (c == 'k') {
            if (top_line > 0) {
                top_line--;
                draw_page();
            }
        } else if (c == 'd') {
            top_line += TEXT_ROWS / 2;
            if (top_line + TEXT_ROWS > nlines)
                top_line = nlines - TEXT_ROWS;
            if (top_line < 0) top_line = 0;
            draw_page();
        } else if (c == 'u') {
            top_line -= TEXT_ROWS / 2;
            if (top_line < 0) top_line = 0;
            draw_page();
        }
    }

    return 0;
}
