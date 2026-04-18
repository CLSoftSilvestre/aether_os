/*
 * AetherOS — Graphical Desktop Shell
 * File: userspace/apps/init/main.c
 *
 * Phase 4.2: Lumina desktop with terminal + System Info sidebar.
 *
 * Desktop layout (1024x768):
 *   [0]    Top bar    (1024x36) — branding + HH:MM:SS uptime
 *   [36]   Accent     (1024x2)  — purple separator
 *   [38]   Main area  (1024x706) — terminal window + sidebar
 *   [744]  Bot bar    (1024x24) — status + free memory
 *
 * Terminal window: 656x604 at x=8 (80 cols x 70 rows at 8x8)
 *   Title bar: 656x28
 *   Client:    80 cols x 70 rows (640x560)
 *
 * System Info sidebar: 344x280 at x=672 (right of terminal)
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

#define FONT_W  8
#define FONT_H  8

/* Terminal window */
#define TERM_COLS  80
#define TERM_ROWS  70
#define WIN_W     (TERM_COLS * FONT_W + 16)          /* 656 */
#define WIN_H     (TITLE_H + 8 + TERM_ROWS * FONT_H + 8)  /* 604 */
#define WIN_X      8
#define WIN_Y     (ACCENT_Y + ACCENT_H + \
                   (BOTBAR_Y - ACCENT_Y - ACCENT_H - WIN_H) / 2)
#define TITLE_H    28

/* Terminal text area */
#define TERM_X    (WIN_X + 8)
#define TERM_Y    (WIN_Y + TITLE_H + 8)
#define TERM_W    (TERM_COLS * FONT_W)   /* 640 */
#define TERM_H    (TERM_ROWS * FONT_H)   /* 560 */

/* System info sidebar */
#define SIDE_X    (WIN_X + WIN_W + 8)    /* 672 */
#define SIDE_W    (1024 - SIDE_X - 8)    /* 344 */
#define SIDE_Y     WIN_Y
#define SIDE_TH    24                    /* sidebar title bar height */
#define SIDE_H     280

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

/* ── Time formatting ─────────────────────────────────────────────────── */

static void fmt_uptime(char *buf, long ticks)
{
    long s = ticks / 100;
    long m = s / 60;  s %= 60;
    long h = m / 60;  m %= 60;
    snprintf(buf, 16, "%02ld:%02ld:%02ld", h, m, s);
}

/* ── Desktop drawing ─────────────────────────────────────────────────── */

static void draw_desktop(void)
{
    gfx_fill(0, 0, 1024, 768, C_DESKTOP);
}

static void draw_top_bar(long ticks)
{
    gfx_fill(0, TOPBAR_Y, 1024, TOPBAR_H, C_PANEL);

    gfx_text(14, TOPBAR_Y + 10, "AetherOS", C_TEXT, C_PANEL);
    gfx_text(14 + 8 * 8 + 8, TOPBAR_Y + 10, "v0.0.5", C_TEXT_DIM, C_PANEL);

    gfx_text_center(0, 1024, TOPBAR_Y + 10,
                    "Phase 4.2  --  Lumina Desktop", C_TEXT_DIM, C_PANEL);

    char tbuf[16];
    fmt_uptime(tbuf, ticks);
    /* "up HH:MM:SS" — 11 chars */
    char ubuf[20];
    snprintf(ubuf, sizeof(ubuf), "up %s", tbuf);
    int len = (int)strlen(ubuf);
    gfx_text(1024 - len * FONT_W - 14, TOPBAR_Y + 10,
             ubuf, C_TEXT_DIM, C_PANEL);

    gfx_fill(0, ACCENT_Y, 1024, ACCENT_H, C_ACCENT);
}

static void draw_bot_bar(long ticks)
{
    gfx_fill(0, BOTBAR_Y, 1024, BOTBAR_H, C_PANEL);
    gfx_hline(0, BOTBAR_Y, 1024, C_SEP);

    /* Left: static info */
    gfx_text(14, BOTBAR_Y + 6,
             "AetherOS 0.0.5  |  QEMU virt  |  Cortex-A76  |  1024x768",
             C_TEXT_DIM, C_PANEL);

    /* Right: free memory */
    long v = sys_pmm_stats();
    unsigned long free_pages  = (unsigned long)((unsigned long long)v >> 32);
    unsigned long free_mb     = free_pages * 4 / 1024;
    char mbuf[24];
    snprintf(mbuf, sizeof(mbuf), "Free: %lu MB", free_mb);
    int mlen = (int)strlen(mbuf);
    gfx_text(1024 - mlen * FONT_W - 14, BOTBAR_Y + 6,
             mbuf, C_TEXT_DIM, C_PANEL);
    (void)ticks;
}

static void draw_window(void)
{
    /* Shadow */
    gfx_fill(WIN_X + 4, WIN_Y + 4, WIN_W, WIN_H, GFX_RGB(8, 8, 14));

    /* Window body */
    gfx_fill(WIN_X, WIN_Y, WIN_W, WIN_H, C_WIN_BG);

    /* Title bar */
    gfx_fill(WIN_X, WIN_Y, WIN_W, TITLE_H, C_TITLEBAR);

    /* Traffic lights */
    gfx_fill(WIN_X + 10, WIN_Y + 8, 12, 12, C_RED);
    gfx_fill(WIN_X + 26, WIN_Y + 8, 12, 12, C_YELLOW);
    gfx_fill(WIN_X + 42, WIN_Y + 8, 12, 12, C_GREEN);

    gfx_text_center(WIN_X, WIN_W, WIN_Y + 8,
                    "aesh  --  AetherOS Terminal", C_TEXT, C_TITLEBAR);

    gfx_hline(WIN_X, WIN_Y + TITLE_H, WIN_W, C_ACCENT);

    gfx_fill(WIN_X, WIN_Y + TITLE_H + 1,
             WIN_W, WIN_H - TITLE_H - 1, C_TERM_BG);

    gfx_rect(WIN_X, WIN_Y, WIN_W, WIN_H, C_SEP);
}

/* Draw the System Info sidebar (static frame + initial values) */
static void draw_sidebar(long ticks)
{
    /* Shadow */
    gfx_fill(SIDE_X + 3, SIDE_Y + 3, SIDE_W, SIDE_H, GFX_RGB(8, 8, 14));

    /* Body */
    gfx_fill(SIDE_X, SIDE_Y, SIDE_W, SIDE_H, C_WIN_BG);

    /* Title bar */
    gfx_fill(SIDE_X, SIDE_Y, SIDE_W, SIDE_TH, C_TITLEBAR);
    gfx_text_center(SIDE_X, SIDE_W, SIDE_Y + 6,
                    "System Info", C_TEXT, C_TITLEBAR);
    gfx_hline(SIDE_X, SIDE_Y + SIDE_TH, SIDE_W, C_ACCENT);

    gfx_rect(SIDE_X, SIDE_Y, SIDE_W, SIDE_H, C_SEP);

    /* Section: Uptime */
    int y = SIDE_Y + SIDE_TH + 12;
    gfx_text(SIDE_X + 10, y, "Uptime", C_TEXT_DIM, C_WIN_BG);
    y += FONT_H + 2;

    char tbuf[16];
    fmt_uptime(tbuf, ticks);
    gfx_text(SIDE_X + 10, y, tbuf, C_ACCENT, C_WIN_BG);
    y += FONT_H + 14;

    /* Section: Memory */
    gfx_hline(SIDE_X + 10, y, SIDE_W - 20, C_SEP);
    y += 6;
    gfx_text(SIDE_X + 10, y, "Memory", C_TEXT_DIM, C_WIN_BG);
    y += FONT_H + 4;

    long v = sys_pmm_stats();
    unsigned long free_p  = (unsigned long)((unsigned long long)v >> 32);
    unsigned long total_p = (unsigned long)((unsigned long long)v & 0xFFFFFFFFUL);
    unsigned long free_mb  = free_p  * 4 / 1024;
    unsigned long total_mb = total_p * 4 / 1024;

    char mbuf[32];
    snprintf(mbuf, sizeof(mbuf), "%lu MB free / %lu MB", free_mb, total_mb);
    gfx_text(SIDE_X + 10, y, mbuf, C_TEXT, C_WIN_BG);
    y += FONT_H + 4;

    /* Memory bar */
    unsigned bar_w = (unsigned)(SIDE_W - 20);
    unsigned used_w = (unsigned)(bar_w - (bar_w * free_p / total_p));
    gfx_fill(SIDE_X + 10, y, used_w, 8, C_ACCENT);
    gfx_fill(SIDE_X + 10 + (int)used_w, y,
             bar_w - used_w, 8, GFX_RGB(40, 40, 60));
    y += 8 + 14;

    /* Section: System */
    gfx_hline(SIDE_X + 10, y, SIDE_W - 20, C_SEP);
    y += 6;
    gfx_text(SIDE_X + 10, y, "Platform", C_TEXT_DIM, C_WIN_BG);
    y += FONT_H + 4;
    gfx_text(SIDE_X + 10, y, "QEMU virt", C_TEXT, C_WIN_BG);
    y += FONT_H + 2;
    gfx_text(SIDE_X + 10, y, "Cortex-A76 / AArch64", C_TEXT, C_WIN_BG);
    y += FONT_H + 2;
    gfx_text(SIDE_X + 10, y, "AetherOS v0.0.5", C_TEXT, C_WIN_BG);
}

/* Refresh only the live parts of the top bar and sidebar */
static void refresh_live(long ticks)
{
    /* Top bar: uptime only */
    int uptime_x = 1024 - 14 * FONT_W - 14;
    gfx_fill(uptime_x - 2, TOPBAR_Y, 1024 - (uptime_x - 2), TOPBAR_H, C_PANEL);
    char ubuf[20];
    fmt_uptime(ubuf, ticks);
    char disp[24];
    snprintf(disp, sizeof(disp), "up %s", ubuf);
    int len = (int)strlen(disp);
    gfx_text(1024 - len * FONT_W - 14, TOPBAR_Y + 10,
             disp, C_TEXT_DIM, C_PANEL);

    /* Sidebar: uptime value */
    int uy = SIDE_Y + SIDE_TH + 12 + FONT_H + 2;
    gfx_fill(SIDE_X + 10, uy, SIDE_W - 20, FONT_H, C_WIN_BG);
    char tbuf[16];
    fmt_uptime(tbuf, ticks);
    gfx_text(SIDE_X + 10, uy, tbuf, C_ACCENT, C_WIN_BG);
}

/* ── Shell built-ins ─────────────────────────────────────────────────── */

#define PROMPT   "aesh> "
#define ARGV_MAX  16
#define LINE_MAX  256

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
    term_puts("  help              show this message\n");
    term_puts("  echo [args]       print arguments\n");
    term_puts("  ls                list files in initrd\n");
    term_puts("  cat <file>        print a file from initrd\n");
    term_puts("  mem               show memory statistics\n");
    term_puts("  time              show formatted uptime\n");
    term_puts("  clear             clear terminal\n");
    term_puts("  uname             print OS information\n");
    term_puts("  pid               print current process ID\n");
    term_puts("  spawn <path>      launch an ELF from initrd (wait)\n");
    term_puts("  spawn <path> &    launch in background (no wait)\n");
    term_puts("  exit [code]       exit the shell\n");
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
    if (!name || name[0] == '\0') {
        term_puts("usage: cat <filename>\n");
        return;
    }
    char buf[4096];
    long n = sys_initrd_read(name, buf, (long)sizeof(buf) - 1);
    if (n < 0) {
        term_printf("cat: %s: not found\n", name);
        return;
    }
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
    unsigned long used_mb  = total_mb - free_mb;
    term_printf("Memory: %lu MB used  /  %lu MB free  /  %lu MB total\n",
                used_mb, free_mb, total_mb);
    term_printf("Pages:  %lu free  /  %lu total  (%lu KB page size)\n",
                free_p, total_p, 4UL);
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
    term_puts("AetherOS  aarch64  Phase 4.3  QEMU virt (Cortex-A76)\n");
}

static void cmd_pid(void)
{
    long pid = sys_getpid();
    term_printf("PID: %ld\n", pid);
}

static void cmd_spawn(const char *path, int background)
{
    if (!path || path[0] == '\0') {
        term_puts("usage: spawn <initrd-path>\n");
        return;
    }
    term_printf("Spawning '%s'%s...\n", path, background ? " [bg]" : "");
    long child = sys_spawn(path);
    if (child < 0) {
        term_printf("spawn: failed to launch '%s'\n", path);
        return;
    }
    term_printf("Child PID %ld started\n", child);
    if (!background) {
        int status = 0;
        sys_waitpid(child, &status);
        term_printf("Child PID %ld exited (status %d)\n", child, status);
    }
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void)
{
    sys_fb_claim();
    gfx_init();

    draw_desktop();
    draw_top_bar(sys_get_ticks());
    draw_bot_bar(sys_get_ticks());
    draw_window();
    draw_sidebar(sys_get_ticks());

    term_init();

    /* Show motd from initrd, fall back to hardcoded banner */
    {
        char motd[2048];
        long n = sys_initrd_read("motd.txt", motd, (long)sizeof(motd) - 1);
        if (n > 0) {
            motd[n] = '\0';
            term_puts(motd);
        } else {
            term_puts("\n  Welcome to AetherOS Phase 4.2\n");
            term_puts("  Lumina Graphical Desktop\n");
            term_puts("  Type 'help' for available commands.\n\n");
        }
    }

    char line[LINE_MAX];
    char *argv[ARGV_MAX];

    for (;;) {
        long ticks = sys_get_ticks();
        refresh_live(ticks);
        draw_bot_bar(ticks);

        term_puts(PROMPT);
        int n = term_readline(line, LINE_MAX);
        if (n == 0) continue;

        /* Trim trailing whitespace */
        while (n > 0 && (line[n-1] == ' ' || line[n-1] == '\t'))
            line[--n] = '\0';
        if (n == 0) continue;

        /* Detect trailing '&' for background execution */
        int background = 0;
        if (n > 0 && line[n-1] == '&') {
            background = 1;
            line[--n] = '\0';
            while (n > 0 && line[n-1] == ' ') line[--n] = '\0';
        }

        int argc = parse_args(line, argv, ARGV_MAX);
        if (argc == 0) continue;

        const char *cmd = argv[0];

        if (strcmp(cmd, "help") == 0) {
            cmd_help();
        } else if (strcmp(cmd, "echo") == 0) {
            cmd_echo(argc, argv);
        } else if (strcmp(cmd, "ls") == 0) {
            cmd_ls();
        } else if (strcmp(cmd, "cat") == 0) {
            cmd_cat(argc > 1 ? argv[1] : NULL);
        } else if (strcmp(cmd, "mem") == 0) {
            cmd_mem();
        } else if (strcmp(cmd, "time") == 0) {
            cmd_time(sys_get_ticks());
        } else if (strcmp(cmd, "clear") == 0) {
            cmd_clear();
        } else if (strcmp(cmd, "uname") == 0) {
            cmd_uname();
        } else if (strcmp(cmd, "pid") == 0) {
            cmd_pid();
        } else if (strcmp(cmd, "spawn") == 0) {
            cmd_spawn(argc > 1 ? argv[1] : NULL, background);
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
