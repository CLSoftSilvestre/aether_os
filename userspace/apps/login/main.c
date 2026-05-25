/*
 * AetherOS — Login screen
 * File: userspace/apps/login/main.c
 *
 * Spawned by init before the desktop is brought up.  Authenticates the user
 * via SYS_USER_LOGIN and then exits (code 0) so init can launch the desktop.
 *
 * Window: 380 × 310, centred on screen.
 * Widgets (all in window-relative coords, origin = content area top-left):
 *   - Title label   "AetherOS"         row 0
 *   - Subtitle label "Sign in to continue" row 1
 *   - Username label + textinput        row 2
 *   - Password label + textinput        row 3  (password masked)
 *   - Sign In button                    row 4
 *   - Error label (hidden until fail)   row 5
 *
 * Keyboard shortcut: Tab cycles focus; Enter in any field submits.
 */

#include <gfx.h>
#include <gpu.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>
#include <stdlib.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define WIN_W    380
#define WIN_H    310
#define TITLE_H   28
#define ACCENT_H   2

/* Content area inside the title bar */
#define CONT_X    24
#define CONT_W   (WIN_W - 48)   /* 332 */

/* Row layout (y relative to content top, i.e. title_h + accent_h) */
#define ROW_TITLE_Y   10
#define ROW_SUB_Y     36
#define ROW_SEP_Y     62
#define ROW_UN_LBL_Y  72
#define ROW_UN_IN_Y   90
#define ROW_PW_LBL_Y  120
#define ROW_PW_IN_Y   138
#define ROW_BTN_Y     176
#define ROW_ERR_Y     218

#define LABEL_H  18
#define INPUT_H  28
#define BTN_H    34

/* ── State ───────────────────────────────────────────────────────────────── */

static long g_win_id = -1;
static int  g_win_x;
static int  g_win_y;

static widget_t g_root;
static widget_t g_lbl_title;
static widget_t g_lbl_sub;
static widget_t g_lbl_un;
static widget_t g_in_un;
static widget_t g_lbl_pw;
static widget_t g_in_pw;
static widget_t g_btn_login;
static widget_t g_lbl_err;

static int g_fail_count = 0;

/* ── Forward declarations ────────────────────────────────────────────────── */

static void draw_frame(void);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void show_error(const char *msg)
{
    label_set_text(&g_lbl_err, msg);
    g_lbl_err.hidden = 0;
    widget_invalidate(&g_lbl_err);
}

static void hide_error(void)
{
    g_lbl_err.hidden = 1;
    widget_invalidate(&g_lbl_err);
}

/* ── Try to authenticate ─────────────────────────────────────────────────── */

static void try_login(void)
{
    const char *name = textinput_get_text(&g_in_un);
    const char *pw   = textinput_get_text(&g_in_pw);

    if (!name || name[0] == '\0') {
        show_error("Please enter a username.");
        return;
    }

    if (sys_user_login(name, pw) == 0) {
        /* Success — exit; init will proceed to launch the desktop */
        if (g_win_id >= 0) sys_wm_request_close(g_win_id);
        // ctx.running = 0;
        return;
    }

    g_fail_count++;
    textinput_clear(&g_in_pw);

    if (g_fail_count >= 3) {
        show_error("Too many failed attempts. Please wait.");
        /* Brief pause before allowing another try */
        sys_sleep(300);   /* 3 seconds at 100 Hz */
        g_fail_count = 0;
    } else {
        show_error("Invalid username or password.");
    }
}

/* ── Callbacks ───────────────────────────────────────────────────────────── */

static void on_un_submit(widget_t *w)
{
    (void)w;
    widget_set_focused(&g_in_pw);
}

static void on_pw_submit(widget_t *w)
{
    (void)w;
    try_login();
}

static void on_btn_click(widget_t *w)
{
    (void)w;
    try_login();
}

static void on_un_change(widget_t *w)
{
    (void)w;
    hide_error();
}

static void on_pw_change(widget_t *w)
{
    (void)w;
    hide_error();
}

/* ── Window chrome ───────────────────────────────────────────────────────── */

static void draw_frame(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H,
                            TITLE_H, "AetherOS — Sign In", 0);
}

static void on_reposition(void *ud)
{
    (void)ud;
    draw_frame();
}

/* ── Build widget tree ───────────────────────────────────────────────────── */

static void build_ui(void)
{
    widget_init_panel(&g_root, 0, 0, WIN_W,
                      WIN_H - TITLE_H - ACCENT_H, 0x00000000u);

    /* Title */
    widget_init_label(&g_lbl_title, CONT_X, ROW_TITLE_Y, CONT_W, 22,
                      "AetherOS", WGT_ALIGN_CENTER);

    /* Subtitle */
    widget_init_label(&g_lbl_sub, CONT_X, ROW_SUB_Y, CONT_W, LABEL_H,
                      "Sign in to continue", WGT_ALIGN_CENTER);

    /* Username */
    widget_init_label(&g_lbl_un, CONT_X, ROW_UN_LBL_Y, CONT_W, LABEL_H,
                      "Username", WGT_ALIGN_LEFT);
    widget_init_textinput(&g_in_un, CONT_X, ROW_UN_IN_Y, CONT_W, INPUT_H,
                          on_un_change, on_un_submit);

    /* Password */
    widget_init_label(&g_lbl_pw, CONT_X, ROW_PW_LBL_Y, CONT_W, LABEL_H,
                      "Password", WGT_ALIGN_LEFT);
    widget_init_textinput(&g_in_pw, CONT_X, ROW_PW_IN_Y, CONT_W, INPUT_H,
                          on_pw_change, on_pw_submit);
    g_in_pw.data.textinput.password = 1;   /* mask characters */

    /* Sign In button */
    widget_init_button(&g_btn_login, CONT_X + (CONT_W - 140) / 2,
                       ROW_BTN_Y, 140, BTN_H,
                       "Sign In", on_btn_click);

    /* Error label — hidden until needed */
    widget_init_label(&g_lbl_err, CONT_X, ROW_ERR_Y, CONT_W, LABEL_H,
                      "", WGT_ALIGN_CENTER);
    g_lbl_err.hidden = 1;

    widget_add_child(&g_root, &g_lbl_title);
    widget_add_child(&g_root, &g_lbl_sub);
    widget_add_child(&g_root, &g_lbl_un);
    widget_add_child(&g_root, &g_in_un);
    widget_add_child(&g_root, &g_lbl_pw);
    widget_add_child(&g_root, &g_in_pw);
    widget_add_child(&g_root, &g_btn_login);
    widget_add_child(&g_root, &g_lbl_err);

    /* Start with focus on the username field */
    widget_set_focused(&g_in_un);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    long scr_w = (long)gfx_width();
    long scr_h = (long)gfx_height();

    /* Centre the window; leave room for the topbar (36px) above */
    g_win_x = (int)((scr_w - WIN_W) / 2);
    g_win_y = (int)((scr_h - WIN_H) / 2);
    if (g_win_y < 38) g_win_y = 38;

    /* Register window — init spawns us after the compositor is running */
    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "Login");
    if (g_win_id < 0) return 1;

    /* Grab keyboard focus immediately so the user can type without clicking */
    sys_wm_focus_set(sys_getpid());

    /* Draw window chrome */
    draw_frame();

    /* Build UI */
    build_ui();

    widget_ctx_t ctx;
    ctx.win_x         = &g_win_x;
    ctx.win_y         = &g_win_y;
    ctx.content_dx    = 0;
    ctx.content_dy    = TITLE_H + ACCENT_H;
    ctx.win_id        = (int)g_win_id;
    ctx.win_w         = WIN_W;
    ctx.win_h         = WIN_H;
    ctx.on_reposition = on_reposition;
    /* ctx.per_frame_fn  = NULL; */
    ctx.userdata      = NULL;
    ctx.running       = 1;

    widget_run(&g_root, &ctx);

    sys_wm_request_close(g_win_id);
    return 0;
}
