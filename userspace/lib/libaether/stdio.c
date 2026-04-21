#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys.h>
#include <input.h>

/* ── vsnprintf ───────────────────────────────────────────────────────── */

static void _emit(char *buf, size_t size, size_t *pos, char c)
{
    if (*pos + 1 < size) buf[(*pos)++] = c;
}

static void _emit_str(char *buf, size_t size, size_t *pos, const char *s)
{
    if (!s) s = "(null)";
    while (*s) _emit(buf, size, pos, *s++);
}

static void _emit_u64(char *buf, size_t size, size_t *pos, unsigned long long v,
                      int base, int upper, int min_width, char pad)
{
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digits = upper ? hi : lo;
    char tmp[24];
    int  n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v) { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; }
    }
    while (n < min_width) tmp[n++] = pad;
    while (n--) _emit(buf, size, pos, tmp[n]);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { _emit(buf, size, &pos, *p); continue; }
        p++;
        if (!*p) break;

        /* Flags: zero-pad */
        char pad = ' ';
        if (*p == '0') { pad = '0'; p++; }

        /* Width */
        int width = 0;
        while (*p >= '1' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        /* Long prefix */
        int is_long = 0;
        if (*p == 'l') { is_long = 1; p++; }
        if (*p == 'l') { p++; }   /* 'll' → treat as long long */

        switch (*p) {
        case 'd': case 'i': {
            long long v = is_long ? (long long)va_arg(ap, long)
                                  : (long long)va_arg(ap, int);
            if (v < 0) { _emit(buf, size, &pos, '-'); v = -v; }
            _emit_u64(buf, size, &pos, (unsigned long long)v, 10, 0, width, pad);
            break;
        }
        case 'u': {
            unsigned long long v = is_long ? (unsigned long long)va_arg(ap, unsigned long)
                                           : (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 10, 0, width, pad);
            break;
        }
        case 'x': {
            unsigned long long v = is_long ? (unsigned long long)va_arg(ap, unsigned long)
                                           : (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 16, 0, width, pad);
            break;
        }
        case 'X': {
            unsigned long long v = is_long ? (unsigned long long)va_arg(ap, unsigned long)
                                           : (unsigned long long)va_arg(ap, unsigned int);
            _emit_u64(buf, size, &pos, v, 16, 1, width, pad);
            break;
        }
        case 'p': {
            unsigned long long v = (unsigned long long)(uintptr_t)va_arg(ap, void *);
            _emit(buf, size, &pos, '0'); _emit(buf, size, &pos, 'x');
            _emit_u64(buf, size, &pos, v, 16, 0, 0, ' ');
            break;
        }
        case 's':
            _emit_str(buf, size, &pos, va_arg(ap, const char *));
            break;
        case 'c':
            _emit(buf, size, &pos, (char)va_arg(ap, int));
            break;
        case '%':
            _emit(buf, size, &pos, '%');
            break;
        default:
            _emit(buf, size, &pos, '%');
            _emit(buf, size, &pos, *p);
            break;
        }
    }
    if (size > 0) buf[pos] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

/* ── printf ──────────────────────────────────────────────────────────── */

int printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sys_write(STDOUT_FILENO, buf, n);
    return n;
}

/* ── putchar / puts ──────────────────────────────────────────────────── */

int putchar(int c)
{
    char ch = (char)c;
    sys_write(STDOUT_FILENO, &ch, 1);
    return c;
}

int puts(const char *s)
{
    sys_puts(s);
    sys_write(STDOUT_FILENO, "\n", 1);
    return 0;
}

/* ── keycode → ASCII conversion tables ──────────────────────────────── */

static const char kc_ascii[KEY_MAX] = {
    [KEY_A]='a',[KEY_B]='b',[KEY_C]='c',[KEY_D]='d',[KEY_E]='e',
    [KEY_F]='f',[KEY_G]='g',[KEY_H]='h',[KEY_I]='i',[KEY_J]='j',
    [KEY_K]='k',[KEY_L]='l',[KEY_M]='m',[KEY_N]='n',[KEY_O]='o',
    [KEY_P]='p',[KEY_Q]='q',[KEY_R]='r',[KEY_S]='s',[KEY_T]='t',
    [KEY_U]='u',[KEY_V]='v',[KEY_W]='w',[KEY_X]='x',[KEY_Y]='y',
    [KEY_Z]='z',
    [KEY_0]='0',[KEY_1]='1',[KEY_2]='2',[KEY_3]='3',[KEY_4]='4',
    [KEY_5]='5',[KEY_6]='6',[KEY_7]='7',[KEY_8]='8',[KEY_9]='9',
    [KEY_ENTER]='\n',[KEY_BACKSPACE]='\b',[KEY_TAB]='\t',[KEY_SPACE]=' ',
    [KEY_MINUS]='-',[KEY_EQUALS]='=',[KEY_LBRACKET]='[',[KEY_RBRACKET]=']',
    [KEY_BACKSLASH]='\\',[KEY_SEMICOLON]=';',[KEY_APOSTROPHE]='\'',
    [KEY_COMMA]=',',[KEY_DOT]='.',[KEY_SLASH]='/',[KEY_GRAVE]='`',
};

static const char kc_ascii_shift[KEY_MAX] = {
    [KEY_A]='A',[KEY_B]='B',[KEY_C]='C',[KEY_D]='D',[KEY_E]='E',
    [KEY_F]='F',[KEY_G]='G',[KEY_H]='H',[KEY_I]='I',[KEY_J]='J',
    [KEY_K]='K',[KEY_L]='L',[KEY_M]='M',[KEY_N]='N',[KEY_O]='O',
    [KEY_P]='P',[KEY_Q]='Q',[KEY_R]='R',[KEY_S]='S',[KEY_T]='T',
    [KEY_U]='U',[KEY_V]='V',[KEY_W]='W',[KEY_X]='X',[KEY_Y]='Y',
    [KEY_Z]='Z',
    [KEY_0]=')',[KEY_1]='!',[KEY_2]='@',[KEY_3]='#',[KEY_4]='$',
    [KEY_5]='%',[KEY_6]='^',[KEY_7]='&',[KEY_8]='*',[KEY_9]='(',
    [KEY_ENTER]='\n',[KEY_BACKSPACE]='\b',[KEY_TAB]='\t',[KEY_SPACE]=' ',
    [KEY_MINUS]='_',[KEY_EQUALS]='+',[KEY_LBRACKET]='{',[KEY_RBRACKET]='}',
    [KEY_BACKSLASH]='|',[KEY_SEMICOLON]=':',[KEY_APOSTROPHE]='"',
    [KEY_COMMA]='<',[KEY_DOT]='>',[KEY_SLASH]='?',[KEY_GRAVE]='~',
};

static char keycode_to_char(key_event_t ev)
{
    int shifted = (ev.modifiers & MOD_SHIFT) ||
                  ((ev.modifiers & MOD_CAPS) &&
                   ev.keycode >= KEY_A && ev.keycode <= KEY_Z);
    const char *table = shifted ? kc_ascii_shift : kc_ascii;
    if ((unsigned int)ev.keycode >= (unsigned int)KEY_MAX) return 0;
    return table[ev.keycode];
}

/* ── Command history ring buffer ─────────────────────────────────────── */

#define HIST_ENTRIES  16
#define HIST_LINE_MAX 256

static char  hist_buf[HIST_ENTRIES][HIST_LINE_MAX];
static int   hist_count;   /* total entries ever stored (wraps via % HIST_ENTRIES) */

static void hist_push(const char *line)
{
    if (!line[0]) return;
    int slot = hist_count % HIST_ENTRIES;
    int i = 0;
    while (i < HIST_LINE_MAX - 1 && line[i]) { hist_buf[slot][i] = line[i]; i++; }
    hist_buf[slot][i] = '\0';
    hist_count++;
}

/* ── readline ────────────────────────────────────────────────────────── */

/*
 * readline() — blocking line editor using PS/2 key events.
 *   - Echoes characters to stdout
 *   - Backspace erases
 *   - Arrow UP/DOWN cycles history
 *   - Ctrl+C calls sys_exit(130)
 *   Returns on Enter (KEY_ENTER).
 */
int readline(char *buf, int max)
{
    int n = 0;
    int hist_idx = hist_count;  /* points past the newest entry = current input */

    buf[0] = '\0';

    while (n < max - 1) {
        unsigned long long raw = sys_key_read();
        key_event_t ev = key_event_unpack(raw);

        /* Only process key-down events */
        if (!ev.is_press) continue;

        /* Ctrl+C */
        if ((ev.modifiers & MOD_CTRL) && ev.keycode == KEY_C) {
            sys_write(STDOUT_FILENO, "^C\n", 3);
            sys_exit(130);
        }

        /* Enter */
        if (ev.keycode == KEY_ENTER) {
            putchar('\n');
            break;
        }

        /* Backspace */
        if (ev.keycode == KEY_BACKSPACE) {
            if (n > 0) {
                sys_write(STDOUT_FILENO, "\b \b", 3);
                n--;
                buf[n] = '\0';
            }
            continue;
        }

        /* Arrow UP — go back in history */
        if (ev.keycode == KEY_UP) {
            int oldest = (hist_count >= HIST_ENTRIES)
                         ? hist_count - HIST_ENTRIES : 0;
            if (hist_idx > oldest) {
                hist_idx--;
                /* Erase current line */
                for (int i = 0; i < n; i++)
                    sys_write(STDOUT_FILENO, "\b \b", 3);
                const char *entry = hist_buf[hist_idx % HIST_ENTRIES];
                int len = 0;
                while (entry[len]) { buf[len] = entry[len]; len++; }
                buf[len] = '\0';
                n = len;
                sys_write(STDOUT_FILENO, buf, (long)n);
            }
            continue;
        }

        /* Arrow DOWN — go forward in history */
        if (ev.keycode == KEY_DOWN) {
            if (hist_idx < hist_count) {
                hist_idx++;
                for (int i = 0; i < n; i++)
                    sys_write(STDOUT_FILENO, "\b \b", 3);
                if (hist_idx == hist_count) {
                    /* Restored to blank current input */
                    n = 0;
                    buf[0] = '\0';
                } else {
                    const char *entry = hist_buf[hist_idx % HIST_ENTRIES];
                    int len = 0;
                    while (entry[len]) { buf[len] = entry[len]; len++; }
                    buf[len] = '\0';
                    n = len;
                    sys_write(STDOUT_FILENO, buf, (long)n);
                }
            }
            continue;
        }

        /* Skip other non-printable navigation keys */
        if (ev.keycode == KEY_LEFT  || ev.keycode == KEY_RIGHT ||
            ev.keycode == KEY_HOME  || ev.keycode == KEY_END   ||
            ev.keycode == KEY_PGUP  || ev.keycode == KEY_PGDN  ||
            ev.keycode == KEY_INSERT|| ev.keycode == KEY_DELETE ||
            ev.keycode == KEY_ESC)
            continue;

        /* Printable character */
        char c = keycode_to_char(ev);
        if (!c) continue;

        buf[n++] = c;
        putchar(c);
    }
    buf[n] = '\0';
    hist_push(buf);
    return n;
}
