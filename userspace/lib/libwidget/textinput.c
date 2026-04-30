/*
 * AetherOS libwidget — WIDGET_TEXTINPUT (Phase 5.3)
 * File: userspace/lib/libwidget/textinput.c
 *
 * Single-line text field with:
 *   - Caret blink (30-tick period via WEV_TICK)
 *   - Left/Right arrows, Home/End cursor movement
 *   - Backspace (delete before caret) and Delete (delete after caret)
 *   - Ctrl+A: select all (moves caret to end, for paste readiness)
 *   - Ctrl+C: write buffer to clipboard
 *   - Ctrl+V: paste clipboard content at caret
 *   - Enter: call on_submit; change: call on_change
 */

#include <widget.h>
#include <gfx.h>
#include <sys.h>
#include <string.h>

#define INPUT_PAD_X  4
#define INPUT_PAD_Y  3
#define C_INPUT_BG   C_WIN_BG
#define C_INPUT_BDR  C_SEP
#define C_INPUT_BDF  C_ACCENT   /* focused border */
#define C_INPUT_CUR  C_ACCENT

/* Convert a keycode+modifiers to an ASCII character, or 0 if not printable */
static char keycode_to_char(keycode_t kc, unsigned int mods)
{
    int shift = (mods & MOD_SHIFT) || (mods & MOD_CAPS);

    /* Letters */
    if (kc >= KEY_A && kc <= KEY_Z) {
        char base = 'a' + (kc - KEY_A);
        return shift ? (char)(base - 32) : base;
    }
    /* Digits — PT-PT shifted row: =!"#$%&/() */
    static const char digits_normal[] = "0123456789";
    static const char digits_shift[]  = "=!\"#$%&/()";
    if (kc >= KEY_0 && kc <= KEY_9) {
        int idx = kc - KEY_0;
        return shift ? digits_shift[idx] : digits_normal[idx];
    }
    /* Space */
    if (kc == KEY_SPACE) return ' ';

    /* Punctuation — PT-PT layout */
    if (!shift) {
        switch (kc) {
        case KEY_GRAVE:      return '\\';   /* PT: \ */
        case KEY_MINUS:      return '\'';   /* PT: ' */
        case KEY_EQUALS:     return '\xab'; /* PT: « */
        case KEY_LBRACKET:   return '+';    /* PT: + */
        case KEY_RBRACKET:   return 0;      /* PT: dead-´ */
        case KEY_BACKSLASH:  return '~';    /* PT: dead-~ (base char) */
        case KEY_SEMICOLON:  return '\xe7'; /* PT: ç */
        case KEY_APOSTROPHE: return '\xba'; /* PT: º */
        case KEY_COMMA:      return ',';
        case KEY_DOT:        return '.';
        case KEY_SLASH:      return '-';    /* PT: - */
        default: break;
        }
    } else {
        switch (kc) {
        case KEY_GRAVE:      return '|';    /* PT: | */
        case KEY_MINUS:      return '?';    /* PT: ? */
        case KEY_EQUALS:     return '\xbb'; /* PT: » */
        case KEY_LBRACKET:   return '*';    /* PT: * */
        case KEY_RBRACKET:   return '`';    /* PT: dead-` (base char) */
        case KEY_BACKSLASH:  return '^';    /* PT: dead-^ (base char) */
        case KEY_SEMICOLON:  return '\xc7'; /* PT: Ç */
        case KEY_APOSTROPHE: return '\xaa'; /* PT: ª */
        case KEY_COMMA:      return ';';    /* PT: ; */
        case KEY_DOT:        return ':';    /* PT: : */
        case KEY_SLASH:      return '_';    /* PT: _ */
        default: break;
        }
    }
    return 0;
}

static void textinput_draw(widget_t *w, int ax, int ay)
{
    wdata_textinput_t *d = &w->data.textinput;
    int focused = (w->state == WS_FOCUSED || w->state == WS_PRESSED);

    /* Background + border */
    gfx_fill(ax, ay, w->bounds.w, w->bounds.h, C_INPUT_BG);
    gfx_rect(ax, ay, w->bounds.w, w->bounds.h,
             focused ? C_INPUT_BDF : C_INPUT_BDR);

    int tx   = ax + INPUT_PAD_X;
    int ty   = ay + INPUT_PAD_Y;
    int visible_cols = (w->bounds.w - 2 * INPUT_PAD_X) / WGT_FONT_W;

    /* Compute scroll offset so cursor stays visible */
    int scroll = 0;
    if (d->cursor >= visible_cols)
        scroll = d->cursor - visible_cols + 1;

    /* Draw text */
    for (int i = 0; i < visible_cols && (scroll + i) <= d->len; i++) {
        int idx = scroll + i;
        char ch = (idx < d->len) ? d->buf[idx] : ' ';
        gfx_char(tx + i * WGT_FONT_W, ty, ch, C_TEXT, C_INPUT_BG);
    }

    /* Caret */
    if (focused && d->blink_on) {
        int caret_col = d->cursor - scroll;
        if (caret_col >= 0 && caret_col < visible_cols) {
            gfx_fill(tx + caret_col * WGT_FONT_W, ty,
                     2, WGT_FONT_H, C_INPUT_CUR);
        }
    }
}

static int textinput_event(widget_t *w, const widget_event_t *ev)
{
    wdata_textinput_t *d = &w->data.textinput;

    if (ev->type == WEV_FOCUS_IN || ev->type == WEV_FOCUS_OUT) {
        d->blink_on = 1;
        d->blink_tick = ev->tick;
        w->dirty = 1;
        return 0;
    }

    if (ev->type == WEV_TICK) {
        /* Toggle caret blink only when focused */
        if (w->state == WS_FOCUSED) {
            int new_blink = (int)((ev->tick / 30) & 1);
            if (new_blink != d->blink_on) {
                d->blink_on = new_blink;
                w->dirty = 1;
            }
        }
        return 0;
    }

    if (ev->type != WEV_KEY_DOWN) return 0;

    keycode_t kc = ev->keycode;
    unsigned int mods = ev->modifiers;

    /* Ctrl combos */
    if (mods & MOD_CTRL) {
        if (kc == KEY_A) {
            /* Select all: move cursor to end */
            d->cursor = d->len;
            w->dirty = 1;
            return 1;
        }
        if (kc == KEY_C) {
            sys_clipboard_write(d->buf, (long)d->len);
            return 1;
        }
        if (kc == KEY_V) {
            char tmp[WGT_TEXTINPUT_MAX];
            long n = sys_clipboard_read(tmp, (long)sizeof(tmp));
            if (n > 0) {
                /* Insert clipboard text at cursor */
                int room = WGT_TEXTINPUT_MAX - 1 - d->len;
                if (n > room) n = room;
                /* Shift right */
                for (int i = d->len; i >= d->cursor; i--)
                    d->buf[i + (int)n] = d->buf[i];
                for (int i = 0; i < (int)n; i++)
                    d->buf[d->cursor + i] = tmp[i];
                d->len += (int)n;
                d->cursor += (int)n;
                w->dirty = 1;
                if (d->on_change) d->on_change(w);
            }
            return 1;
        }
        return 0;
    }

    /* Navigation */
    if (kc == KEY_LEFT  && d->cursor > 0)  { d->cursor--; w->dirty = 1; return 1; }
    if (kc == KEY_RIGHT && d->cursor < d->len) { d->cursor++; w->dirty = 1; return 1; }
    if (kc == KEY_HOME) { d->cursor = 0;      w->dirty = 1; return 1; }
    if (kc == KEY_END)  { d->cursor = d->len; w->dirty = 1; return 1; }

    /* Backspace */
    if (kc == KEY_BACKSPACE && d->cursor > 0) {
        for (int i = d->cursor - 1; i < d->len - 1; i++)
            d->buf[i] = d->buf[i + 1];
        d->len--;
        d->cursor--;
        d->buf[d->len] = '\0';
        w->dirty = 1;
        if (d->on_change) d->on_change(w);
        return 1;
    }

    /* Delete */
    if (kc == KEY_DELETE && d->cursor < d->len) {
        for (int i = d->cursor; i < d->len - 1; i++)
            d->buf[i] = d->buf[i + 1];
        d->len--;
        d->buf[d->len] = '\0';
        w->dirty = 1;
        if (d->on_change) d->on_change(w);
        return 1;
    }

    /* Enter */
    if (kc == KEY_ENTER) {
        if (d->on_submit) d->on_submit(w);
        return 1;
    }

    /* Escape */
    if (kc == KEY_ESC) return 0; /* let parent handle */

    /* Printable character insertion */
    char ch = keycode_to_char(kc, mods);
    if (ch && d->len < WGT_TEXTINPUT_MAX - 1) {
        for (int i = d->len; i > d->cursor; i--)
            d->buf[i] = d->buf[i - 1];
        d->buf[d->cursor] = ch;
        d->len++;
        d->cursor++;
        d->buf[d->len] = '\0';
        w->dirty = 1;
        if (d->on_change) d->on_change(w);
        return 1;
    }

    return 0;
}

void widget_init_textinput(widget_t *w, int x, int y, int width, int height,
                           void (*on_change)(widget_t *w),
                           void (*on_submit)(widget_t *w))
{
    widget_init(w, WIDGET_TEXTINPUT, x, y, width, height);
    w->draw_fn   = textinput_draw;
    w->event_fn  = textinput_event;
    w->focusable = 1;

    wdata_textinput_t *d = &w->data.textinput;
    memset(d->buf, 0, sizeof(d->buf));
    d->len       = 0;
    d->cursor    = 0;
    d->blink_on  = 1;
    d->blink_tick = 0;
    d->on_change = on_change;
    d->on_submit = on_submit;
}

const char *textinput_get_text(widget_t *w)
{
    return w->data.textinput.buf;
}

void textinput_set_text(widget_t *w, const char *text)
{
    wdata_textinput_t *d = &w->data.textinput;
    int i = 0;
    while (text && text[i] && i < WGT_TEXTINPUT_MAX - 1) {
        d->buf[i] = text[i];
        i++;
    }
    d->buf[i] = '\0';
    d->len    = i;
    d->cursor = i;
    w->dirty  = 1;
}

void textinput_clear(widget_t *w)
{
    wdata_textinput_t *d = &w->data.textinput;
    d->buf[0] = '\0';
    d->len    = 0;
    d->cursor = 0;
    w->dirty  = 1;
}
