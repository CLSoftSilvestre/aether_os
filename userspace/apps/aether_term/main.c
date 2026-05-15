/*
 * AetherOS — AetherTerm
 * File: userspace/apps/aether_term/main.c
 *
 * Phase 5.6: adds CWD tracking and filesystem navigation commands (cd, mkdir,
 * pwd, touch, rm) on top of Phase 5.1 networking.
 *
 * Window position is stored in g_win_x / g_win_y (mutable globals).
 * All drawing uses WX / WY macros that add the current offset so the window
 * redraws correctly after a drag.
 */

#include <gfx.h>
#include <gpu.h>
#include <widget.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys.h>
#include <input.h>

/* ── Layout constants (sizes only — positions are runtime) ───────────────── */

#define TOPBAR_H   36
#define ACCENT_H    2
#define BOTBAR_Y  696   /* 720 - BOTBAR_H(24) */

#define FONT_W   8
#define FONT_H  16

#define TERM_COLS  80
#define TERM_ROWS  32   /* (BOTBAR_Y - TOPBAR_H - ACCENT_H - TITLE_H - 16) / FONT_H */
#define WIN_W     (TERM_COLS * FONT_W + 16)
#define TITLE_H    28
#define WIN_H     (TITLE_H + 8 + TERM_ROWS * FONT_H + 8)

/* Initial position (computed once, may change after a drag) */
#define WIN_X_INIT   ((1280 / 2) - (WIN_W / 2))
#define WIN_Y_INIT   ((TOPBAR_H + ACCENT_H + \
                      (BOTBAR_Y - TOPBAR_H - ACCENT_H - WIN_H) / 2) - 25)

/* Runtime window position — updated via WM_EV_REDRAW */
static int g_win_x = WIN_X_INIT;
static int g_win_y = WIN_Y_INIT;

/* WM window handle */
static long g_win_id = -1;

/* GPU BO for compositor path (NULL = legacy FB) */
static gpu_bo_t      g_term_bo  = GPU_BO_INVALID;
static unsigned int *g_bo_ptr   = NULL;

/* Nested frame depth counter — only the outermost begin/end call gfx APIs */
static int g_frame_depth = 0;

static void term_frame_begin(void)
{
    if (g_bo_ptr && g_frame_depth == 0)
        gfx_begin_frame(g_bo_ptr, WIN_W, WIN_H, g_win_x, g_win_y);
    g_frame_depth++;
}

static void term_frame_end(void)
{
    if (--g_frame_depth == 0 && g_bo_ptr)
        gfx_end_frame();
}

/* Position accessors used throughout (single point of offset) */
#define WX  g_win_x
#define WY  g_win_y
#define TX  (g_win_x + 8)
#define TY  (g_win_y + TITLE_H + 8)

/* ── Terminal emulator state ─────────────────────────────────────────────── */

static char t_buf[TERM_ROWS][TERM_COLS];
static int  t_col;
static int  t_row;

/* ── Transparent background (kernel-blended) ──────────────────────────────
 * SYS_WP_BLEND_FILL runs entirely in the kernel: reads init's wallpaper
 * buffer (registered via SYS_WP_REGISTER) and writes 80% C_TERM_BG +
 * 20% wallpaper directly into the framebuffer.  No cross-process pointer
 * access needed — the kernel has access to all physical memory. */
#define CONTENT_H  (WIN_H - TITLE_H - 1)

static void blit_term_bg(void)
{
    gfx_fill((unsigned)(WX + 2), (unsigned)(WY + TITLE_H + 1),
             (unsigned)(WIN_W - 4), (unsigned)(CONTENT_H - 2), C_TERM_BG);
}

static void blit_term_bg_row(int row)
{
    gfx_fill((unsigned)(TX), (unsigned)(TY + row * FONT_H),
             (unsigned)(WIN_W - 16), (unsigned)FONT_H, C_TERM_BG);
}

static void blit_term_bg_cell(int row, int col)
{
    gfx_fill((unsigned)(TX + col * FONT_W), (unsigned)(TY + row * FONT_H),
             (unsigned)FONT_W, (unsigned)FONT_H, C_TERM_BG);
}

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
    term_frame_begin();
    blit_term_bg_row(row);
    for (int c = 0; c < TERM_COLS; c++) {
        if (t_buf[row][c] != ' ')
            gfx_char_transparent(TX + c * FONT_W, TY + row * FONT_H,
                                 t_buf[row][c], C_TEXT);
    }
    term_frame_end();
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
    term_frame_begin();
    blit_term_bg_cell(t_row, t_col);
    if (t_buf[t_row][t_col] != ' ')
        gfx_char_transparent(TX + t_col * FONT_W, TY + t_row * FONT_H,
                             t_buf[t_row][t_col], C_TEXT);
    term_frame_end();
}

static void term_draw_cursor(void)
{
    term_frame_begin();
    gfx_fill(TX + t_col * FONT_W,
             TY + t_row * FONT_H,
             FONT_W, FONT_H, C_ACCENT);
    if (t_buf[t_row][t_col] != ' ')
        gfx_char(TX + t_col * FONT_W,
                 TY + t_row * FONT_H,
                 t_buf[t_row][t_col], C_TERM_BG, C_ACCENT);
    term_frame_end();
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
        term_frame_begin();
        blit_term_bg_cell(t_row, t_col);
        term_frame_end();
        return;
    }
    if ((unsigned char)c < 32) return;

    t_buf[t_row][t_col] = c;
    term_frame_begin();
    blit_term_bg_cell(t_row, t_col);
    gfx_char_transparent(TX + t_col * FONT_W, TY + t_row * FONT_H,
                         c, C_TEXT);
    term_frame_end();
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

/* keycode → ASCII (unshifted / shifted) */
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

/* ── Window drawing ──────────────────────────────────────────────────────── */

static void draw_window(void)
{
    term_frame_begin();
    gfx_glass_window_frame(WX, WY, WIN_W, WIN_H,
                            TITLE_H, "Aether Terminal  --  aesh", 0);
    blit_term_bg();
    term_frame_end();
}

static void redraw_after_child(void)
{
    term_frame_begin();
    draw_window();
    term_redraw_all();
    term_frame_end();
}

/* ── Time formatting ─────────────────────────────────────────────────────── */

static void fmt_uptime(char *buf, long ticks)
{
    long s = ticks / 100, m = s / 60; s %= 60;
    long h = m / 60;                  m %= 60;
    snprintf(buf, 16, "%02ld:%02ld:%02ld", h, m, s);
}

/* ── Readline ────────────────────────────────────────────────────────────── */

/*
 * term_readline — WM-routed line editor.
 *
 * Uses sys_wm_key_recv() instead of sys_key_read() so keyboard input only
 * arrives when this window has focus.  Handles WM_EV_REDRAW inline: updates
 * g_win_x / g_win_y and redraws the window at the new position.
 */
static int term_readline(char *buf, int max)
{
    int n   = 0;
    int pos = 0;

    term_draw_cursor();

    while (n < max - 1) {
        unsigned long long raw = sys_wm_key_recv();

        /* WM_EV_REDRAW: window was dragged — repaint at new position */
        if (wm_event_is_redraw(raw)) {
            g_win_x = wm_event_redraw_x(raw);
            g_win_y = wm_event_redraw_y(raw);
            term_frame_begin();
            draw_window();
            term_redraw_all();
            term_draw_cursor();
            term_frame_end();
            continue;
        }

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

        /* Backspace */
        if (ev.keycode == KEY_BACKSPACE && pos > 0) {
            term_erase_cursor();
            for (int i = pos - 1; i < n - 1; i++) buf[i] = buf[i + 1];
            n--; pos--;
            buf[n] = '\0';
            t_col--;  /* move to the deleted character's screen position */
            term_frame_begin();
            for (int i = pos; i < n; i++) {
                t_buf[t_row][t_col] = buf[i];
                gfx_char(TX + t_col * FONT_W, TY + t_row * FONT_H,
                         buf[i], C_TEXT, C_TERM_BG);
                t_col++;
            }
            t_buf[t_row][t_col] = ' ';
            gfx_char(TX + t_col * FONT_W, TY + t_row * FONT_H,
                     ' ', C_TEXT, C_TERM_BG);
            t_col -= (n - pos);
            term_frame_end();
            term_draw_cursor();
            continue;
        }

        /* Arrow LEFT */
        if (ev.keycode == KEY_LEFT && pos > 0) {
            term_erase_cursor();
            pos--; t_col--;
            term_draw_cursor();
            continue;
        }

        /* Arrow RIGHT */
        if (ev.keycode == KEY_RIGHT && pos < n) {
            term_erase_cursor();
            pos++; t_col++;
            term_draw_cursor();
            continue;
        }

        /* Printable character */
        char c = term_key_to_char(ev);
        if (!c) continue;

        term_erase_cursor();
        for (int i = n; i > pos; i--) buf[i] = buf[i - 1];
        buf[pos] = c;
        n++; pos++;
        buf[n] = '\0';
        int save_col = t_col;
        term_frame_begin();
        for (int i = pos - 1; i < n; i++) {
            t_buf[t_row][t_col] = buf[i];
            gfx_char(TX + t_col * FONT_W, TY + t_row * FONT_H,
                     buf[i], C_TEXT, C_TERM_BG);
            t_col++;
        }
        t_col = save_col + 1;
        term_frame_end();
        term_draw_cursor();
    }
    buf[n] = '\0';
    return n;
}

/* ── Shell built-ins ─────────────────────────────────────────────────────── */

#define ARGV_MAX   16
#define LINE_MAX  256

/* ── Current working directory ───────────────────────────────────────────── */

#define CWD_MAX  256
static char g_cwd[CWD_MAX] = "/";

/* Print the prompt including the CWD. */
static void print_prompt(void)
{
    term_puts("aesh[");
    term_puts(g_cwd);
    term_puts("]> ");
}

/*
 * path_resolve — build an absolute path from the CWD + user-supplied path.
 *
 * Rules:
 *   - Empty or NULL  → copy CWD into out
 *   - Starts with /  → absolute; copy as-is
 *   - Anything else  → join CWD + "/" + path, then normalize
 *
 * Normalisation collapses "." components and resolves ".." against the
 * accumulated result.  The result is always "/"-prefixed and never has a
 * trailing slash (except for the root itself).
 */
static void path_resolve(char *out, int outsz, const char *rel)
{
    char tmp[CWD_MAX * 2];
    int n = 0;

    if (!rel || rel[0] == '\0') {
        /* empty → stay in CWD */
        int i;
        for (i = 0; g_cwd[i] && i < outsz - 1; i++) out[i] = g_cwd[i];
        out[i] = '\0';
        return;
    }

    if (rel[0] == '/') {
        /* absolute path */
        for (int i = 0; rel[i] && i < (int)sizeof(tmp) - 1; i++) tmp[n++] = rel[i];
    } else {
        /* prepend CWD */
        for (int i = 0; g_cwd[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = g_cwd[i];
        if (n > 1 && tmp[n-1] != '/' && n < (int)sizeof(tmp) - 1) tmp[n++] = '/';
        for (int i = 0; rel[i] && n < (int)sizeof(tmp) - 1; i++) tmp[n++] = rel[i];
    }
    tmp[n] = '\0';

    /* Normalise: walk components, build result in out[] */
    /* We use a simple stack stored inside out itself: out[0]='/' always. */
    out[0] = '/'; out[1] = '\0';
    int outlen = 1;

    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;   /* skip slashes */
        if (!*p) break;

        /* collect one component */
        const char *start = p;
        while (*p && *p != '/') p++;
        int clen = (int)(p - start);

        if (clen == 1 && start[0] == '.') continue;  /* skip "." */

        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            /* go up one level */
            if (outlen > 1) {
                outlen--;
                while (outlen > 1 && out[outlen-1] != '/') outlen--;
                if (outlen > 1 && out[outlen-1] == '/') outlen--;
            }
            out[outlen] = '\0';
            continue;
        }

        /* append /component */
        if (outlen + 1 + clen + 1 >= outsz) break;  /* would overflow */
        if (outlen > 1) out[outlen++] = '/';
        for (int i = 0; i < clen; i++) out[outlen++] = start[i];
        out[outlen] = '\0';
    }
    if (outlen == 0) { out[0] = '/'; out[1] = '\0'; }
}

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
    /* term_puts("  help              show this message\n");*/
    term_puts("  echo [args]       print arguments\n");
    term_puts("  pwd               print working directory\n");
    term_puts("  cd [path]         change directory (.. goes up, / is root)\n");
    term_puts("  ls [path]         list directory (default: CWD)\n");
    term_puts("  mkdir <path>      create a directory (FAT32 only)\n");
    term_puts("  touch <path>      create an empty file (FAT32 only)\n");
    term_puts("  rm <path>         remove a file (FAT32 only)\n");
    term_puts("  cat <path>        print a file (disk or initrd)\n");
    term_puts("  mount             show mounted filesystems\n");
    term_puts("  disk              show disk usage\n");
    term_puts("  mem               show memory statistics\n");
    term_puts("  time              show formatted uptime\n");
    term_puts("  clear             clear terminal\n");
    term_puts("  uname             print OS information\n");
    term_puts("  pid               print current process ID\n");
    term_puts("  ps                list all running processes\n");
    term_puts("  kill <pid>        terminate a process by PID\n");
    /* term_puts("  files             launch graphical file browser\n"); */
    /* term_puts("  view              launch text viewer\n"); */
    term_puts("  spawn <path>      launch an ELF from initrd (wait)\n");
    term_puts("  spawn <path> &    launch in background (no wait)\n");
    term_puts("  exit [code]       exit the terminal\n");
    /* term_puts("Filesystem paths:\n");
    term_puts("  /           FAT32 disk root (when disk.img attached)\n");
    term_puts("  /initrd/    embedded CPIO initrd (always available)\n");
    term_puts("  /afs/       AetherOS Filesystem (virtio-blk hd1)\n");
    term_puts("  Relative paths are resolved against the current CWD.\n"); */
    term_puts("\nNetworking:\n");
    term_puts("  net               show IP/MAC/gateway/DNS\n");
    term_puts("  ping <ip>         ICMP echo to IP address\n");
    term_puts("  nslookup <host>   DNS A-record lookup\n");
    term_puts("  wget <ip>:<port><path>  HTTP GET (first 512 bytes)\n");
    term_puts("  http <url>          HTTP/1.1 client (Content-Length + chunked)\n");
    term_puts("    e.g. http http://10.0.2.2:8080/\n");
}

static void cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (i > 1) term_putc(' ');
        term_puts(argv[i]);
    }
    term_putc('\n');
}

static void cmd_ls(const char *path)
{
    char resolved[CWD_MAX];
    path_resolve(resolved, sizeof(resolved), path);

    char buf[4096];
    long n = sys_fs_readdir(resolved, buf, (long)sizeof(buf) - 1);
    if (n < 0) {
        term_printf("ls: %s: no such directory\n", resolved);
        return;
    }
    if (n == 0) { term_puts("(empty)\n"); return; }
    buf[n] = '\0';
    term_printf("[%s]\n", resolved);
    term_puts(buf);
    if (n > 0 && buf[n-1] != '\n') term_putc('\n');
}

static void cmd_cat(const char *path)
{
    if (!path || path[0] == '\0') { term_puts("usage: cat <path>\n"); return; }

    char resolved[CWD_MAX];
    path_resolve(resolved, sizeof(resolved), path);

    /* Try VFS first (handles both disk and /initrd/ paths) */
    char buf[4096];
    long vfd = sys_fs_open(resolved);
    if (vfd >= 0) {
        long total = 0;
        long n;
        while ((n = sys_fs_read(vfd, (void *)(buf + total),
                                (long)sizeof(buf) - 1 - total)) > 0) {
            total += n;
            if (total >= (long)sizeof(buf) - 1) break;
        }
        sys_fs_close(vfd);
        buf[total] = '\0';
        term_puts(buf);
        if (total > 0 && buf[total-1] != '\n') term_putc('\n');
        return;
    }

    /* Fallback: try initrd directly (bare filename without /initrd/ prefix) */
    long n = sys_initrd_read(resolved, buf, (long)sizeof(buf) - 1);
    if (n < 0) { term_printf("cat: %s: not found\n", resolved); return; }
    buf[n] = '\0';
    term_puts(buf);
    if (n > 0 && buf[n-1] != '\n') term_putc('\n');
}

/* ── Navigation commands ─────────────────────────────────────────────────── */

static void cmd_pwd(void)
{
    term_puts(g_cwd);
    term_putc('\n');
}

static void cmd_cd(const char *path)
{
    if (!path || path[0] == '\0') {
        /* cd with no args → go to root, like a minimal shell convention */
        g_cwd[0] = '/'; g_cwd[1] = '\0';
        return;
    }

    char candidate[CWD_MAX];
    path_resolve(candidate, sizeof(candidate), path);

    /* Verify the target is a real directory by trying readdir */
    char probe[16];
    long n = sys_fs_readdir(candidate, probe, sizeof(probe));
    if (n < 0) {
        term_printf("cd: %s: no such directory\n", candidate);
        return;
    }

    for (int i = 0; i < CWD_MAX; i++) { g_cwd[i] = candidate[i]; if (!candidate[i]) break; }
}

static void cmd_mkdir(const char *path)
{
    if (!path || path[0] == '\0') { term_puts("usage: mkdir <path>\n"); return; }

    char resolved[CWD_MAX];
    path_resolve(resolved, sizeof(resolved), path);

    if (sys_fs_mkdir(resolved) < 0)
        term_printf("mkdir: %s: failed (exists, read-only fs, or disk full)\n", resolved);
    else
        term_printf("mkdir: created %s\n", resolved);
}

static void cmd_touch(const char *path)
{
    if (!path || path[0] == '\0') { term_puts("usage: touch <path>\n"); return; }

    char resolved[CWD_MAX];
    path_resolve(resolved, sizeof(resolved), path);

    long vfd = sys_fs_create(resolved);
    if (vfd < 0) {
        term_printf("touch: %s: failed\n", resolved);
        return;
    }
    sys_fs_close(vfd);
}

static void cmd_rm(const char *path)
{

    if (!path || path[0] == '\0') { term_puts("usage: rm <path>\n"); return; }

    char resolved[CWD_MAX];
    path_resolve(resolved, sizeof(resolved), path);

    if (sys_fs_rm(resolved) < 0)
        term_printf("rm: %s: failed (file does not exist or directory is not empty)\n", resolved);
    else{
        term_printf("rm: %s deleted with success\n", resolved);
    }

}

static void cmd_mount(void)
{
    term_puts("Mounted filesystems:\n");
    term_puts("  /initrd    initrd (CPIO, embedded in kernel)\n");

    char buf[64];
    long n = sys_fs_readdir("/", buf, sizeof(buf));
    if (n > 0 && buf[0] != '(')
        term_puts("  /          FAT32  (virtio-blk hd0)\n");
    else
        term_puts("  /          (no FAT32 — run make_disk.sh and attach disk.img)\n");

    n = sys_fs_readdir("/afs", buf, sizeof(buf));
    if (n > 0 && buf[0] != '(')
        term_puts("  /afs       AetherFS (virtio-blk hd1)\n");
    else
        term_puts("  /afs       (no AetherFS — run make_afs.sh and attach afs.img)\n");
}

static void cmd_disk(void)
{
    char buf[32];
    long n = sys_fs_readdir("/", buf, sizeof(buf));
    if (n < 0 || (n > 0 && buf[0] == '(')) {
        term_puts("disk: no FAT32 disk attached\n");
        term_puts("      Create one with: bash scripts/make_disk.sh\n");
        return;
    }
    term_puts("Disk (virtio-blk, FAT32):\n");
    term_puts("  Size:   32 MB\n");
    term_puts("  Mount:  /\n");
    term_puts("  Use 'ls /' or 'cat /readme.txt' to access files.\n");
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

static void epoch_to_datetime(unsigned long epoch,
                               int *yr, int *mo, int *dy,
                               int *hr, int *mn, int *sc)
{
    static const int mdays[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    *sc = (int)(epoch % 60); epoch /= 60;
    *mn = (int)(epoch % 60); epoch /= 60;
    *hr = (int)(epoch % 24); epoch /= 24;
    long days = (long)epoch;
    int y = 1970;
    for (;;) {
        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        if (days < (leap ? 366 : 365)) break;
        days -= (leap ? 366 : 365); y++;
    }
    *yr = y;
    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    int m;
    for (m = 0; m < 12; m++) {
        int md = mdays[m] + (m == 1 && leap ? 1 : 0);
        if (days < md) break;
        days -= md;
    }
    *mo = m + 1;
    *dy = (int)days + 1;
}

static void cmd_time(void)
{
    unsigned long epoch = sys_rtc_get();
    long ticks = sys_get_ticks();
    int yr, mo, dy, hr, mn, sc;
    epoch_to_datetime(epoch, &yr, &mo, &dy, &hr, &mn, &sc);
    term_printf("Date/Time: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                yr, mo, dy, hr, mn, sc);
    char tbuf[16];
    fmt_uptime(tbuf, ticks);
    term_printf("Uptime:    %s  (%ld ticks at 100 Hz)\n", tbuf, ticks);
}

static void cmd_clear(void)
{
    blit_term_bg();
    for (int r = 0; r < TERM_ROWS; r++) term_clear_row(r);
    t_col = 0;
    t_row = 0;
}

static void cmd_uname(void)
{
    term_puts("AetherOS kernel aarch64 (Cortex-A76)\n");
    term_puts("(c) Azordev.pt All rights reserved.\n");
}

static void cmd_pid(void)
{
    term_printf("PID: %ld\n", sys_getpid());
}

static void cmd_ps(void)
{
    ps_entry_t procs[32];
    long n = sys_ps(procs, 32);
    if (n < 0) { term_puts("ps: failed\n"); return; }
    static const char *state_names[] = {
        "unused", "ready", "running", "sleeping", "dead", "zombie", "waiting",
    };
    term_puts("  PID  PPID  STATE     NAME\n");
    for (long i = 0; i < n; i++) {
        int s = procs[i].state;
        if (s < 0 || s > 6) s = 0;
        term_printf("  %3u  %4u  %s  %s\n",
                    procs[i].pid, procs[i].ppid,
                    state_names[s], procs[i].name);
    }
}

static void cmd_kill(const char *pid_str)
{
    if (!pid_str || pid_str[0] == '\0') { term_puts("usage: kill <pid>\n"); return; }
    long pid = 0;
    for (int i = 0; pid_str[i] >= '0' && pid_str[i] <= '9'; i++)
        pid = pid * 10 + (pid_str[i] - '0');
    if (pid <= 0) { term_puts("kill: invalid PID\n"); return; }
    if (sys_kill(pid) < 0)
        term_printf("kill: PID %ld: failed\n", pid);
    else
        term_printf("kill: PID %ld signalled\n", pid);
}

/*
 * wait_foreground — poll for child exit while watching for Ctrl+C.
 * Reclaims WM focus when the child exits or is killed.
 */
static int wait_foreground(long child)
{
    int status = 0;
    for (;;) {
        long r = sys_waitpid_nb(child, &status);
        if (r != 0) break;

        /* Check hardware ring directly (not WM FIFO) — safe because we are
         * not in sys_wm_key_recv() here, so hardware events stay in the ring */
        unsigned long long ke = sys_key_poll();
        if (ke) {
            key_event_t ev = key_event_unpack(ke);
            if (ev.is_press && (ev.modifiers & MOD_CTRL) && ev.keycode == KEY_C) {
                sys_kill(child);
                while (sys_key_poll()) {}
                term_puts("^C\n");
                sys_waitpid(child, &status);
                status = -1;
                break;
            }
        }
        sys_sleep(1);
    }
    /* Reclaim keyboard focus now that the child is done */
    sys_wm_focus_set(sys_getpid());
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

/* ── Networking commands (Phase 5.1) ─────────────────────────────────────── */

static void fmt_ip(char *buf, unsigned int ip)
{
    snprintf(buf, 16, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8)  & 0xFF,  ip        & 0xFF);
}

static unsigned int parse_ip(const char *s)
{
    unsigned int octs[4];
    int n = 0;
    while (n < 4) {
        if (*s < '0' || *s > '9') return 0;
        unsigned int v = 0;
        while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned int)(*s++ - '0');
        if (v > 255) return 0;
        octs[n++] = v;
        if (n < 4) { if (*s != '.') return 0; s++; }
    }
    if (*s != '\0') return 0;
    return (octs[0] << 24) | (octs[1] << 16) | (octs[2] << 8) | octs[3];
}

static void cmd_net(void)
{
    net_status_t st;
    if (sys_net_status(&st) < 0) { term_puts("net: network not available\n"); return; }
    char buf[16];
    fmt_ip(buf, st.ip);      term_printf("  IP:      %s\n", buf);
    fmt_ip(buf, st.mask);    term_printf("  Mask:    %s\n", buf);
    fmt_ip(buf, st.gateway); term_printf("  Gateway: %s\n", buf);
    fmt_ip(buf, st.dns);     term_printf("  DNS:     %s\n", buf);
    term_printf("  MAC:     %02x:%02x:%02x:%02x:%02x:%02x\n",
                st.mac[0], st.mac[1], st.mac[2],
                st.mac[3], st.mac[4], st.mac[5]);
}

static void cmd_ping(const char *ip_str)
{
    if (!ip_str || ip_str[0] == '\0') { term_puts("usage: ping <ip>\n"); return; }
    unsigned int ip = parse_ip(ip_str);
    if (!ip) { term_printf("ping: invalid address: %s\n", ip_str); return; }
    term_printf("PING %s ...\n", ip_str);
    long rtt = sys_net_ping(ip);
    if (rtt < 0)
        term_printf("ping: %s: request timed out\n", ip_str);
    else
        term_printf("reply from %s: time=%ld ms\n", ip_str, rtt);
}

static void cmd_nslookup(const char *hostname)
{
    if (!hostname || hostname[0] == '\0') { term_puts("usage: nslookup <hostname>\n"); return; }
    term_printf("Resolving %s ...\n", hostname);
    unsigned int ip = sys_net_dns(hostname);
    if (!ip) { term_printf("nslookup: %s: not found\n", hostname); return; }
    char buf[16];
    fmt_ip(buf, ip);
    term_printf("%s -> %s\n", hostname, buf);
}

static void cmd_wget(const char *addr)
{
    if (!addr || addr[0] == '\0') {
        term_puts("usage: wget <ip>:<port><path>\n");
        term_puts("  e.g.: wget 10.0.2.2:80/\n");
        return;
    }

    char ipbuf[64];
    unsigned short port = 80;
    const char *path = "/";

    const char *colon = addr;
    while (*colon && *colon != ':') colon++;

    if (*colon == ':') {
        int iplen = (int)(colon - addr);
        if (iplen >= (int)sizeof(ipbuf)) iplen = (int)sizeof(ipbuf) - 1;
        for (int i = 0; i < iplen; i++) ipbuf[i] = addr[i];
        ipbuf[iplen] = '\0';
        const char *portstr = colon + 1;
        port = (unsigned short)atoi(portstr);
        while (*portstr && *portstr != '/') portstr++;
        if (*portstr == '/') path = portstr;
    } else {
        int iplen = 0;
        while (addr[iplen] && iplen < 63) { ipbuf[iplen] = addr[iplen]; iplen++; }
        ipbuf[iplen] = '\0';
    }

    unsigned int ip = parse_ip(ipbuf);
    if (!ip) {
        ip = sys_net_dns(ipbuf);
        if (!ip) { term_printf("wget: cannot resolve %s\n", ipbuf); return; }
    }

    char ipfmt[16];
    fmt_ip(ipfmt, ip);
    term_printf("Connecting to %s (%s) port %u...\n", ipbuf, ipfmt, (unsigned)port);

    long fd = sys_socket(SOCK_TCP);
    if (fd < 0) { term_puts("wget: socket failed\n"); return; }

    if (sys_connect(fd, ip, port) < 0) {
        term_printf("wget: connect to %s:%u failed\n", ipfmt, (unsigned)port);
        sys_net_close(fd);
        return;
    }

    char req[256];
    int reqlen = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                          path, ipbuf);
    if (sys_net_send(fd, req, (long)reqlen) < 0) {
        term_puts("wget: send failed\n");
        sys_net_close(fd);
        return;
    }

    char resp[512];
    long n = sys_net_recv(fd, resp, (long)sizeof(resp) - 1);
    sys_net_close(fd);

    if (n < 0) { term_puts("wget: recv failed\n"); return; }
    resp[n] = '\0';
    term_puts(resp);
    if (n > 0 && resp[n-1] != '\n') term_putc('\n');
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    term_init();

    /* Register with the WM; take focus immediately */
    g_win_id = sys_wm_register(WX, WY, WIN_W, WIN_H, "AetherTerm");
    sys_wm_focus_set(sys_getpid());

    /* Allocate GPU BO so the compositor can composite the terminal window */
    {
        unsigned bo_bytes = (unsigned)WIN_W * (unsigned)WIN_H * 4u;
        g_term_bo = gpu_alloc(bo_bytes);
        if (g_term_bo != GPU_BO_INVALID) {
            g_bo_ptr = (unsigned int *)gpu_map(g_term_bo);
            if (g_bo_ptr) {
                sys_wm_set_buffer(g_win_id, g_term_bo);
                gfx_set_damage_target((int)g_win_id);
            } else {
                gpu_free(g_term_bo);
                g_term_bo = GPU_BO_INVALID;
            }
        }
    }

    draw_window();

    {
        /*
        char motd[2048];
        long n = sys_initrd_read("motd.txt", motd, (long)sizeof(motd) - 1);
        if (n > 0) {
            motd[n] = '\0';
            term_puts(motd);
        } else {
            term_puts("\n  Welcome to AetherOS Phase 5.1\n");
            term_puts("\n  (c) Azordev.pt All rights reserved.\n");
            term_puts("  AetherTerm  --  type 'help' for commands\n\n");
        }*/
        term_puts("\nWelcome to AetherOS");
        term_puts("\n(c) Azordev.pt All rights reserved.\n");
        term_puts("\ntype 'help' for commands list\n\n");
    }

    char line[LINE_MAX];
    char *argv[ARGV_MAX];

    for (;;) {
        print_prompt();
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

        if      (strcmp(cmd, "help")     == 0) cmd_help();
        else if (strcmp(cmd, "echo")     == 0) cmd_echo(argc, argv);
        else if (strcmp(cmd, "pwd")      == 0) cmd_pwd();
        else if (strcmp(cmd, "cd")       == 0) cmd_cd(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "ls")       == 0) cmd_ls(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "mkdir")    == 0) cmd_mkdir(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "touch")    == 0) cmd_touch(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "rm")       == 0) cmd_rm(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "cat")      == 0) cmd_cat(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "mount")    == 0) cmd_mount();
        else if (strcmp(cmd, "disk")     == 0) cmd_disk();
        else if (strcmp(cmd, "mem")      == 0) cmd_mem();
        else if (strcmp(cmd, "time")     == 0) cmd_time();
        else if (strcmp(cmd, "clear")    == 0) cmd_clear();
        else if (strcmp(cmd, "uname")    == 0) cmd_uname();
        else if (strcmp(cmd, "pid")      == 0) cmd_pid();
        else if (strcmp(cmd, "ps")       == 0) cmd_ps();
        else if (strcmp(cmd, "kill")     == 0) cmd_kill(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "files")    == 0) cmd_files();
        else if (strcmp(cmd, "view")     == 0) cmd_view();
        else if (strcmp(cmd, "spawn")    == 0) cmd_spawn(argc > 1 ? argv[1] : NULL, background);
        else if (strcmp(cmd, "net")      == 0) cmd_net();
        else if (strcmp(cmd, "ping")     == 0) cmd_ping(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "nslookup") == 0) cmd_nslookup(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "wget")     == 0) cmd_wget(argc > 1 ? argv[1] : NULL);
        else if (strcmp(cmd, "exit")  == 0) {
            int code = (argc > 1) ? atoi(argv[1]) : 0;
            term_puts("Goodbye!\n");
            if (g_win_id >= 0) sys_wm_request_close(g_win_id);
            exit(code);
        } else {
            char path[64];
            snprintf(path, sizeof(path), "/%s", cmd);

            if (background) {
                long child = sys_spawn_args(path, (const char *const *)argv, argc);
                if (child < 0)
                    term_printf("aesh: %s: command not found\n", cmd);
            } else {
                /* Pipe-capture child's stdout so it appears in the terminal. */
                int pfds[2];
                if (sys_pipe(pfds) < 0) {
                    /* No pipe available — fall back to direct spawn */
                    long child = sys_spawn_args(path, (const char *const *)argv, argc);
                    if (child < 0)
                        term_printf("aesh: %s: command not found\n", cmd);
                    else { wait_foreground(child); redraw_after_child(); }
                } else {
                    /* Redirect our stdout to pipe write so the child inherits it */
                    sys_dup2(STDOUT_FILENO, 5);            /* save our stdout in slot 5 */
                    sys_dup2(pfds[1], STDOUT_FILENO);      /* our stdout → pipe write   */
                    long child = sys_spawn_args(path, (const char *const *)argv, argc);
                    sys_dup2(5, STDOUT_FILENO);            /* restore our stdout        */
                    sys_close(5);
                    sys_close(pfds[1]);                    /* parent drops write end    */

                    if (child < 0) {
                        sys_close(pfds[0]);
                        term_printf("aesh: %s: command not found\n", cmd);
                    } else {
                        /* Drain child output into the terminal window */
                        char rbuf[256];
                        long n;
                        while ((n = sys_read(pfds[0], rbuf, (long)sizeof(rbuf) - 1)) > 0) {
                            rbuf[n] = '\0';
                            term_puts(rbuf);
                        }
                        sys_close(pfds[0]);
                        /* Child exited (EOF = write end closed); reap it */
                        int status = 0;
                        sys_waitpid(child, &status);
                        redraw_after_child();
                    }
                }
            }
        }
    }

    return 0;
}
