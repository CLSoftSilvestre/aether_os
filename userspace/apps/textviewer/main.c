/*
 * AetherOS — TextEdit
 * File: userspace/apps/textviewer/main.c
 *
 * Phase 5.4: Text editor built on libwidget.
 * Replaces the blocking text-pager from Phase 4.4.
 *
 * Features:
 *   - Open files from AetherFS (/path) or initrd (bare name)
 *   - Full editing via the textarea widget (cursor, Backspace, Enter, etc.)
 *   - Save to AetherFS via SYS_FS_WRITE / SYS_FS_CREATE (Phase 5.5)
 *   - Modified indicator [*] in title bar
 *   - Foundation for the Lua IDE (Phase 6)
 *
 * Keyboard shortcuts (textarea focused):
 *   Ctrl+N  new document
 *   Ctrl+O  open file named in the filename field
 *   Ctrl+S  save to file named in the filename field
 *   Ctrl+W  close window
 *   Ctrl+C  copy all to clipboard  (textarea built-in)
 *   Ctrl+V  paste from clipboard   (textarea built-in)
 *   Tab     cycle focus
 *
 * Window: 720 x 520.  Content area: 704 x 484 (8 px side pad, 28 title + 4 pad).
 */

#include <gfx.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define WIN_W       720
#define WIN_H       520
#define TITLE_H      28
#define SIDE_PAD      8
#define CONT_PAD      4

#define WIN_X_INIT   60
#define WIN_Y_INIT   70

#define CONT_W  (WIN_W - 2 * SIDE_PAD)             /* 704 */
#define CONT_H  (WIN_H - TITLE_H - 2 * CONT_PAD)   /* 484 */

static int  g_win_x  = WIN_X_INIT;
static int  g_win_y  = WIN_Y_INIT;
static long g_win_id = -1;

/* ── Toolbar layout ──────────────────────────────────────────────────────── */

#define TOOLBAR_H    30
#define SEP_H         2
#define BTN_H        26

#define BTN_NEW_X     2
#define BTN_NEW_W    50
#define BTN_OPN_X    58
#define BTN_OPN_W    58
#define BTN_SAV_X   122
#define BTN_SAV_W    58
#define LBL_FIL_X   190
#define LBL_FIL_W    36
#define INP_FIL_X   228
#define INP_FIL_W   (CONT_W - INP_FIL_X - 2)       /* 474 */

#define EDITOR_Y     (TOOLBAR_H + SEP_H)                    /* 32  */
#define STATUSBAR_H   18
#define STATUSBAR_Y  (CONT_H - STATUSBAR_H - SEP_H)         /* 464 */
#define EDITOR_H     (STATUSBAR_Y - SEP_H - EDITOR_Y)       /* 430 */

/* ── Editor state ─────────────────────────────────────────────────────────── */

#define FILEBUF_MAX  32768
#define MAX_LINES     1024

static char g_filename[128];   /* current file path; empty = untitled */
static int  g_modified;        /* 1 after any unsaved change */
static char g_filebuf[FILEBUF_MAX];

/* ── Widget instances ─────────────────────────────────────────────────────── */

static widget_t g_root;
static widget_t g_btn_new;
static widget_t g_btn_open;
static widget_t g_btn_save;
static widget_t g_lbl_file;
static widget_t g_inp_file;
static widget_t g_sep_toolbar;
static widget_t g_editor;
static widget_t g_sep_status;
static widget_t g_statusbar;

static widget_ctx_t    g_ctx;
static widget_event_fn g_orig_editor_event;

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void draw_frame(void);
static void draw_title_text(void);
static void do_new(void);
static void do_open(void);
static void do_save(void);

/* ── Status helper ────────────────────────────────────────────────────────── */

static void set_status(const char *msg)
{
    label_set_text(&g_statusbar, msg);
}

/* ── Title bar text only (preserves traffic-light buttons on incremental update) */

static void draw_title_text(void)
{
    int wx = g_win_x, wy = g_win_y;
    char title[160];
    if (g_filename[0])
        snprintf(title, sizeof(title), "TextEdit  --  %s%s",
                 g_filename, g_modified ? " [*]" : "");
    else
        snprintf(title, sizeof(title), "TextEdit  --  untitled%s",
                 g_modified ? " [*]" : "");
    gfx_fill((unsigned)(wx + 60), (unsigned)wy,
             (unsigned)(WIN_W - 70), TITLE_H, C_TITLEBAR);
    gfx_text_center((unsigned)wx, WIN_W, (unsigned)(wy + 10),
                    title, C_TEXT, C_TITLEBAR);
}

/* ── Full window chrome ──────────────────────────────────────────────────── */

static void draw_frame(void)
{
    int wx = g_win_x, wy = g_win_y;
    gfx_fill((unsigned)(wx + 4), (unsigned)(wy + 4), WIN_W, WIN_H, GFX_RGB(6, 6, 10));
    gfx_fill((unsigned)wx, (unsigned)wy, WIN_W, WIN_H, C_WIN_BG);
    gfx_fill((unsigned)wx, (unsigned)wy, WIN_W, TITLE_H, C_TITLEBAR);
    gfx_draw_close_button((unsigned)(wx + 10), (unsigned)(wy + 8), 0);
    // gfx_fill((unsigned)(wx + 26), (unsigned)(wy + 8), 12, 12, C_YELLOW);
    // gfx_fill((unsigned)(wx + 42), (unsigned)(wy + 8), 12, 12, C_GREEN);
    draw_title_text();
    gfx_hline((unsigned)wx, (unsigned)(wy + TITLE_H), WIN_W, C_ACCENT);
    gfx_rect((unsigned)wx, (unsigned)wy, WIN_W, WIN_H, C_SEP);
}

/* ── Reposition callback ─────────────────────────────────────────────────── */

static void on_reposition(void *ud)
{
    (void)ud;
    draw_frame();
}

/* ── File operations ─────────────────────────────────────────────────────── */

static void do_new(void)
{
    textarea_set_text(&g_editor, "");
    g_filename[0] = '\0';
    textinput_set_text(&g_inp_file, "");
    g_modified = 0;
    draw_title_text();
    set_status("New file.");
    widget_set_focused(&g_editor);
}

static void do_open(void)
{
    const char *path = textinput_get_text(&g_inp_file);
    if (!path || !path[0]) {
        set_status("Open: enter a filename in the File field first.");
        return;
    }

    /* Try AetherFS first */
    long vfd = sys_fs_open(path);
    if (vfd >= 0) {
        long n = sys_fs_read(vfd, g_filebuf, FILEBUF_MAX - 1);
        sys_fs_close(vfd);
        if (n >= 0) {
            g_filebuf[n] = '\0';
            textarea_set_text(&g_editor, g_filebuf);
            strncpy(g_filename, path, sizeof(g_filename) - 1);
            g_filename[sizeof(g_filename) - 1] = '\0';
            g_modified = 0;
            draw_title_text();
            set_status("Opened from AetherFS.");
            widget_set_focused(&g_editor);
            return;
        }
    }

    /* Fall back to initrd */
    long n = sys_initrd_read(path, g_filebuf, FILEBUF_MAX - 1);
    if (n >= 0) {
        g_filebuf[n] = '\0';
        textarea_set_text(&g_editor, g_filebuf);
        strncpy(g_filename, path, sizeof(g_filename) - 1);
        g_filename[sizeof(g_filename) - 1] = '\0';
        g_modified = 0;
        draw_title_text();
        set_status("Opened from initrd (read-only source).");
        widget_set_focused(&g_editor);
    } else {
        set_status("Open: file not found in AetherFS or initrd.");
    }
}

static void do_save(void)
{
    const char *path = textinput_get_text(&g_inp_file);
    if (!path || !path[0]) {
        set_status("Save: enter a filename in the File field first.");
        return;
    }

    textarea_get_text(&g_editor, g_filebuf, FILEBUF_MAX);
    int len = 0;
    while (g_filebuf[len]) len++;

    long vfd = sys_fs_create(path);
    if (vfd < 0) {
        set_status("Save: AetherFS write not yet available (Phase 5.5).");
        return;
    }

    long written = sys_fs_write(vfd, g_filebuf, (long)len);
    sys_fs_close(vfd);

    if (written >= 0) {
        strncpy(g_filename, path, sizeof(g_filename) - 1);
        g_filename[sizeof(g_filename) - 1] = '\0';
        g_modified = 0;
        draw_title_text();
        set_status("Saved.");
    } else {
        set_status("Save: write error.");
    }
}

/* ── Button / input callbacks ──────────────────────────────────────────────── */

static void on_new_click(widget_t *w)       { (void)w; do_new();  }
static void on_open_click(widget_t *w)      { (void)w; do_open(); }
static void on_save_click(widget_t *w)      { (void)w; do_save(); }
static void on_filename_submit(widget_t *w) { (void)w; do_open(); }

/* ── Textarea event wrapper ─────────────────────────────────────────────────
 * Intercepts Ctrl+N/O/S/W before the textarea sees them.
 * Marks g_modified on the first content-changing keystroke.
 * ─────────────────────────────────────────────────────────────────────────── */

static int is_edit_key(keycode_t kc)
{
    switch (kc) {
    case KEY_NONE:
    case KEY_UP:    case KEY_DOWN:   case KEY_LEFT:  case KEY_RIGHT:
    case KEY_HOME:  case KEY_END:    case KEY_PGUP:  case KEY_PGDN:
    case KEY_TAB:   case KEY_ESC:
    case KEY_LSHIFT: case KEY_RSHIFT:
    case KEY_LCTRL:  case KEY_RCTRL:
    case KEY_LALT:   case KEY_RALT:
    case KEY_CAPS_LOCK:
    case KEY_F1: case KEY_F2: case KEY_F3:  case KEY_F4:
    case KEY_F5: case KEY_F6: case KEY_F7:  case KEY_F8:
    case KEY_F9: case KEY_F10: case KEY_F11: case KEY_F12:
        return 0;
    default:
        return 1;
    }
}

static int editor_event_wrap(widget_t *w, const widget_event_t *ev)
{
    if (ev->type != WEV_KEY_DOWN)
        return g_orig_editor_event(w, ev);

    keycode_t    kc   = ev->keycode;
    unsigned int mods = ev->modifiers;

    if (mods & MOD_CTRL) {
        switch (kc) {
        case KEY_N: do_new();          return 1;
        case KEY_O: do_open();         return 1;
        case KEY_S: do_save();         return 1;
        case KEY_W: g_ctx.running = 0; return 1;
        default: break;
        }
    }

    int ret = g_orig_editor_event(w, ev);

    if (!g_modified) {
        int editing = ((mods & MOD_CTRL) && kc == KEY_V) ||
                      (!(mods & MOD_CTRL) && is_edit_key(kc));
        if (editing) {
            g_modified = 1;
            draw_title_text();
        }
    }

    return ret;
}

/* ── Build widget tree ──────────────────────────────────────────────────────── */

static void build_ui(void)
{
    widget_init_panel(&g_root, 0, 0, CONT_W, CONT_H, C_WIN_BG);

    widget_init_button(&g_btn_new,  BTN_NEW_X, 2, BTN_NEW_W, BTN_H,
                       "New",  on_new_click);
    widget_init_button(&g_btn_open, BTN_OPN_X, 2, BTN_OPN_W, BTN_H,
                       "Open", on_open_click);
    widget_init_button(&g_btn_save, BTN_SAV_X, 2, BTN_SAV_W, BTN_H,
                       "Save", on_save_click);

    widget_init_label(&g_lbl_file, LBL_FIL_X, 8, LBL_FIL_W, 16,
                      "File:", WGT_ALIGN_LEFT);
    widget_init_textinput(&g_inp_file, INP_FIL_X, 2, INP_FIL_W, BTN_H,
                          NULL, on_filename_submit);

    widget_init_panel(&g_sep_toolbar, 0, TOOLBAR_H, CONT_W, SEP_H, C_SEP);

    widget_init_textarea(&g_editor, 0, EDITOR_Y, CONT_W, EDITOR_H, MAX_LINES);

    widget_init_panel(&g_sep_status, 0, EDITOR_Y + EDITOR_H, CONT_W, SEP_H, C_SEP);

    widget_init_label(&g_statusbar, 2, STATUSBAR_Y, CONT_W - 4, STATUSBAR_H,
                      "  Ctrl+N: new   Ctrl+O: open   Ctrl+S: save   Ctrl+W: close",
                      WGT_ALIGN_LEFT);

    widget_add_child(&g_root, &g_btn_new);
    widget_add_child(&g_root, &g_btn_open);
    widget_add_child(&g_root, &g_btn_save);
    widget_add_child(&g_root, &g_lbl_file);
    widget_add_child(&g_root, &g_inp_file);
    widget_add_child(&g_root, &g_sep_toolbar);
    widget_add_child(&g_root, &g_editor);
    widget_add_child(&g_root, &g_sep_status);
    widget_add_child(&g_root, &g_statusbar);

    g_orig_editor_event = g_editor.event_fn;
    g_editor.event_fn   = editor_event_wrap;

    widget_set_focused(&g_editor);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "TextEdit");
    sys_wm_focus_set(sys_getpid());

    draw_frame();
    build_ui();

    g_ctx.win_x         = &g_win_x;
    g_ctx.win_y         = &g_win_y;
    g_ctx.content_dx    = SIDE_PAD;
    g_ctx.content_dy    = TITLE_H + CONT_PAD;
    g_ctx.on_reposition = on_reposition;
    g_ctx.userdata      = NULL;
    g_ctx.running       = 1;

    widget_run(&g_root, &g_ctx);

    sys_wm_unregister(g_win_id);
    return 0;
}
