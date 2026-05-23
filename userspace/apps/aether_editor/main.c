/*
 * AetherOS — AetherEditor  (Phase 5.5)
 * File: userspace/apps/aether_editor/main.c
 *
 * Libwidget-based script editor.
 * Layout:
 *   ┌─────────────────────────────────────────────────────┐
 *   │  ●  AetherEditor — script.as          [Open][Save]  │
 *   ├─────────────────────────────────────────────────────┤
 *   │  [textarea  — source editor]                        │
 *   ├─────────────────────────────────────────────────────┤
 *   │  [Run ▶]  [Clear]                                   │
 *   ├─────────────────────────────────────────────────────┤
 *   │  [output label / scrolling result]                  │
 *   └─────────────────────────────────────────────────────┘
 *
 * Run flow:
 *   1. Write textarea content to /tmp/script.as via SYS_FS_CREATE + SYS_FS_WRITE
 *   2. Create a pipe pair
 *   3. sys_dup2(pipe_write_fd, 1) — child inherits fd[1] = pipe write end
 *   4. sys_spawn_args("/aether_interp", {"/aether_interp", "/tmp/script.as"}, 2)
 *   5. Restore parent fd[1] to UART; close parent's pipe write references
 *   6. sys_waitpid(child, NULL) — block until script finishes
 *   7. Read output from pipe; display in output panel
 *
 * Save/Open flow (5.5.5 integration test):
 *   Save: prompt (textinput dialog), write to /scripts/<name>.as
 *   Open: list /scripts/ directory, load selected file into textarea
 */

#include <gfx.h>
#include <sys.h>
#include <widget.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define TOPBAR_H   36
#define ACCENT_H    2

#define WIN_W    560
#define WIN_H    500
#define TITLE_H   28
#define SIDE_PAD   8
#define CONT_PAD   6

#define WIN_X_INIT  100
#define WIN_Y_INIT  (TOPBAR_H + ACCENT_H + 30)

#define CONT_W  (WIN_W - 2 * SIDE_PAD)                   /* 544 */
#define CONT_H  (WIN_H - TITLE_H - 2 * CONT_PAD)         /* 452 */

/* Content area vertical split */
#define TOOLBAR_H     26   /* [Open] [Save] in title area (overlaid) */
#define EDITOR_H     260   /* source textarea height */
#define RUN_BAR_H     30   /* Run / Clear buttons */
#define OUTPUT_H     (CONT_H - EDITOR_H - RUN_BAR_H - 2) /* ~130 */

static int g_win_x = WIN_X_INIT;
static int g_win_y = WIN_Y_INIT;
static long g_win_id = -1;

/* ── Editor state ────────────────────────────────────────────────────────── */

#define SCRIPT_TMP  "/tmp/script.as"
#define SCRIPTS_DIR "/scripts"
#define OUTPUT_MAX  2048

static char g_output[OUTPUT_MAX];

/* ── Widgets ─────────────────────────────────────────────────────────────── */

static widget_t g_root;
static widget_t g_editor;         /* textarea — source */
static widget_t g_btn_new;        /* toolbar: new */
static widget_t g_btn_open;       /* toolbar: open */
static widget_t g_btn_save;       /* toolbar: save */
static widget_t g_btn_run;        /* toolbar: run */
static widget_t g_btn_clear;      /* toolbar: clear */
static widget_t g_output_label;   /* output panel */

/* toolbar panel */
static widget_t g_run_panel;

static widget_ctx_t g_ctx;

/* ── Forward declarations ────────────────────────────────────────────────── */
static void draw_frame(void);

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void set_output(const char *text)
{
    strncpy(g_output, text, OUTPUT_MAX - 1);
    g_output[OUTPUT_MAX - 1] = '\0';
    label_set_text(&g_output_label, g_output);
}

/* ── Run a script ────────────────────────────────────────────────────────── */

static void run_script(void)
{
    /* 1. Get script from textarea */
    char script[8192];
    textarea_get_text(&g_editor, script, sizeof(script));
    if (!script[0]) {
        set_output("[No script to run]");
        return;
    }

    /* 2. Write to /tmp/script.as */
    long vfd = sys_fs_create(SCRIPT_TMP);
    if (vfd < 0) {
        set_output("[Error: cannot create /tmp/script.as]");
        return;
    }
    long written = sys_fs_write(vfd, script, (long)strlen(script));
    sys_fs_close(vfd);
    if (written < 0) {
        set_output("[Error: write failed]");
        return;
    }

    /* 3. Create pipe */
    int pfds[2];
    if (sys_pipe(pfds) != 0) {
        set_output("[Error: pipe failed]");
        return;
    }
    /* pfds[0] = read end, pfds[1] = write end */

    /* 4. Redirect fd[1] (stdout) to pipe write end */
    sys_dup2(pfds[1], 1);

    /* 5. Spawn aether_interp with argument */
    const char *argv[3];
    argv[0] = "/aether_interp";
    argv[1] = SCRIPT_TMP;
    argv[2] = NULL;
    long child = sys_spawn_args("/aether_interp", argv, 2);

    /* 6. Restore parent's fd[1] to UART (using fd[0] which is still UART) */
    sys_dup2(0, 1);
    /* Close parent's copy of pipe write end (originally at pfds[1]) so that
       when child exits, write_open drops to 0 and our read returns EOF. */
    sys_dup2(0, pfds[1]);

    if (child < 0) {
        sys_dup2(0, pfds[0]);  /* close read end */
        set_output("[Error: could not spawn aether_interp]");
        return;
    }

    /* 7. Wait for child to finish */
    int status = 0;
    sys_waitpid(child, &status);

    /* 8. Read all output from pipe */
    char out_buf[OUTPUT_MAX];
    long n = 0;
    long total = 0;
    while (total < (long)sizeof(out_buf) - 1) {
        n = sys_read(pfds[0], out_buf + total,
                     (long)sizeof(out_buf) - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    out_buf[total] = '\0';

    sys_dup2(0, pfds[0]);  /* close read end */

    if (total == 0)
        set_output("[no output]");
    else
        set_output(out_buf);
}

/* ── Save / Open logic ───────────────────────────────────────────────────── */

static void save_script(const char *path)
{
    if (!path || !path[0]) return;

    char script[8192];
    textarea_get_text(&g_editor, script, sizeof(script));

    long vfd = sys_fs_create(path);
    if (vfd < 0) {
        set_output("[Error: cannot create save file]");
        return;
    }
    long n = sys_fs_write(vfd, script, (long)strlen(script));
    sys_fs_close(vfd);

    if (n < 0) {
        set_output("[Error: write failed]");
    } else {
        char msg[64];
        snprintf(msg, sizeof(msg), "[Saved to %s]", path);
        set_output(msg);
    }
}

static void load_script_from(const char *path)
{
    long vfd = sys_fs_open(path);
    if (vfd < 0) {
        set_output("[Error: cannot open file]");
        return;
    }
    char buf[8192];
    long n = sys_fs_read(vfd, buf, sizeof(buf) - 1);
    sys_fs_close(vfd);
    if (n < 0) {
        set_output("[Error: read failed]");
        return;
    }
    buf[n] = '\0';
    textarea_set_text(&g_editor, buf);
    set_output("[Loaded]");
}

/* ── Button callbacks ────────────────────────────────────────────────────── */

static void on_new_click(widget_t *w)
{
    (void)w;
    textarea_set_text(&g_editor, "");
    set_output("");
}

static void on_run_click(widget_t *w)
{
    (void)w;
    set_output("[Running...]");
    draw_frame();
    run_script();
}

static void on_clear_click(widget_t *w)
{
    (void)w;
    set_output("");
}

static void on_save_click(widget_t *w)
{
    (void)w;
    char path[FILEDLG_PATH_MAX];
    path[0] = '\0';
    if (filedlg_save(SCRIPTS_DIR, "*.as", NULL, path) && path[0])
        save_script(path);
}

static void on_open_click(widget_t *w)
{
    (void)w;
    char path[FILEDLG_PATH_MAX];
    path[0] = '\0';
    if (filedlg_open(SCRIPTS_DIR, "*.as", path) && path[0])
        load_script_from(path);
}

/* ── Window frame ────────────────────────────────────────────────────────── */

static void draw_frame(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H,
                            TITLE_H, "AetherEditor", 0);
}

static void on_reposition(void *ud)
{
    (void)ud;
    draw_frame();
}

/* ── Build UI ────────────────────────────────────────────────────────────── */

static void build_ui(void)
{
    widget_init_panel(&g_root, 0, 0, CONT_W, CONT_H, C_WIN_BG);

    /* Source editor textarea — top section */
    widget_init_textarea(&g_editor, 0, 0, CONT_W, EDITOR_H, 128);

    /* Pre-fill with a hello-world example */
    textarea_set_text(&g_editor,
        "-- AetherScript hello world\n"
        "print(\"Hello from AetherOS \" .. os.version)\n"
        "for i = 1, 5 do\n"
        "    print(i)\n"
        "end\n");

    /* Icon toolbar — 30×30 icon buttons, 2px gaps, grouped by function:
     *   [New] [Open] [Save]  <4px gap>  [Run] [Clear]             */
    int run_y = EDITOR_H + 2;
    widget_init_panel(&g_run_panel, 0, run_y, CONT_W, RUN_BAR_H, C_TITLEBAR);

    widget_init_icon_button(&g_btn_new,   0,  run_y, RUN_BAR_H, RUN_BAR_H,
                            ICON_BTN_NEW,   on_new_click);
    widget_init_icon_button(&g_btn_open,  32, run_y, RUN_BAR_H, RUN_BAR_H,
                            ICON_BTN_OPEN,  on_open_click);
    widget_init_icon_button(&g_btn_save,  64, run_y, RUN_BAR_H, RUN_BAR_H,
                            ICON_BTN_SAVE,  on_save_click);
    /* 4px visual gap at x=96 separates file ops from script ops */
    widget_init_icon_button(&g_btn_run,  100, run_y, RUN_BAR_H, RUN_BAR_H,
                            ICON_BTN_RUN,   on_run_click);
    widget_init_icon_button(&g_btn_clear,132, run_y, RUN_BAR_H, RUN_BAR_H,
                            ICON_BTN_CLEAR, on_clear_click);

    /* Output panel */
    int out_y = run_y + RUN_BAR_H + 2;
    widget_init_label(&g_output_label, 0, out_y, CONT_W, OUTPUT_H,
                      "", WGT_ALIGN_LEFT);

    /* Build widget tree */
    widget_add_child(&g_root, &g_editor);
    widget_add_child(&g_root, &g_run_panel);
    widget_add_child(&g_root, &g_btn_new);
    widget_add_child(&g_root, &g_btn_open);
    widget_add_child(&g_root, &g_btn_save);
    widget_add_child(&g_root, &g_btn_run);
    widget_add_child(&g_root, &g_btn_clear);
    widget_add_child(&g_root, &g_output_label);

    widget_set_focused(&g_editor);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(int argc, const char *const *argv)
{
    gfx_init();

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "AetherEditor");

    draw_frame();
    build_ui();

    /* Phase 5.6: if launched with a path argument, load the file immediately */
    if (argc >= 2 && argv[1] && argv[1][0])
        load_script_from(argv[1]);

    g_ctx.win_x         = &g_win_x;
    g_ctx.win_y         = &g_win_y;
    g_ctx.content_dx    = SIDE_PAD;
    g_ctx.content_dy    = TITLE_H + CONT_PAD;
    g_ctx.win_id        = (int)g_win_id;
    g_ctx.win_w         = WIN_W;
    g_ctx.win_h         = WIN_H;
    g_ctx.on_reposition = on_reposition;
    g_ctx.userdata      = NULL;
    g_ctx.running       = 1;

    widget_run(&g_root, &g_ctx);

    sys_wm_request_close(g_win_id);
    return 0;
}
