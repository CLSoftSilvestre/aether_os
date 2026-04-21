/*
 * AetherOS — AetherTerm
 * File: userspace/apps/aether_term/main.c
 *
 * Phase 4.4: Separate terminal process spawned by init.
 * Draws a Lumina-themed terminal window, runs the aesh shell inside.
 * Assumes init already called sys_fb_claim() and drew the desktop chrome.
 *
 * Shell commands: help, echo, ls, cat, mem, time, clear, uname, pid,
 *                 spawn, files, view, exit
 */

#include <gfx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys.h>
#include <input.h>

/* ── Layout constants (must match init) ─────────────────────────────────── */

#define TOPBAR_H   36
#define ACCENT_H    2
#define BOTBAR_Y  744

#define FONT_W  8
#define FONT_H  8

#define TERM_COLS  80
#define TERM_ROWS  70
#define WIN_W     (TERM_COLS * FONT_W + 16)
#define TITLE_H    28
#define WIN_H     (TITLE_H + 8 + TERM_ROWS * FONT_H + 8)
#define WIN_X      8
#define WIN_Y     (TOPBAR_H + ACCENT_H + \
                   (BOTBAR_Y - TOPBAR_H - ACCENT_H - WIN_H) / 2)

#define TERM_X    (WIN_X + 8)
#define TERM_Y    (WIN_Y + TITLE_H + 8)
#define TERM_W    (TERM_COLS * FONT_W)
#define TERM_H    (TERM_ROWS * FONT_H)

/* ── Terminal emulator state ─────────────────────────────────────────────── */

static char t_buf[TERM_ROWS][TERM_COLS];
static int  t_col;
static int  t_row;

static void term_clear_row(int row)
{
    for (int c = 0; c < TERM_COLS; c++) t_buf[row][c] = ' ';
}

static void term_init(void)
{
    for (int r = 0; r < TERM_ROWS; r++) term_clear_row(r);
    t_col = 0;
    t_row = 0;
}

static void term_render_row(int row)
{
    for (int c = 0; c < TERM_COLS; c++) {
        gfx_char(TERM_X + c * FONT_W,
                 TERM_Y + row * FONT_H,
                 t_buf[row][c], C_TEXT, C_TERM_BG);
    }
}

static void term_redraw_all(void)
{
    for (int r = 0; r < TERM_ROWS; r++) term_render_row(r);
}

static void term_scroll(void)
{
    for (int r = 0; r < TERM_ROWS - 1; r++) {
        for (int c = 0; c < TERM_COLS; c++)
            t_buf[r][c] = t_buf[r + 1][c];
        term_render_row(r);
    }
    term_clear_row(TERM_ROWS - 1);
    term_render_row(TERM_ROWS - 1);
    t_row = TERM_ROWS - 1;
}

static void term_erase_cursor(void)
{
    gfx_char(TERM_X + t_col * FONT_W,
             TERM_Y + t_row * FONT_H,
             t_buf[t_row][t_col], C_TEXT, C_TERM_BG);
}

static void term_draw_cursor(void)
{
    gfx_fill(TERM_X + t_col * FONT_W,
             TERM_Y + t_row * FONT_H,
             FONT_W, FONT_H, C_ACCENT);
    if (t_buf[t_row][t_col] != ' ')
        gfx_char(TERM_X + t_col * FONT_W,
                 TERM_Y + t_row * FONT_H,
                 t_buf[t_row][t_col], C_TERM_BG, C_ACCENT);
}

static void term_putc(char c)
{
    if (c == '\r') { t_col = 0; return; }
    if (c == '\n') {
        t_col = 0;
        t_row++;
        if (t_row >= TERM_ROWS) term_scroll();
        return;
    }
    if ((c == '\b' || c == 127) && t_col > 0) {
        t_col--;
        t_buf[t_row][t_col] = ' ';
        gfx_char(TERM_X + t_col * FONT_W,
                 TERM_Y + t_row * FONT_H,
                 ' ', C_TEXT, C_TERM_BG);
        return;
    }
    if ((unsigned char)c < 32) return;

    t_buf[t_row][t_col] = c;
    gfx_char(TERM_X + t_col * FONT_W,
             TERM_Y + t_row * FONT_H,
             c, C_TEXT, C_TERM_BG);
    t_col++;
    if (t_col >= TERM_COLS) {
        t_col = 0;
        t_row++;
        if (t_row >= TERM_ROWS) term_scroll();
    }
}

static void term_puts(const char *s) { while (*s) term_putc(*s++); }

static void term_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(buf);
}

/* keycode → ASCII (unshifted / shifted) — local copy for terminal use */
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

static char term_key_to_char(key_event_t ev)
{
    int shifted = (ev.modifiers & MOD_SHIFT) ||
                  ((ev.modifiers & MOD_CAPS) &&
                   ev.keycode >= KEY_A && ev.keycode <= KEY_Z);
    const char *tbl = shifted ? kc_ascii_shift : kc_ascii;
    if ((unsigned int)ev.keycode >= (unsigned int)KEY_MAX) return 0;
    return tbl[ev.keycode];
}

/*
 * term_readline — graphical line editor using PS/2 key events.
 *
 * Supports: character echo + cursor, Backspace, Left/Right cursor
 * movement within the line, Ctrl+C (exit 130).
 */
static int term_readline(char *buf, int max)
{
    int n   = 0;   /* total chars in buffer */
    int pos = 0;   /* cursor position within buffer (0..n) */

    term_draw_cursor();

    while (n < max - 1) {
        unsigned long long raw = sys_key_read();
        key_event_t ev = key_event_unpack(raw);
        if (!ev.is_press) continue;

        /* Ctrl+C */
        if ((ev.modifiers & MOD_CTRL) && ev.keycode == KEY_C) {
            term_erase_cursor();
            term_puts("^C\n");
            exit(130);
        }

        /* Enter */
        if (ev.keycode == KEY_ENTER) {
            term_erase_cursor();
            term_putc('\n');
            break;
        }

        /* Backspace — delete char before cursor */
        if (ev.keycode == KEY_BACKSPACE && pos > 0) {
            term_erase_cursor();
            /* Shift chars left */
            for (int i = pos - 1; i < n - 1; i++) buf[i] = buf[i + 1];
            n--; pos--;
            buf[n] = '\0';
            /* Redraw from pos to end + blank the last char */
            for (int i = pos; i < n; i++) {
                t_buf[t_row][t_col] = buf[i];
                gfx_char(TERM_X + t_col * FONT_W, TERM_Y + t_row * FONT_H,
                         buf[i], C_TEXT, C_TERM_BG);
                t_col++;
            }
            /* Blank the vacated position */
            gfx_char(TERM_X + t_col * FONT_W, TERM_Y + t_row * FONT_H,
                     ' ', C_TEXT, C_TERM_BG);
            t_buf[t_row][t_col] = ' ';
            /* Move visual cursor back to pos */
            t_col = t_col - (n - pos);
            term_draw_cursor();
            continue;
        }

        /* Arrow LEFT */
        if (ev.keycode == KEY_LEFT && pos > 0) {
            term_erase_cursor();
            pos--;
            t_col--;
            term_draw_cursor();
            continue;
        }

        /* Arrow RIGHT */
        if (ev.keycode == KEY_RIGHT && pos < n) {
            term_erase_cursor();
            pos++;
            t_col++;
            term_draw_cursor();
            continue;
        }

        /* Printable character — insert at cursor position */
        char c = term_key_to_char(ev);
        if (!c) continue;

        term_erase_cursor();
        /* Shift chars right to make room */
        for (int i = n; i > pos; i--) buf[i] = buf[i - 1];
        buf[pos] = c;
        n++; pos++;
        buf[n] = '\0';
        /* Redraw from insertion point to end */
        int save_col = t_col;
        for (int i = pos - 1; i < n; i++) {
            t_buf[t_row][t_col] = buf[i];
            gfx_char(TERM_X + t_col * FONT_W, TERM_Y + t_row * FONT_H,
                     buf[i], C_TEXT, C_TERM_BG);
            t_col++;
        }
        t_col = save_col + 1;   /* cursor advances one past insertion */
        term_draw_cursor();
    }
    buf[n] = '\0';
    return n;
}

/* ── Window drawing ──────────────────────────────────────────────────────── */

static void draw_window(void)
{
    gfx_fill(WIN_X + 4, WIN_Y + 4, WIN_W, WIN_H, GFX_RGB(8, 8, 14));
    gfx_fill(WIN_X, WIN_Y, WIN_W, WIN_H, C_WIN_BG);
    gfx_fill(WIN_X, WIN_Y, WIN_W, TITLE_H, C_TITLEBAR);

    gfx_fill(WIN_X + 10, WIN_Y + 8, 12, 12, C_RED);
    gfx_fill(WIN_X + 26, WIN_Y + 8, 12, 12, C_YELLOW);
    gfx_fill(WIN_X + 42, WIN_Y + 8, 12, 12, C_GREEN);

    gfx_text_center(WIN_X, WIN_W, WIN_Y + 8,
                    "AetherTerm  --  aesh", C_TEXT, C_TITLEBAR);

    gfx_hline(WIN_X, WIN_Y + TITLE_H, WIN_W, C_ACCENT);
    gfx_fill(WIN_X, WIN_Y + TITLE_H + 1,
             WIN_W, WIN_H - TITLE_H - 1, C_TERM_BG);
    gfx_rect(WIN_X, WIN_Y, WIN_W, WIN_H, C_SEP);
}

static void redraw_after_child(void)
{
    draw_window();
    term_redraw_all();
}

/* ── Time formatting ─────────────────────────────────────────────────────── */

static void fmt_uptime(char *buf, long ticks)
{
    long s = ticks / 100, m = s / 60; s %= 60;
    long h = m / 60;                  m %= 60;
    snprintf(buf, 16, "%02ld:%02ld:%02ld", h, m, s);
}

/* ── Shell built-ins ─────────────────────────────────────────────────────── */

#define PROMPT    "aesh> "
#define ARGV_MAX   16
#define LINE_MAX  256

static int parse_args(char *line, char **argv, int maxargs)
{
    int argc = 0;
    char *tok = strtok(line, " \t");
    while (tok && argc < maxargs - 1) { argv[argc++] = tok; tok = strtok(NULL, " \t"); }
    argv[argc] = NULL;
    return argc;
}

static void cmd_help(void)
{
    term_puts("Built-in commands:\n");
    term_puts("  help              show this message\n");
    term_puts("  echo [args]       print arguments\n");
    term_puts("  ls                list files in initrd\n");
    term_puts("  cat <file>        print a file from initrd\n");
    term_puts("  mem               show memory statistics\n");
    term_puts("  time              show formatted uptime\n");
    term_puts("  clear             clear terminal\n");
    term_puts("  uname             print OS information\n");
    term_puts("  pid               print current process ID\n");
    term_puts("  files             launch graphical file browser\n");
    term_puts("  view              launch text viewer\n");
    term_puts("  spawn <path>      launch an ELF from initrd (wait)\n");
    term_puts("  spawn <path> &    launch in background (no wait)\n");
    term_puts("  exit [code]       exit the terminal\n");
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) term_putc(' ');
        term_puts(argv[i]);
    }
    term_putc('\n');
}

static void cmd_ls(void)
{
    char buf[2048];
    long n = sys_initrd_ls(buf, sizeof(buf));
    if (n < 0) { term_puts("ls: failed\n"); return; }
    term_puts(buf);
    if (n > 0 && buf[n-1] != '\n') term_putc('\n');
}

static void cmd_cat(const char *name)
{
    if (!name || name[0] == '\0') { term_puts("usage: cat <filename>\n"); return; }
    char buf[4096];
    long n = sys_initrd_read(name, buf, (long)sizeof(buf) - 1);
    if (n < 0) { term_printf("cat: %s: not found\n", name); return; }
    buf[n] = '\0';
    term_puts(buf);
    if (n > 0 && buf[n-1] != '\n') term_putc('\n');
}

static void cmd_mem(void)
{
    long v = sys_pmm_stats();
    unsigned long free_p  = (unsigned long)((unsigned long long)v >> 32);
    unsigned long total_p = (unsigned long)((unsigned long long)v & 0xFFFFFFFFUL);
    unsigned long free_mb  = free_p  * 4 / 1024;
    unsigned long total_mb = total_p * 4 / 1024;
    term_printf("Memory: %lu MB used / %lu MB free / %lu MB total\n",
                total_mb - free_mb, free_mb, total_mb);
}

static void cmd_time(long ticks)
{
    char tbuf[16];
    fmt_uptime(tbuf, ticks);
    term_printf("Uptime: %s  (%ld ticks at 100 Hz)\n", tbuf, ticks);
}

static void cmd_clear(void)
{
    gfx_fill(WIN_X + 1, WIN_Y + TITLE_H + 1,
             WIN_W - 2, WIN_H - TITLE_H - 2, C_TERM_BG);
    for (int r = 0; r < TERM_ROWS; r++) term_clear_row(r);
    t_col = 0;
    t_row = 0;
}

static void cmd_uname(void)
{
    term_puts("AetherOS  aarch64  Phase 4.5  QEMU virt (Cortex-A76)\n");
}

static void cmd_pid(void)
{
    term_printf("PID: %ld\n", sys_getpid());
}

/*
 * wait_foreground — poll for child exit while checking keyboard for Ctrl+C.
 * Returns the child's exit status, or -1 if killed via Ctrl+C.
 */
static int wait_foreground(long child)
{
    int status = 0;
    for (;;) {
        long r = sys_waitpid_nb(child, &status);
        if (r != 0) break;   /* child exited (r == child PID) or not found */

        unsigned long long ke = sys_key_poll();
        if (ke) {
            key_event_t ev = key_event_unpack(ke);
            if (ev.is_press && (ev.modifiers & MOD_CTRL) && ev.keycode == KEY_C) {
                sys_kill(child);
                /* drain remaining key events */
                while (sys_key_poll()) {}
                term_puts("^C\n");
                /* collect zombie so the task slot is freed */
                sys_waitpid(child, &status);
                return -1;
            }
        }
        sys_sleep(1);
    }
    return status;
}

static void cmd_spawn(const char *path, int background)
{
    if (!path || path[0] == '\0') { term_puts("usage: spawn <initrd-path>\n"); return; }
    term_printf("Spawning '%s'%s...\n", path, background ? " [bg]" : "");
    long child = sys_spawn(path);
    if (child < 0) { term_printf("spawn: failed to launch '%s'\n", path); return; }
    term_printf("Child PID %ld started\n", child);
    if (!background) {
        int status = wait_foreground(child);
        if (status >= 0)
            term_printf("Child PID %ld exited (status %d)\n", child, status);
        redraw_after_child();
    }
}

static void cmd_files(void)
{
    long child = sys_spawn("/files");
    if (child < 0) { term_puts("files: not found in initrd\n"); return; }
    wait_foreground(child);
    redraw_after_child();
}

static void cmd_view(void)
{
    long child = sys_spawn("/textviewer");
    if (child < 0) { term_puts("textviewer: not found in initrd\n"); return; }
    wait_foreground(child);
    redraw_after_child();
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    draw_window();
    term_init();

    {
        char motd[2048];
        long n = sys_initrd_read("motd.txt", motd, (long)sizeof(motd) - 1);
        if (n > 0) {
            motd[n] = '\0';
            term_puts(motd);
        } else {
            term_puts("\n  Welcome to AetherOS Phase 4.4\n");
            term_puts("  AetherTerm  --  type 'help' for commands\n\n");
        }
    }

    char line[LINE_MAX];
    char *argv[ARGV_MAX];

    for (;;) {
        term_puts(PROMPT);
        int n = term_readline(line, LINE_MAX);
        if (n == 0) continue;

        while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t')) line[--n] = '\0';
        if (n == 0) continue;

        int background = 0;
        if (n > 0 && line[n-1] == '&') {
            background = 1;
            line[--n] = '\0';
            while (n > 0 && line[n-1] == ' ') line[--n] = '\0';
        }

        int argc = parse_args(line, argv, ARGV_MAX);
        if (argc == 0) continue;
        const char *cmd = argv[0];

        if      (strcmp(cmd, "help")  == 0) cmd_help();
        else if (strcmp(cmd, "echo")  == 0) cmd_echo(argc, argv);
        else if (strcmp(cmd, "ls")    == 0) cmd_ls();
        else if (strcmp(cmd, "cat")   == 0) cmd_cat(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "mem")   == 0) cmd_mem();
        else if (strcmp(cmd, "time")  == 0) cmd_time(sys_get_ticks());
        else if (strcmp(cmd, "clear") == 0) cmd_clear();
        else if (strcmp(cmd, "uname") == 0) cmd_uname();
        else if (strcmp(cmd, "pid")   == 0) cmd_pid();
        else if (strcmp(cmd, "files") == 0) cmd_files();
        else if (strcmp(cmd, "view")  == 0) cmd_view();
        else if (strcmp(cmd, "spawn") == 0) cmd_spawn(argc > 1 ? argv[1] : NULL, background);
        else if (strcmp(cmd, "exit")  == 0) {
            int code = (argc > 1) ? atoi(argv[1]) : 0;
            term_puts("Goodbye!\n");
            exit(code);
        } else {
            /* Try to spawn as an initrd path */
            char path[64];
            snprintf(path, sizeof(path), "/%s", cmd);
            long child = sys_spawn(path);
            if (child < 0) {
                term_printf("aesh: %s: command not found\n", cmd);
            } else {
                if (!background) {
                    wait_foreground(child);
                    redraw_after_child();
                }
            }
        }
    }

    return 0;
}
