/*
 * AetherOS — Graphical Desktop Shell
 * File: userspace/apps/init/main.c
 *
 * Phase 4.1: Draws a Lumina-themed desktop with a terminal window.
 * The aesh shell runs inside the terminal using graphical text rendering.
 *
 * Desktop layout (1024×768):
 *   [0]    Top bar   (1024×36) — branding + uptime
 *   [36]   Accent    (1024×2)  — purple separator
 *   [38]   Main area (1024×706) — window centered here
 *   [744]  Bot bar   (1024×24) — status info
 *
 * Terminal window: 816×608 centered in main area
 *   Title bar: 816×28
 *   Client:    100 cols × 72 rows at 8×8 (800×576)
 */

#include <gfx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys.h>

/* ── Layout constants ────────────────────────────────────────────────── */

#define TOPBAR_Y    0
#define TOPBAR_H   36
#define ACCENT_Y   36
#define ACCENT_H    2
#define BOTBAR_Y  744
#define BOTBAR_H   24

/* Terminal window */
#define WIN_W     816
#define WIN_H     608
#define WIN_X     ((1024 - WIN_W) / 2)    /* 104 */
#define WIN_Y     (ACCENT_Y + ACCENT_H + (BOTBAR_Y - ACCENT_Y - ACCENT_H - WIN_H) / 2)  /* ~88 */
#define TITLE_H    28

/* Terminal text area inside window */
#define TERM_X    (WIN_X + 8)
#define TERM_Y    (WIN_Y + TITLE_H + 8)
#define TERM_COLS  100
#define TERM_ROWS   72
#define TERM_W    (TERM_COLS * 8)   /* 800 */
#define TERM_H    (TERM_ROWS * 8)   /* 576 */

#define FONT_W  8
#define FONT_H  8

/* ── Terminal emulator state ─────────────────────────────────────────── */

static char  t_buf[TERM_ROWS][TERM_COLS];
static int   t_col;
static int   t_row;

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

/* Erase the cursor block at current position */
static void term_erase_cursor(void)
{
    gfx_char(TERM_X + t_col * FONT_W,
             TERM_Y + t_row * FONT_H,
             t_buf[t_row][t_col], C_TEXT, C_TERM_BG);
}

/* Draw cursor block */
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
    if (c == '\r') {
        t_col = 0;
        return;
    }
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

static void term_puts(const char *s)
{
    while (*s) term_putc(*s++);
}

static void term_printf(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(buf);
}

/* Blocking readline with echo and backspace, draws cursor while waiting */
static int term_readline(char *buf, int max)
{
    int n = 0;
    term_draw_cursor();

    while (n < max - 1) {
        char c;
        sys_read(STDIN_FILENO, &c, 1);

        term_erase_cursor();

        if (c == '\r' || c == '\n') {
            term_putc('\n');
            break;
        }
        if ((c == '\b' || c == 127) && n > 0) {
            n--;
            term_putc('\b');
        } else if ((unsigned char)c >= 32) {
            buf[n++] = c;
            term_putc(c);
        }

        term_draw_cursor();
    }
    buf[n] = '\0';
    return n;
}

/* ── Desktop drawing ─────────────────────────────────────────────────── */

static void draw_desktop(void)
{
    /* Full background */
    gfx_fill(0, 0, 1024, 768, C_DESKTOP);
}

static void draw_top_bar(long ticks)
{
    /* Bar background */
    gfx_fill(0, TOPBAR_Y, 1024, TOPBAR_H, C_PANEL);

    /* Left: OS name */
    gfx_text(14, TOPBAR_Y + 10, "AetherOS", C_TEXT, C_PANEL);
    gfx_text(14 + 8 * 8 + 8, TOPBAR_Y + 10, "v0.0.5", C_TEXT_DIM, C_PANEL);

    /* Center: phase */
    gfx_text_center(0, 1024, TOPBAR_Y + 10,
                    "Phase 4.1 -- Graphical Desktop", C_TEXT_DIM, C_PANEL);

    /* Right: uptime */
    long secs = ticks / 100;
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "up %lds", (long)secs);
    int len = (int)strlen(tbuf);
    gfx_text(1024 - len * 8 - 14, TOPBAR_Y + 10, tbuf, C_TEXT_DIM, C_PANEL);

    /* Accent line */
    gfx_fill(0, ACCENT_Y, 1024, ACCENT_H, C_ACCENT);
}

static void draw_bot_bar(void)
{
    gfx_fill(0, BOTBAR_Y, 1024, BOTBAR_H, C_PANEL);
    gfx_hline(0, BOTBAR_Y, 1024, C_SEP);
    gfx_text(14, BOTBAR_Y + 6,
             "AetherOS 0.0.5  |  QEMU virt  |  Cortex-A76  |  1024x768",
             C_TEXT_DIM, C_PANEL);
}

static void draw_window(void)
{
    /* Window shadow (offset 4px) */
    gfx_fill(WIN_X + 4, WIN_Y + 4, WIN_W, WIN_H,
             GFX_RGB(8, 8, 14));

    /* Window body */
    gfx_fill(WIN_X, WIN_Y, WIN_W, WIN_H, C_WIN_BG);

    /* Title bar */
    gfx_fill(WIN_X, WIN_Y, WIN_W, TITLE_H, C_TITLEBAR);

    /* Traffic light buttons */
    gfx_fill(WIN_X + 10, WIN_Y + 8,  12, 12, C_RED);
    gfx_fill(WIN_X + 26, WIN_Y + 8,  12, 12, C_YELLOW);
    gfx_fill(WIN_X + 42, WIN_Y + 8,  12, 12, C_GREEN);

    /* Title text — centered */
    gfx_text_center(WIN_X, WIN_W, WIN_Y + 8,
                    "aesh -- AetherOS Terminal", C_TEXT, C_TITLEBAR);

    /* Thin accent border along title bar bottom */
    gfx_hline(WIN_X, WIN_Y + TITLE_H, WIN_W, C_ACCENT);

    /* Terminal background */
    gfx_fill(WIN_X, WIN_Y + TITLE_H + 1, WIN_W, WIN_H - TITLE_H - 1, C_TERM_BG);

    /* Subtle inner border */
    gfx_rect(WIN_X, WIN_Y, WIN_W, WIN_H, C_SEP);
}

/* Redraw just the uptime in the top bar (called each prompt) */
static void refresh_topbar(long ticks)
{
    /* Erase right side of top bar */
    gfx_fill(700, TOPBAR_Y, 324, TOPBAR_H, C_PANEL);

    long secs = ticks / 100;
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "up %lds", (long)secs);
    int len = (int)strlen(tbuf);
    gfx_text(1024 - len * 8 - 14, TOPBAR_Y + 10, tbuf, C_TEXT_DIM, C_PANEL);
}

/* ── Shell built-ins ─────────────────────────────────────────────────── */

#define PROMPT    "aesh> "
#define ARGV_MAX   16
#define LINE_MAX   256

static int parse_args(char *line, char **argv, int maxargs)
{
    int argc = 0;
    char *tok = strtok(line, " \t");
    while (tok && argc < maxargs - 1) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \t");
    }
    argv[argc] = NULL;
    return argc;
}

static void cmd_help(void)
{
    term_puts("Built-in commands:\n");
    term_puts("  help           show this message\n");
    term_puts("  echo [args]    print arguments\n");
    term_puts("  ls             list files in initrd\n");
    term_puts("  clear          clear terminal\n");
    term_puts("  uname          print OS information\n");
    term_puts("  exit [code]    exit the shell\n");
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

static void cmd_clear(void)
{
    gfx_fill(WIN_X, WIN_Y + TITLE_H + 1, WIN_W, WIN_H - TITLE_H - 1, C_TERM_BG);
    for (int r = 0; r < TERM_ROWS; r++) term_clear_row(r);
    t_col = 0;
    t_row = 0;
}

static void cmd_uname(void)
{
    term_puts("AetherOS  aarch64  Phase 4.1  QEMU virt (Cortex-A76)\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    /* Claim the framebuffer — kernel fb_console goes silent */
    sys_fb_claim();

    gfx_init();

    /* Draw the full desktop */
    draw_desktop();
    draw_top_bar(sys_get_ticks());
    draw_bot_bar();
    draw_window();

    /* Init terminal emulator */
    term_init();

    /* Banner inside terminal */
    term_puts("\n");
    term_puts("  Welcome to AetherOS Phase 4.1\n");
    term_puts("  Lumina Graphical Desktop\n");
    term_puts("  Type 'help' for available commands.\n");
    term_puts("\n");

    /* REPL */
    char line[LINE_MAX];
    char *argv[ARGV_MAX];

    for (;;) {
        refresh_topbar(sys_get_ticks());

        term_puts(PROMPT);
        int n = term_readline(line, LINE_MAX);
        if (n == 0) continue;

        /* Trim trailing whitespace */
        while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t'))
            line[--n] = '\0';
        if (n == 0) continue;

        int argc = parse_args(line, argv, ARGV_MAX);
        if (argc == 0) continue;

        const char *cmd = argv[0];

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "echo") == 0) {
            cmd_echo(argc, argv);
        } else if (strcmp(cmd, "ls") == 0) {
            cmd_ls();
        } else if (strcmp(cmd, "clear") == 0) {
            cmd_clear();
        } else if (strcmp(cmd, "uname") == 0) {
            cmd_uname();
        } else if (strcmp(cmd, "exit") == 0) {
            int code = (argc > 1) ? atoi(argv[1]) : 0;
            term_puts("Goodbye!\n");
            exit(code);
        } else {
            term_printf("aesh: %s: command not found\n", cmd);
        }
    }

    return 0;
}
