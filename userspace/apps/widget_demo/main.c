/*
 * AetherOS — Widget Demo (Phase 5.3)
 * File: userspace/apps/widget_demo/main.c
 *
 * Demonstrates the libwidget toolkit:
 *   - Label (static header)
 *   - TextInput (type here)
 *   - Button ("Add" echoes input into listview)
 *   - ListView (scrollable, keyboard-navigable)
 *   - Checkbox ("Show hints")
 *   - Status label (updated by callbacks)
 *
 * Window: 500 × 380 px.  Content area: 484 × 320 px (8 px side pad, 28 title).
 *
 * Success criteria (Phase 5.3):
 *   widget_demo launches from aether_term; button click works; text input
 *   accepts keyboard; list view navigates with keyboard; Tab cycles focus.
 */

#include <gfx.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>
#include <stdio.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define TOPBAR_H   36
#define ACCENT_H    2
#define BOTBAR_Y  744

#define WIN_W    500
#define WIN_H    380
#define TITLE_H   28
#define SIDE_PAD   8
#define CONT_PAD   8

#define WIN_X_INIT  120
#define WIN_Y_INIT  (TOPBAR_H + ACCENT_H + 40)

#define CONT_W  (WIN_W - 2 * SIDE_PAD)                  /* 484 */
#define CONT_H  (WIN_H - TITLE_H - 2 * CONT_PAD)        /* 316 */

static int g_win_x = WIN_X_INIT;
static int g_win_y = WIN_Y_INIT;
static long g_win_id = -1;

/* ── Widget tree (global statics — no heap cost) ─────────────────────────── */

static widget_t g_root;
static widget_t g_header;     /* "Widget Library Demo"       */
static widget_t g_input;      /* text input                  */
static widget_t g_btn_add;    /* "Add to list"               */
static widget_t g_listview;   /* scrollable list             */
static widget_t g_checkbox;   /* "Show hints"                */
static widget_t g_status;     /* status / hint label         */

/* ── Forward declarations ────────────────────────────────────────────────── */

static void draw_frame(void);

/* ── Callback implementations ────────────────────────────────────────────── */

static void on_add_click(widget_t *btn)
{
    (void)btn;
    const char *text = textinput_get_text(&g_input);
    if (!text || !text[0]) {
        label_set_text(&g_status, "  Type something first!");
        return;
    }
    listview_add_item(&g_listview, text, NULL);
    textinput_clear(&g_input);
    label_set_text(&g_status, "  Item added.");
    /* Give focus back to the input */
    widget_set_focused(&g_input);
}

static void on_input_submit(widget_t *inp)
{
    (void)inp;
    on_add_click(NULL);
}

static void on_item_select(widget_t *lv, int idx, void *ud)
{
    (void)lv; (void)ud;
    char buf[64];
    snprintf(buf, sizeof(buf), "  Selected item %d.", idx);
    label_set_text(&g_status, buf);
}

static void on_checkbox_toggle(widget_t *cb, int checked)
{
    (void)cb;
    if (checked)
        label_set_text(&g_status,
                       "  Hint: Tab cycles focus, Enter activates buttons.");
    else
        label_set_text(&g_status, "  Hints hidden.");
}

/* ── Window frame (title bar + shadow) ───────────────────────────────────── */

static void draw_frame(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H,
                            TITLE_H, "Widget Demo", 0);
}

/* ── Reposition callback (called by widget_run on WM_EV_REDRAW) ──────────── */

static void on_reposition(void *ud)
{
    (void)ud;
    draw_frame();
}

/* ── Setup ───────────────────────────────────────────────────────────────── */

static void build_ui(void)
{
    /* Root panel covers entire content area */
    widget_init_panel(&g_root, 0, 0, CONT_W, CONT_H, C_WIN_BG);

    /* Header label */
    widget_init_label(&g_header, 0, 0, CONT_W, 20,
                      "Widget Library Demo  (Tab=focus  Enter=confirm)",
                      WGT_ALIGN_LEFT);

    /* Text input */
    widget_init_textinput(&g_input, 0, 30, 310, 26,
                          NULL,             /* on_change */
                          on_input_submit); /* on_submit */

    /* Add button */
    widget_init_button(&g_btn_add, 318, 28, 166, 30,
                       "Add to list", on_add_click);

    /* List view — up to 64 items */
    widget_init_listview(&g_listview, 0, 70, CONT_W, 180,
                         64, on_item_select);

    /* Pre-populate with a few items */
    listview_add_item(&g_listview, "Alpha — first item",   NULL);
    listview_add_item(&g_listview, "Beta  — second item",  NULL);
    listview_add_item(&g_listview, "Gamma — third item",   NULL);

    /* Checkbox */
    widget_init_checkbox(&g_checkbox, 0, 262, 300, 22,
                         "Show hints (toggle me)", on_checkbox_toggle);

    /* Status label */
    widget_init_label(&g_status, 0, 292, CONT_W, 20,
                      "  Type text, press Enter or click Add.",
                      WGT_ALIGN_LEFT);

    /* Build tree */
    widget_add_child(&g_root, &g_header);
    widget_add_child(&g_root, &g_input);
    widget_add_child(&g_root, &g_btn_add);
    widget_add_child(&g_root, &g_listview);
    widget_add_child(&g_root, &g_checkbox);
    widget_add_child(&g_root, &g_status);

    /* Initial focus on text input */
    widget_set_focused(&g_input);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    /* Register window with WM */
    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H,
                                "Widget Demo");

    /* Draw window chrome */
    draw_frame();

    /* Build widget tree */
    build_ui();

    /* Run widget event loop */
    widget_ctx_t ctx;
    ctx.win_x         = &g_win_x;
    ctx.win_y         = &g_win_y;
    ctx.content_dx    = SIDE_PAD;
    ctx.content_dy    = TITLE_H + CONT_PAD;
    ctx.on_reposition = on_reposition;
    ctx.userdata      = NULL;
    ctx.running       = 1;

    widget_run(&g_root, &ctx);

    /* Unregister on normal exit (shouldn't reach here in practice) */
    sys_wm_unregister(g_win_id);
    return 0;
}
