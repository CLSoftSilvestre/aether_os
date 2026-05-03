/*
 * AetherOS — Tic-Tac-Toe
 *
 * Classic 3×3 noughts-and-crosses.
 * Modes: Human vs Human  |  Human vs CPU (minimax, always optimal).
 *
 * Layout (300×376 window):
 *   Mode screen  — title + two mode-selection buttons
 *   Game screen  — status bar, score, 3×3 cell grid, New Game + Back buttons
 *
 * The same widget tree is used throughout; the mode and game panels are
 * toggled via their .hidden flag.  widget_invalidate_all(&g_root) forces a
 * full redraw whenever screens switch.
 */

#include <stdint.h>
#include <gfx.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>
#include <stdio.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define WIN_W      300
#define WIN_H      376
#define TITLE_H     28
#define SIDE_PAD     8
#define CONT_PAD     8

#define WIN_X_INIT  140
#define WIN_Y_INIT   78   /* TOPBAR_H + ACCENT_H + 40 */

#define CONT_W  (WIN_W - 2 * SIDE_PAD)               /* 284 */
#define CONT_H  (WIN_H - TITLE_H - 2 * CONT_PAD)     /* 332 */

/* ── Board geometry (within the game panel) ──────────────────────────────── */

#define CELL_SIZE    80
#define BOARD_SIZE   (3 * CELL_SIZE)                  /* 240 */
#define BOARD_X      ((CONT_W - BOARD_SIZE) / 2)      /* 22 */
#define BOARD_Y      36

#define BTN_ROW_Y    (BOARD_Y + BOARD_SIZE + 14)      /* 290 */
#define HALF_W       ((CONT_W - 12) / 2)              /* 136 */

/* ── Color palette ───────────────────────────────────────────────────────── */

#define C_CELL_BG_N  GFX_RGB( 28,  26,  48)
#define C_CELL_BG_H  GFX_RGB( 44,  40,  72)
#define C_CELL_BDR   GFX_RGB( 65,  58, 110)
#define C_WIN_BDR    GFX_RGB(220, 180,  40)   /* gold winning-cell border  */
#define C_WIN_BG2    GFX_RGB( 40,  36,  16)   /* dim gold tint for winners */

#define C_X_COL      GFX_RGB(235,  87,  87)   /* red  – X player           */
#define C_O_COL      GFX_RGB(  0, 200, 220)   /* cyan – O player           */

#define C_BTN_N      GFX_RGB( 40,  38,  65)
#define C_BTN_H      GFX_RGB( 58,  55,  92)
#define C_BTN_P      GFX_RGB( 28,  26,  50)
#define C_BTN_BDR    GFX_RGB( 75,  70, 120)

#define C_MODE_N     GFX_RGB( 50,  44,  90)
#define C_MODE_H     GFX_RGB( 70,  62, 125)
#define C_MODE_P     GFX_RGB( 35,  30,  65)
#define C_MODE_BDR   GFX_RGB( 90,  80, 155)

#define C_BTN_TXT    GFX_RGB(215, 215, 240)

/* ── Window state ────────────────────────────────────────────────────────── */

static int  g_win_x  = WIN_X_INIT;
static int  g_win_y  = WIN_Y_INIT;
static long g_win_id = -1;

/* ── Game constants ──────────────────────────────────────────────────────── */

#define MODE_NONE  0
#define MODE_HVH   1
#define MODE_HVC   2

#define MARK_EMPTY 0
#define MARK_X     1
#define MARK_O     2

/* The 8 winning lines (rows, columns, diagonals) */
static const int WIN_LINES[8][3] = {
    {0,1,2}, {3,4,5}, {6,7,8},   /* rows      */
    {0,3,6}, {1,4,7}, {2,5,8},   /* columns   */
    {0,4,8}, {2,4,6}             /* diagonals */
};

/* ── Game state ──────────────────────────────────────────────────────────── */

static int g_mode       = MODE_NONE;
static int g_board[9];              /* MARK_EMPTY / MARK_X / MARK_O */
static int g_turn;                  /* whose mark to place next */
static int g_game_over;             /* 0=playing 1=X wins 2=O wins 3=draw */
static int g_score_x;
static int g_score_o;
static int g_score_draws;
static int g_win_cells[3];         /* cell indices of winning triple; -1 if none */

/* ── Widget declarations ─────────────────────────────────────────────────── */

static widget_t g_root;

/* Mode-selection panel */
static widget_t g_mode_panel;
static widget_t g_mode_title;
static widget_t g_mode_hint;
static widget_t g_hvh_btn;
static widget_t g_hvc_btn;

/* Game panel */
static widget_t g_game_panel;
static widget_t g_status_lbl;
static widget_t g_score_lbl;
static widget_t g_board_panel;
static widget_t g_cell[9];
static widget_t g_newgame_btn;
static widget_t g_back_btn;

/* ── Game logic ──────────────────────────────────────────────────────────── */

static int check_winner(void)
{
    for (int i = 0; i < 8; i++) {
        int a = g_board[WIN_LINES[i][0]];
        int b = g_board[WIN_LINES[i][1]];
        int c = g_board[WIN_LINES[i][2]];
        if (a != MARK_EMPTY && a == b && b == c) {
            g_win_cells[0] = WIN_LINES[i][0];
            g_win_cells[1] = WIN_LINES[i][1];
            g_win_cells[2] = WIN_LINES[i][2];
            return a;
        }
    }
    return 0;
}

static int board_full(void)
{
    for (int i = 0; i < 9; i++)
        if (g_board[i] == MARK_EMPTY) return 0;
    return 1;
}

/* ── Minimax (CPU plays O, maximises; human plays X, minimises) ──────────── */

static int minimax_score(int board[9], int is_max)
{
    /* Check terminal */
    for (int i = 0; i < 8; i++) {
        int a = board[WIN_LINES[i][0]];
        int b = board[WIN_LINES[i][1]];
        int c = board[WIN_LINES[i][2]];
        if (a != MARK_EMPTY && a == b && b == c)
            return (a == MARK_O) ? 10 : -10;
    }
    int full = 1;
    for (int j = 0; j < 9; j++) { if (!board[j]) { full = 0; break; } }
    if (full) return 0;

    if (is_max) {
        int best = -100;
        for (int k = 0; k < 9; k++) {
            if (!board[k]) {
                board[k] = MARK_O;
                int s = minimax_score(board, 0);
                board[k] = MARK_EMPTY;
                if (s > best) best = s;
            }
        }
        return best;
    } else {
        int best = 100;
        for (int k = 0; k < 9; k++) {
            if (!board[k]) {
                board[k] = MARK_X;
                int s = minimax_score(board, 1);
                board[k] = MARK_EMPTY;
                if (s < best) best = s;
            }
        }
        return best;
    }
}

static int cpu_pick_move(void)
{
    int tmp[9];
    for (int i = 0; i < 9; i++) tmp[i] = g_board[i];

    int best = -100, best_idx = -1;
    for (int i = 0; i < 9; i++) {
        if (!tmp[i]) {
            tmp[i] = MARK_O;
            int s = minimax_score(tmp, 0);
            tmp[i] = MARK_EMPTY;
            if (s > best) { best = s; best_idx = i; }
        }
    }
    return best_idx;
}

/* ── UI helpers ──────────────────────────────────────────────────────────── */

static void update_status(void)
{
    char buf[48];
    if (g_game_over == 0) {
        const char *p = (g_turn == MARK_X) ? "X" : "O";
        snprintf(buf, sizeof(buf), "Player %s's turn", p);
    } else if (g_game_over == 1) {
        snprintf(buf, sizeof(buf), "Player X wins! ");
    } else if (g_game_over == 2) {
        snprintf(buf, sizeof(buf), "Player O wins! ");
    } else {
        snprintf(buf, sizeof(buf), "Draw!          ");
    }
    label_set_text(&g_status_lbl, buf);
}

static void update_score(void)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "X: %d   O: %d   Draws: %d",
             g_score_x, g_score_o, g_score_draws);
    label_set_text(&g_score_lbl, buf);
}

static void reset_board(void)
{
    for (int i = 0; i < 9; i++) g_board[i] = MARK_EMPTY;
    g_win_cells[0] = g_win_cells[1] = g_win_cells[2] = -1;
    g_turn      = MARK_X;
    g_game_over = 0;
    for (int i = 0; i < 9; i++) {
        g_cell[i].state = WS_NORMAL;
        g_cell[i].dirty = 1;
    }
    update_status();
}

/* Place a mark, check outcome, optionally trigger CPU reply. */
static void place_mark(int idx, int mark)
{
    g_board[idx] = mark;
    g_cell[idx].dirty = 1;

    int winner = check_winner();
    if (winner) {
        g_game_over = winner;
        if (winner == MARK_X) g_score_x++;
        else                   g_score_o++;
        for (int k = 0; k < 3; k++) g_cell[g_win_cells[k]].dirty = 1;
        update_score();
        update_status();
        return;
    }
    if (board_full()) {
        g_game_over = 3;
        g_score_draws++;
        update_score();
        update_status();
        return;
    }

    g_turn = (mark == MARK_X) ? MARK_O : MARK_X;
    update_status();

    /* CPU response in HvC mode */
    if (g_mode == MODE_HVC && g_turn == MARK_O) {
        int ci = cpu_pick_move();
        if (ci >= 0) place_mark(ci, MARK_O);
    }
}

/* ── Cell widget ─────────────────────────────────────────────────────────── */

static void cell_draw(widget_t *w, int ax, int ay)
{
    int idx  = (int)(uintptr_t)w->userdata;
    int mark = g_board[idx];

    int is_win = 0;
    if (g_game_over == 1 || g_game_over == 2) {
        for (int k = 0; k < 3; k++)
            if (g_win_cells[k] == idx) { is_win = 1; break; }
    }

    unsigned bg  = (w->state == WS_HOVERED && mark == MARK_EMPTY && !g_game_over)
                   ? C_CELL_BG_H : C_CELL_BG_N;
    unsigned bdr = is_win ? C_WIN_BDR : C_CELL_BDR;
    if (is_win) bg = C_WIN_BG2;

    gfx_fill(ax, ay, CELL_SIZE, CELL_SIZE, bg);
    gfx_rect(ax, ay, CELL_SIZE, CELL_SIZE, bdr);

    if (mark == MARK_X) {
        /* Two thick diagonal stripes */
        int m   = 18;
        int len = CELL_SIZE - 2 * m;    /* 44 */
        for (int i = 0; i < len; i++) {
            gfx_fill(ax + m + i, ay + m + i,             4, 4, C_X_COL);
            gfx_fill(ax + m + i, ay + m + (len - 1 - i), 4, 4, C_X_COL);
        }
    } else if (mark == MARK_O) {
        /* Thick ring via concentric rects */
        int m   = 14;
        int osz = CELL_SIZE - 2 * m;    /* 52 */
        for (int t = 0; t < 7; t++)
            gfx_rect(ax + m + t, ay + m + t, osz - 2*t, osz - 2*t, C_O_COL);
    }
}

static int cell_event(widget_t *w, const widget_event_t *ev)
{
    int idx = (int)(uintptr_t)w->userdata;

    if (ev->type == WEV_MOUSE_MOVE) {
        if (g_board[idx] == MARK_EMPTY && !g_game_over) {
            /* Reset hover on all other empty cells */
            for (int i = 0; i < 9; i++) {
                if (i != idx && g_cell[i].state == WS_HOVERED) {
                    g_cell[i].state = WS_NORMAL;
                    g_cell[i].dirty = 1;
                }
            }
            if (w->state != WS_HOVERED) { w->state = WS_HOVERED; w->dirty = 1; }
        }
        return 0;
    }

    if (ev->type == WEV_MOUSE_DOWN) {
        if (g_board[idx] != MARK_EMPTY || g_game_over) return 1;
        if (g_mode == MODE_HVC && g_turn == MARK_O)    return 1;

        w->state = WS_NORMAL;
        w->dirty = 1;
        place_mark(idx, g_turn);
        return 1;
    }

    return 0;
}

/* ── Generic button draw / event ─────────────────────────────────────────── */

static void std_btn_draw(widget_t *w, int ax, int ay,
                          unsigned cn, unsigned ch, unsigned cp, unsigned bdr)
{
    unsigned bg;
    switch (w->state) {
    case WS_HOVERED: bg = ch; break;
    case WS_PRESSED: bg = cp; break;
    default:         bg = cn; break;
    }
    gfx_fill(ax, ay, w->bounds.w, w->bounds.h, bg);
    gfx_rect(ax, ay, w->bounds.w, w->bounds.h, bdr);
    gfx_text_center((unsigned)ax, (unsigned)w->bounds.w,
                    (unsigned)(ay + (w->bounds.h - WGT_FONT_H) / 2),
                    w->data.button.text, C_BTN_TXT, bg);
}

static void game_btn_draw(widget_t *w, int ax, int ay)
{
    std_btn_draw(w, ax, ay, C_BTN_N, C_BTN_H, C_BTN_P, C_BTN_BDR);
}

static void mode_btn_draw(widget_t *w, int ax, int ay)
{
    std_btn_draw(w, ax, ay, C_MODE_N, C_MODE_H, C_MODE_P, C_MODE_BDR);
}

static int click_btn_event(widget_t *w, const widget_event_t *ev)
{
    if (ev->type == WEV_MOUSE_DOWN) {
        w->state = WS_PRESSED; w->dirty = 1; return 1;
    }
    if (ev->type == WEV_MOUSE_UP) {
        w->state = WS_HOVERED; w->dirty = 1;
        if (w->data.button.on_click) w->data.button.on_click(w);
        return 1;
    }
    if (ev->type == WEV_FOCUS_IN)  { w->state = WS_FOCUSED;  w->dirty = 1; }
    if (ev->type == WEV_FOCUS_OUT) { w->state = WS_NORMAL;   w->dirty = 1; }
    return 0;
}

/* ── Button callbacks ────────────────────────────────────────────────────── */

static void on_hvh(widget_t *w)
{
    (void)w;
    g_mode = MODE_HVH;
    g_score_x = g_score_o = g_score_draws = 0;
    reset_board();
    update_score();
    g_mode_panel.hidden = 1;
    g_game_panel.hidden = 0;
    widget_invalidate_all(&g_root);
}

static void on_hvc(widget_t *w)
{
    (void)w;
    g_mode = MODE_HVC;
    g_score_x = g_score_o = g_score_draws = 0;
    reset_board();
    update_score();
    g_mode_panel.hidden = 1;
    g_game_panel.hidden = 0;
    widget_invalidate_all(&g_root);
}

static void on_newgame(widget_t *w)
{
    (void)w;
    reset_board();
    widget_invalidate_all(&g_game_panel);
}

static void on_back(widget_t *w)
{
    (void)w;
    g_mode = MODE_NONE;
    g_game_panel.hidden = 1;
    g_mode_panel.hidden = 0;
    widget_invalidate_all(&g_root);
}

/* ── UI build ────────────────────────────────────────────────────────────── */

static void make_click_btn(widget_t *btn, int x, int y, int bw, int bh,
                            const char *lbl,
                            void (*on_click)(widget_t *),
                            widget_draw_fn dfn)
{
    widget_init_button(btn, x, y, bw, bh, lbl, on_click);
    btn->draw_fn  = dfn;
    btn->event_fn = click_btn_event;
    btn->focusable = 1;
}

static void build_ui(void)
{
    widget_init_panel(&g_root, 0, 0, CONT_W, CONT_H, C_WIN_BG);

    /* ── Mode-selection panel ────────────────────────────────────────────── */
    widget_init_panel(&g_mode_panel, 0, 0, CONT_W, CONT_H, C_WIN_BG);

    widget_init_label(&g_mode_title, 0, 70, CONT_W, 16,
                      "Tic-Tac-Toe", WGT_ALIGN_CENTER);
    widget_init_label(&g_mode_hint,  0, 94, CONT_W, 12,
                      "Select game mode:", WGT_ALIGN_CENTER);

    make_click_btn(&g_hvh_btn, 42, 132, 200, 42, "Human vs Human", on_hvh, mode_btn_draw);
    make_click_btn(&g_hvc_btn, 42, 188, 200, 42, "Human vs CPU",   on_hvc, mode_btn_draw);

    widget_add_child(&g_mode_panel, &g_mode_title);
    widget_add_child(&g_mode_panel, &g_mode_hint);
    widget_add_child(&g_mode_panel, &g_hvh_btn);
    widget_add_child(&g_mode_panel, &g_hvc_btn);

    /* ── Game panel (hidden until mode is chosen) ────────────────────────── */
    widget_init_panel(&g_game_panel, 0, 0, CONT_W, CONT_H, C_WIN_BG);
    g_game_panel.hidden = 1;

    widget_init_label(&g_status_lbl, 0,  4, CONT_W, 14,
                      "Player X's turn", WGT_ALIGN_CENTER);
    widget_init_label(&g_score_lbl,  0, 20, CONT_W, 12,
                      "X: 0   O: 0   Draws: 0", WGT_ALIGN_CENTER);

    /* 3×3 board */
    widget_init_panel(&g_board_panel, BOARD_X, BOARD_Y,
                      BOARD_SIZE, BOARD_SIZE, C_WIN_BG);
    for (int i = 0; i < 9; i++) {
        int r = i / 3, c = i % 3;
        widget_init(&g_cell[i], WIDGET_BUTTON,
                    c * CELL_SIZE, r * CELL_SIZE, CELL_SIZE, CELL_SIZE);
        g_cell[i].draw_fn   = cell_draw;
        g_cell[i].event_fn  = cell_event;
        g_cell[i].focusable = 0;
        g_cell[i].userdata  = (void *)(uintptr_t)i;
        widget_add_child(&g_board_panel, &g_cell[i]);
    }

    make_click_btn(&g_newgame_btn, 0,          BTN_ROW_Y, HALF_W, 30,
                   "New Game", on_newgame, game_btn_draw);
    make_click_btn(&g_back_btn,   HALF_W + 12, BTN_ROW_Y, HALF_W, 30,
                   "< Mode",   on_back,    game_btn_draw);

    widget_add_child(&g_game_panel, &g_status_lbl);
    widget_add_child(&g_game_panel, &g_score_lbl);
    widget_add_child(&g_game_panel, &g_board_panel);
    widget_add_child(&g_game_panel, &g_newgame_btn);
    widget_add_child(&g_game_panel, &g_back_btn);

    widget_add_child(&g_root, &g_mode_panel);
    widget_add_child(&g_root, &g_game_panel);
}

/* ── Window frame ────────────────────────────────────────────────────────── */

static void draw_frame(void)
{
    int wx = g_win_x, wy = g_win_y;
    gfx_fill(wx + 4, wy + 4, WIN_W, WIN_H, GFX_RGB(6, 6, 10));
    gfx_fill(wx, wy, WIN_W, WIN_H, C_WIN_BG);
    gfx_fill(wx, wy, WIN_W, TITLE_H, C_TITLEBAR);
    gfx_draw_close_button(wx + 10, wy + 8, 0);
    gfx_text_center((unsigned)wx, WIN_W, (unsigned)(wy + 10),
                    "Tic-Tac-Toe", C_TEXT, C_TITLEBAR);
    gfx_hline((unsigned)wx, (unsigned)(wy + TITLE_H), WIN_W, C_ACCENT);
    gfx_rect((unsigned)wx, (unsigned)wy, WIN_W, WIN_H, C_SEP);
}

static void on_reposition(void *ud) { (void)ud; draw_frame(); }

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "Tic-Tac-Toe");
    draw_frame();

    g_win_cells[0] = g_win_cells[1] = g_win_cells[2] = -1;
    g_score_x = g_score_o = g_score_draws = 0;

    build_ui();

    widget_ctx_t ctx;
    ctx.win_x         = &g_win_x;
    ctx.win_y         = &g_win_y;
    ctx.content_dx    = SIDE_PAD;
    ctx.content_dy    = TITLE_H + CONT_PAD;
    ctx.on_reposition = on_reposition;
    ctx.userdata      = NULL;
    ctx.running       = 1;

    widget_run(&g_root, &ctx);

    sys_wm_unregister(g_win_id);
    return 0;
}
