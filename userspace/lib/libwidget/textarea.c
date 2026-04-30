/*
 * AetherOS libwidget — WIDGET_TEXTAREA (Phase 5.3)
 * File: userspace/lib/libwidget/textarea.c
 *
 * Multi-line editable text area with:
 *   - Dynamic line buffer (malloc'd at init, WGT_TEXTAREA_LINE_LEN chars each)
 *   - Vertical scroll via Up/Down arrows and Page Up/Down
 *   - Character insertion at (cur_row, cur_col), newline splits current line
 *   - Backspace deletes char before caret; at col 0 merges with previous line
 *   - Ctrl+A: move to beginning; Ctrl+C: copy all to clipboard; Ctrl+V: insert
 *   - get_text/set_text helpers for App Editor integration
 */

#include <widget.h>
#include <gfx.h>
#include <sys.h>
#include <string.h>
#include <stdlib.h>

#define TA_PAD_X   4
#define TA_PAD_Y   4
#define C_TA_BG    C_TERM_BG
#define C_TA_BDR   C_SEP
#define C_TA_BDF   C_ACCENT
#define C_TA_CUR   C_ACCENT
#define C_TA_LNUM  C_TEXT_DIM  /* line number colour */

static int visible_rows(widget_t *w)
{
    return (w->bounds.h - 2 * TA_PAD_Y) / WGT_FONT_H;
}

static int visible_cols(widget_t *w)
{
    return (w->bounds.w - 2 * TA_PAD_X) / WGT_FONT_W;
}

static void textarea_draw(widget_t *w, int ax, int ay)
{
    wdata_textarea_t *d = &w->data.textarea;
    int focused = (w->state == WS_FOCUSED || w->state == WS_PRESSED);
    int rows = visible_rows(w);
    int cols = visible_cols(w);

    gfx_fill(ax, ay, w->bounds.w, w->bounds.h, C_TA_BG);
    gfx_rect(ax, ay, w->bounds.w, w->bounds.h,
             focused ? C_TA_BDF : C_TA_BDR);

    int tx = ax + TA_PAD_X;
    int ty = ay + TA_PAD_Y;

    for (int r = 0; r < rows; r++) {
        int line_idx = d->scroll_top + r;
        int ly = ty + r * WGT_FONT_H;

        if (line_idx >= d->n_lines) break;

        char *line = d->lines[line_idx];
        int len = 0;
        while (line[len]) len++;

        for (int c = 0; c < cols && c < len; c++) {
            unsigned int fg = C_TEXT;
            unsigned int bg = C_TA_BG;

            /* Cursor highlight */
            if (focused && line_idx == d->cur_row && c == d->cur_col) {
                fg = C_TA_BG;
                bg = C_TA_CUR;
            }
            gfx_char(tx + c * WGT_FONT_W, ly, line[c], fg, bg);
        }

        /* Caret at end of line */
        if (focused && line_idx == d->cur_row && d->cur_col >= len && d->cur_col < cols) {
            gfx_fill(tx + d->cur_col * WGT_FONT_W, ly, 2, WGT_FONT_H, C_TA_CUR);
        }
    }
}

/* Insert a character at current cursor position */
static void ta_insert_char(wdata_textarea_t *d, char ch)
{
    if (d->cur_row >= d->n_lines) return;
    char *line = d->lines[d->cur_row];
    int len = 0;
    while (line[len]) len++;
    if (len >= WGT_TEXTAREA_LINE_LEN - 1) return;

    for (int i = len; i > d->cur_col; i--)
        line[i] = line[i - 1];
    line[d->cur_col] = ch;
    line[len + 1] = '\0';
    d->cur_col++;
}

/* Split current line at cursor (Enter key) */
static void ta_split_line(wdata_textarea_t *d)
{
    if (d->n_lines >= d->n_lines_max) return;
    char *cur  = d->lines[d->cur_row];
    int   col  = d->cur_col;

    /* Shift lines down (copy contents, bottom-up to avoid clobber) */
    for (int i = d->n_lines; i > d->cur_row + 1; i--)
        memcpy(d->lines[i], d->lines[i - 1], WGT_TEXTAREA_LINE_LEN);

    /* New line gets the tail of current line */
    char *newl = d->lines[d->cur_row + 1];
    int   tail_len = 0;
    while (cur[col + tail_len]) tail_len++;
    for (int i = 0; i <= tail_len; i++)
        newl[i] = cur[col + i];
    cur[col] = '\0';

    d->n_lines++;
    d->cur_row++;
    d->cur_col = 0;
}

/* Merge line d->cur_row with line d->cur_row - 1 (Backspace at col 0) */
static void ta_merge_lines(wdata_textarea_t *d)
{
    if (d->cur_row == 0) return;
    char *prev = d->lines[d->cur_row - 1];
    char *cur  = d->lines[d->cur_row];
    int prev_len = 0;
    while (prev[prev_len]) prev_len++;
    int cur_len  = 0;
    while (cur[cur_len])   cur_len++;
    if (prev_len + cur_len >= WGT_TEXTAREA_LINE_LEN - 1) return;

    for (int i = 0; i <= cur_len; i++)
        prev[prev_len + i] = cur[i];

    /* Shift lines up */
    for (int i = d->cur_row; i < d->n_lines - 1; i++)
        memcpy(d->lines[i], d->lines[i + 1], WGT_TEXTAREA_LINE_LEN);

    d->n_lines--;
    d->cur_row--;
    d->cur_col = prev_len;
}

static int textarea_event(widget_t *w, const widget_event_t *ev)
{
    wdata_textarea_t *d = &w->data.textarea;
    int rows = visible_rows(w);

    if (ev->type == WEV_FOCUS_IN || ev->type == WEV_FOCUS_OUT) {
        w->dirty = 1;
        return 0;
    }

    if (ev->type != WEV_KEY_DOWN) return 0;

    keycode_t kc  = ev->keycode;
    unsigned  mods = ev->modifiers;

    if (mods & MOD_CTRL) {
        if (kc == KEY_A) {
            d->cur_row = 0; d->cur_col = 0; d->scroll_top = 0;
            w->dirty = 1; return 1;
        }
        if (kc == KEY_C) {
            /* Copy all text to clipboard (up to 4KB) */
            char tmp[4096];
            textarea_get_text(w, tmp, (int)sizeof(tmp));
            int tlen = 0;
            while (tmp[tlen]) tlen++;
            sys_clipboard_write(tmp, (long)tlen);
            return 1;
        }
        if (kc == KEY_V) {
            char tmp[WGT_TEXTAREA_LINE_LEN];
            long n = sys_clipboard_read(tmp, (long)sizeof(tmp));
            for (long i = 0; i < n; i++) {
                if (tmp[i] == '\n')
                    ta_split_line(d);
                else
                    ta_insert_char(d, tmp[i]);
            }
            w->dirty = 1; return 1;
        }
        return 0;
    }

    /* Cursor movement */
    if (kc == KEY_UP && d->cur_row > 0) {
        d->cur_row--;
        if (d->cur_row < d->scroll_top) d->scroll_top = d->cur_row;
        w->dirty = 1; return 1;
    }
    if (kc == KEY_DOWN && d->cur_row < d->n_lines - 1) {
        d->cur_row++;
        if (d->cur_row >= d->scroll_top + rows)
            d->scroll_top = d->cur_row - rows + 1;
        w->dirty = 1; return 1;
    }
    if (kc == KEY_LEFT && d->cur_col > 0) {
        d->cur_col--; w->dirty = 1; return 1;
    }
    if (kc == KEY_RIGHT) {
        char *line = d->lines[d->cur_row];
        int len = 0; while (line[len]) len++;
        if (d->cur_col < len) { d->cur_col++; w->dirty = 1; return 1; }
    }
    if (kc == KEY_HOME) { d->cur_col = 0; w->dirty = 1; return 1; }
    if (kc == KEY_END) {
        char *line = d->lines[d->cur_row];
        int len = 0; while (line[len]) len++;
        d->cur_col = len; w->dirty = 1; return 1;
    }
    if (kc == KEY_PGUP) {
        d->cur_row -= rows;
        if (d->cur_row < 0) d->cur_row = 0;
        d->scroll_top -= rows;
        if (d->scroll_top < 0) d->scroll_top = 0;
        w->dirty = 1; return 1;
    }
    if (kc == KEY_PGDN) {
        d->cur_row += rows;
        if (d->cur_row >= d->n_lines) d->cur_row = d->n_lines - 1;
        d->scroll_top += rows;
        if (d->scroll_top + rows > d->n_lines)
            d->scroll_top = d->n_lines - rows;
        if (d->scroll_top < 0) d->scroll_top = 0;
        w->dirty = 1; return 1;
    }

    if (kc == KEY_ENTER) {
        ta_split_line(d);
        if (d->cur_row >= d->scroll_top + rows)
            d->scroll_top = d->cur_row - rows + 1;
        w->dirty = 1; return 1;
    }

    if (kc == KEY_BACKSPACE) {
        if (d->cur_col > 0) {
            char *line = d->lines[d->cur_row];
            int len = 0; while (line[len]) len++;
            for (int i = d->cur_col - 1; i < len - 1; i++)
                line[i] = line[i + 1];
            line[len - 1] = '\0';
            d->cur_col--;
        } else {
            ta_merge_lines(d);
            if (d->cur_row < d->scroll_top) d->scroll_top = d->cur_row;
        }
        w->dirty = 1; return 1;
    }

    if (kc == KEY_DELETE) {
        char *line = d->lines[d->cur_row];
        int len = 0; while (line[len]) len++;
        if (d->cur_col < len) {
            for (int i = d->cur_col; i < len - 1; i++)
                line[i] = line[i + 1];
            line[len - 1] = '\0';
            w->dirty = 1; return 1;
        }
    }

    /* Printable character */
    if (kc == KEY_SPACE) {
        ta_insert_char(d, ' ');
        w->dirty = 1; return 1;
    }
    if (kc >= KEY_A && kc <= KEY_Z) {
        int shift = (mods & MOD_SHIFT) || (mods & MOD_CAPS);
        char ch = (char)('a' + (kc - KEY_A));
        if (shift) ch -= 32;
        ta_insert_char(d, ch);
        w->dirty = 1; return 1;
    }
    if (kc >= KEY_0 && kc <= KEY_9) {
        int shift = (mods & MOD_SHIFT);
        static const char digs[]  = "0123456789";
        static const char sdigs[] = "=!\"#$%&/()"; /* PT-PT shifted row */
        ta_insert_char(d, shift ? sdigs[kc - KEY_0] : digs[kc - KEY_0]);
        w->dirty = 1; return 1;
    }

    /* Punctuation — PT-PT layout */
    {
        int shift = (mods & MOD_SHIFT);
        char ch = 0;
        if (!shift) {
            switch (kc) {
            case KEY_GRAVE:      ch = '\\';   break; /* PT: \ */
            case KEY_MINUS:      ch = '\'';   break; /* PT: ' */
            case KEY_LBRACKET:   ch = '+';    break; /* PT: + */
            case KEY_BACKSLASH:  ch = '~';    break; /* PT: dead-~ */
            case KEY_SEMICOLON:  ch = '\xe7'; break; /* PT: ç */
            case KEY_COMMA:      ch = ',';    break;
            case KEY_DOT:        ch = '.';    break;
            case KEY_SLASH:      ch = '-';    break; /* PT: - */
            default: break;
            }
        } else {
            switch (kc) {
            case KEY_GRAVE:      ch = '|';    break; /* PT: | */
            case KEY_MINUS:      ch = '?';    break; /* PT: ? */
            case KEY_LBRACKET:   ch = '*';    break; /* PT: * */
            case KEY_RBRACKET:   ch = '`';    break; /* PT: dead-` */
            case KEY_BACKSLASH:  ch = '^';    break; /* PT: dead-^ */
            case KEY_SEMICOLON:  ch = '\xc7'; break; /* PT: Ç */
            case KEY_COMMA:      ch = ';';    break; /* PT: ; */
            case KEY_DOT:        ch = ':';    break; /* PT: : */
            case KEY_SLASH:      ch = '_';    break; /* PT: _ */
            default: break;
            }
        }
        if (ch) { ta_insert_char(d, ch); w->dirty = 1; return 1; }
    }

    return 0;
}

void widget_init_textarea(widget_t *w, int x, int y, int width, int height,
                          int max_lines)
{
    widget_init(w, WIDGET_TEXTAREA, x, y, width, height);
    w->draw_fn   = textarea_draw;
    w->event_fn  = textarea_event;
    w->focusable = 1;

    wdata_textarea_t *d = &w->data.textarea;
    d->lines = (char (*)[WGT_TEXTAREA_LINE_LEN])
               malloc((size_t)max_lines * WGT_TEXTAREA_LINE_LEN);
    d->n_lines_max = max_lines;
    d->n_lines     = 1;
    d->cur_row     = 0;
    d->cur_col     = 0;
    d->scroll_top  = 0;

    /* Initialise first line */
    if (d->lines) {
        d->lines[0][0] = '\0';
    }
}

void textarea_set_text(widget_t *w, const char *text)
{
    wdata_textarea_t *d = &w->data.textarea;
    if (!d->lines) return;

    d->n_lines = 0;
    d->cur_row = 0;
    d->cur_col = 0;
    d->scroll_top = 0;

    int col = 0;
    d->n_lines = 1;
    d->lines[0][0] = '\0';

    for (int i = 0; text[i] && d->n_lines <= d->n_lines_max; i++) {
        if (text[i] == '\n') {
            d->lines[d->n_lines - 1][col] = '\0';
            if (d->n_lines < d->n_lines_max) {
                d->n_lines++;
                d->lines[d->n_lines - 1][0] = '\0';
            }
            col = 0;
        } else if (col < WGT_TEXTAREA_LINE_LEN - 1) {
            d->lines[d->n_lines - 1][col++] = text[i];
            d->lines[d->n_lines - 1][col] = '\0';
        }
    }
    w->dirty = 1;
}

void textarea_get_text(widget_t *w, char *buf, int max)
{
    wdata_textarea_t *d = &w->data.textarea;
    int pos = 0;
    for (int r = 0; r < d->n_lines && pos < max - 1; r++) {
        char *line = d->lines[r];
        for (int c = 0; line[c] && pos < max - 2; c++)
            buf[pos++] = line[c];
        if (r < d->n_lines - 1 && pos < max - 1)
            buf[pos++] = '\n';
    }
    buf[pos] = '\0';
}
