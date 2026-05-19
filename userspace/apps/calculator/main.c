/*
 * AetherOS — Calculator
 *
 * Win95-style four-function calculator using libwidget buttons.
 * Arithmetic uses 64-bit fixed-point (SCALE = 10000, i.e. 4 decimal places)
 * to avoid both floating-point registers (-mgeneral-regs-only restriction)
 * and __int128 division (which needs libgcc's __divti3, unavailable here).
 * Overflow is detected with a pre-check before each multiplication.
 *
 * Layout (5 rows × 4 buttons beneath a display panel):
 *   Row 1: BS   CE   C    ÷
 *   Row 2:  7    8   9    ×
 *   Row 3:  4    5   6    −
 *   Row 4:  1    2   3    +
 *   Row 5: [0 wide]  .   =
 */

#include <stdint.h>
#include <gfx.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>
#include <stdio.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define TOPBAR_H    36
#define ACCENT_H     2

#define WIN_W      244
#define WIN_H      300
#define TITLE_H     28
#define SIDE_PAD     8
#define CONT_PAD     8

#define WIN_X_INIT  260
#define WIN_Y_INIT  (TOPBAR_H + ACCENT_H + 40)

#define CONT_W   (WIN_W - 2 * SIDE_PAD)           /* 228 */
#define CONT_H   (WIN_H - TITLE_H - 2 * CONT_PAD) /* 256 */

/* ── Button grid constants ───────────────────────────────────────────────── */

#define DISP_H    46
#define GAP        4
#define BTN_W     54    /* 4×54 + 3×4 = 228 == CONT_W */
#define BTN_H     36
#define BTN_Y0    (DISP_H + GAP + 2)   /* 52 */
#define ROW_STR   (BTN_H + GAP)        /* 40 */

#define COL0_X    0
#define COL1_X    (BTN_W + GAP)
#define COL2_X    (2 * (BTN_W + GAP))
#define COL3_X    (3 * (BTN_W + GAP))

#define ZERO_W    (BTN_W * 2 + GAP)    /* 112: spans cols 0-1 */

/* ── Color palette ───────────────────────────────────────────────────────── */

#define C_DISP_BG   GFX_RGB( 10,  14,  22)
#define C_DISP_TXT  GFX_RGB(160, 255, 180)   /* phosphor green */
#define C_DISP_BDR  GFX_RGB( 70,  80, 120)

#define C_BTN_NUM_N  GFX_RGB( 40,  42,  68)
#define C_BTN_NUM_H  GFX_RGB( 58,  60,  92)
#define C_BTN_NUM_P  GFX_RGB( 28,  30,  50)
#define C_BTN_NUM_B  GFX_RGB( 65,  68, 108)

#define C_BTN_OP_N   GFX_RGB( 52,  46,  90)
#define C_BTN_OP_H   GFX_RGB( 72,  64, 125)
#define C_BTN_OP_P   GFX_RGB( 36,  32,  65)
#define C_BTN_OP_B   GFX_RGB( 90,  82, 155)

#define C_BTN_CLR_N  GFX_RGB( 85,  30,  30)
#define C_BTN_CLR_H  GFX_RGB(115,  45,  45)
#define C_BTN_CLR_P  GFX_RGB( 60,  20,  20)
#define C_BTN_CLR_B  GFX_RGB(150,  60,  60)

#define C_BTN_EQ_N   GFX_RGB(105,  90, 225)
#define C_BTN_EQ_H   GFX_RGB(140, 125, 255)
#define C_BTN_EQ_P   GFX_RGB( 80,  68, 175)
#define C_BTN_EQ_B   GFX_RGB(165, 152, 255)

#define C_BTN_TXT    GFX_RGB(215, 215, 240)

/* ── Window state ────────────────────────────────────────────────────────── */

static int  g_win_x = WIN_X_INIT;
static int  g_win_y = WIN_Y_INIT;
static long g_win_id = -1;

/* ── Fixed-point arithmetic (SCALE = 10000 → 4 decimal places) ─────────── */
/*
 * All values are stored as (real_value × SCALE).
 * Example: 3.14 → 31400.
 *
 * Max safe operand for mul: sqrt(LLONG_MAX) / SCALE ≈ 303000 whole-number part.
 * Numbers larger than that trigger an overflow error.  Division is always safe
 * provided the intermediate a×SCALE fits in long long (checked explicitly).
 */

typedef long long fixed_t;

#define SCALE       10000LL
#define FMAX_WHOLE  9999999LL           /* max whole-number part we display */
#define FMAX        (FMAX_WHOLE * SCALE) /* max fixed_t we store */
#define LLONG_MAX_V 9223372036854775807LL

static int g_error = 0;   /* 1 = display shows "Error"; only C resets it */

typedef enum { OP_NONE, OP_ADD, OP_SUB, OP_MUL, OP_DIV } calc_op_t;

/* Parse display string "3.14" → 31400 */
static fixed_t str_to_fixed(const char *s)
{
    long long ipart = 0, frac = 0, frac_mul = SCALE / 10;
    int neg = 0, in_frac = 0;

    if (*s == '-') { neg = 1; s++; }
    for (; *s; s++) {
        if (*s == '.') { in_frac = 1; continue; }
        int d = *s - '0';
        if (in_frac) {
            if (frac_mul > 0) { frac += (long long)d * frac_mul; frac_mul /= 10; }
        } else {
            ipart = ipart * 10 + d;
        }
    }
    long long v = ipart * SCALE + frac;
    return neg ? -v : v;
}

/* Format fixed_t 31400 → "3.14" */
static void fixed_to_str(char *out, int outsz, fixed_t v)
{
    int pos = 0;
    int neg = (v < 0);
    if (neg) { v = -v; if (pos < outsz - 1) out[pos++] = '-'; }

    long long ipart = v / SCALE;
    long long frac  = v % SCALE;

    /* Integer digits */
    {
        char tmp[20]; int tlen = 0;
        if (ipart == 0) {
            tmp[tlen++] = '0';
        } else {
            while (ipart > 0 && tlen < 19) {
                tmp[tlen++] = '0' + (int)(ipart % 10);
                ipart /= 10;
            }
            for (int i = 0, j = tlen - 1; i < j; i++, j--) {
                char c = tmp[i]; tmp[i] = tmp[j]; tmp[j] = c;
            }
        }
        for (int i = 0; i < tlen && pos < outsz - 1; i++)
            out[pos++] = tmp[i];
    }

    /* Fractional digits — strip trailing zeros */
    if (frac != 0) {
        if (pos < outsz - 1) out[pos++] = '.';
        char tmp[5]; long long f = frac;
        for (int i = 3; i >= 0; i--) { tmp[i] = '0' + (int)(f % 10); f /= 10; }
        int len = 4;
        while (len > 0 && tmp[len - 1] == '0') len--;
        for (int i = 0; i < len && pos < outsz - 1; i++)
            out[pos++] = tmp[i];
    }

    out[pos] = '\0';
}

static fixed_t safe_mul(fixed_t a, fixed_t b)
{
    long long aa = (a < 0) ? -a : a;
    long long bb = (b < 0) ? -b : b;
    int neg = (a < 0) != (b < 0);

    /* Pre-check: would aa * bb overflow long long? */
    if (aa != 0 && bb > LLONG_MAX_V / aa) { g_error = 1; return 0; }

    long long prod   = aa * bb;
    long long result = prod / SCALE;

    if (result > FMAX) { g_error = 1; return 0; }
    return neg ? -result : result;
}

static fixed_t safe_div(fixed_t a, fixed_t b)
{
    if (b == 0) { g_error = 1; return 0; }

    long long aa = (a < 0) ? -a : a;
    long long bb = (b < 0) ? -b : b;
    int neg = (a < 0) != (b < 0);

    /* Pre-check: would aa * SCALE overflow long long? */
    if (aa > LLONG_MAX_V / SCALE) { g_error = 1; return 0; }

    long long result = (aa * SCALE) / bb;
    if (result > FMAX) { g_error = 1; return 0; }
    return neg ? -result : result;
}

static fixed_t apply_op(fixed_t a, calc_op_t op, fixed_t b)
{
    switch (op) {
    case OP_ADD: return a + b;
    case OP_SUB: return a - b;
    case OP_MUL: return safe_mul(a, b);
    case OP_DIV: return safe_div(a, b);
    default:     return b;
    }
}

/* ── Calculator state ────────────────────────────────────────────────────── */

#define DISP_MAX 20

static char      g_disp[DISP_MAX] = "0";
static fixed_t   g_acc   = 0;
static calc_op_t g_op    = OP_NONE;
static int       g_fresh = 1;   /* next digit replaces display */
static int       g_dot   = 0;   /* decimal point already entered */

/* ── Widget declarations ─────────────────────────────────────────────────── */

static widget_t g_root;
static widget_t g_disp_wgt;
static widget_t g_row[5];
static widget_t g_btn[19];   /* 4+4+4+4+3 */

/* ── Calculator logic ────────────────────────────────────────────────────── */

static void update_display(void)
{
    label_set_text(&g_disp_wgt, g_disp);
}

static void show_error(void)
{
    const char *e = "Error";
    int i;
    for (i = 0; e[i] && i < DISP_MAX - 1; i++) g_disp[i] = e[i];
    g_disp[i] = '\0';
    g_error = 1;
    g_acc = 0; g_op = OP_NONE; g_fresh = 1; g_dot = 0;
    update_display();
}

static void input_digit(char c)
{
    if (g_error) return;

    if (g_fresh) {
        g_disp[0] = c; g_disp[1] = '\0';
        g_fresh = 0;
        update_display();
        return;
    }

    int len = strlen(g_disp);
    if (len >= DISP_MAX - 1) return;

    /* Replace lone "0" with the new digit (unless new digit is also '0') */
    if (len == 1 && g_disp[0] == '0') {
        if (c != '0') { g_disp[0] = c; update_display(); }
        return;
    }

    g_disp[len] = c; g_disp[len + 1] = '\0';
    update_display();
}

static void input_dot(void)
{
    if (g_error || g_dot) return;
    g_dot = 1;

    if (g_fresh) {
        g_disp[0] = '0'; g_disp[1] = '.'; g_disp[2] = '\0';
        g_fresh = 0;
    } else {
        int len = strlen(g_disp);
        if (len < DISP_MAX - 1) { g_disp[len] = '.'; g_disp[len + 1] = '\0'; }
    }
    update_display();
}

static void press_op(calc_op_t op)
{
    if (g_error) return;

    fixed_t cur = str_to_fixed(g_disp);

    if (g_op != OP_NONE && !g_fresh) {
        fixed_t result = apply_op(g_acc, g_op, cur);
        if (g_error) { show_error(); return; }
        g_acc = result;
        fixed_to_str(g_disp, DISP_MAX, result);
        update_display();
    } else {
        g_acc = cur;
    }

    g_op    = op;
    g_fresh = 1;
    g_dot   = 0;
}

static void press_equals(void)
{
    if (g_error || g_op == OP_NONE) return;

    fixed_t cur    = str_to_fixed(g_disp);
    fixed_t result = apply_op(g_acc, g_op, cur);

    if (g_error) { show_error(); return; }

    fixed_to_str(g_disp, DISP_MAX, result);
    update_display();

    g_acc   = 0;
    g_op    = OP_NONE;
    g_fresh = 1;
    g_dot   = 0;
}

static void press_clear(void)
{
    g_disp[0] = '0'; g_disp[1] = '\0';
    g_acc = 0; g_op = OP_NONE; g_fresh = 1; g_dot = 0; g_error = 0;
    update_display();
}

static void press_ce(void)
{
    if (g_error) { press_clear(); return; }
    g_disp[0] = '0'; g_disp[1] = '\0';
    g_fresh = 1; g_dot = 0;
    update_display();
}

static void press_backspace(void)
{
    if (g_error || g_fresh) return;
    int len = strlen(g_disp);
    if (len <= 1) {
        g_disp[0] = '0'; g_disp[1] = '\0'; g_dot = 0;
    } else {
        if (g_disp[len - 1] == '.') g_dot = 0;
        g_disp[len - 1] = '\0';
    }
    update_display();
}

static void handle_action(char a)
{
    if (a >= '0' && a <= '9') { input_digit(a); return; }
    switch (a) {
    case '.': input_dot();           break;
    case '+': press_op(OP_ADD);      break;
    case '-': press_op(OP_SUB);      break;
    case '*': press_op(OP_MUL);      break;
    case '/': press_op(OP_DIV);      break;
    case '=': press_equals();        break;
    case 'c': press_clear();         break;
    case 'e': press_ce();            break;
    case 'b': press_backspace();     break;
    }
}

/* ── Button kind (drives color selection) ────────────────────────────────── */

typedef enum { BK_NUM = 0, BK_OP, BK_CLR, BK_EQ } btn_kind_t;

static btn_kind_t kind_of(char a)
{
    if (a == '=')                                       return BK_EQ;
    if (a == '+' || a == '-' || a == '*' || a == '/')  return BK_OP;
    if (a == 'b' || a == 'c' || a == 'e')              return BK_CLR;
    return BK_NUM;
}

/* ── Custom button draw ──────────────────────────────────────────────────── */

static void calc_btn_draw(widget_t *w, int ax, int ay)
{
    char a = (char)(uintptr_t)w->userdata;
    btn_kind_t k = kind_of(a);
    unsigned bg, bdr;

    switch (k) {
    case BK_OP:
        bdr = C_BTN_OP_B;
        switch (w->state) {
        case WS_HOVERED: bg = C_BTN_OP_H;  break;
        case WS_PRESSED: bg = C_BTN_OP_P;  break;
        default:         bg = C_BTN_OP_N;  break;
        }
        break;
    case BK_CLR:
        bdr = C_BTN_CLR_B;
        switch (w->state) {
        case WS_HOVERED: bg = C_BTN_CLR_H; break;
        case WS_PRESSED: bg = C_BTN_CLR_P; break;
        default:         bg = C_BTN_CLR_N; break;
        }
        break;
    case BK_EQ:
        bdr = C_BTN_EQ_B;
        switch (w->state) {
        case WS_HOVERED: bg = C_BTN_EQ_H;  break;
        case WS_PRESSED: bg = C_BTN_EQ_P;  break;
        default:         bg = C_BTN_EQ_N;  break;
        }
        break;
    default: /* BK_NUM */
        bdr = C_BTN_NUM_B;
        switch (w->state) {
        case WS_HOVERED: bg = C_BTN_NUM_H; break;
        case WS_PRESSED: bg = C_BTN_NUM_P; break;
        default:         bg = C_BTN_NUM_N; break;
        }
        break;
    }

    gfx_fill(ax, ay, w->bounds.w, w->bounds.h, bg);
    gfx_rect(ax, ay, w->bounds.w, w->bounds.h, bdr);
    gfx_text_center(ax, w->bounds.w,
                    ay + (w->bounds.h - WGT_FONT_H) / 2,
                    w->data.button.text, C_BTN_TXT, bg);
}

static int calc_btn_event(widget_t *w, const widget_event_t *ev)
{
    if (ev->type == WEV_MOUSE_DOWN) {
        w->state = WS_PRESSED; w->dirty = 1;
        return 1;
    }
    if (ev->type == WEV_MOUSE_UP) {
        w->state = WS_FOCUSED; w->dirty = 1;
        handle_action((char)(uintptr_t)w->userdata);
        return 1;
    }
    if (ev->type == WEV_KEY_DOWN && ev->keycode == KEY_ENTER) {
        w->state = WS_FOCUSED; w->dirty = 1;
        handle_action((char)(uintptr_t)w->userdata);
        return 1;
    }
    if (ev->type == WEV_FOCUS_IN || ev->type == WEV_FOCUS_OUT) {
        w->state = (ev->type == WEV_FOCUS_IN) ? WS_FOCUSED : WS_NORMAL;
        w->dirty = 1;
    }
    return 0;
}

/* ── Custom display draw ─────────────────────────────────────────────────── */

static void disp_draw(widget_t *w, int ax, int ay)
{
    gfx_fill(ax, ay, w->bounds.w, w->bounds.h, C_DISP_BG);
    gfx_rect(ax, ay, w->bounds.w, w->bounds.h, C_DISP_BDR);

    const char *s = w->data.label.text;
    int len = 0;
    while (s[len]) len++;

    int tx = ax + w->bounds.w - len * WGT_FONT_W - 8;
    if (tx < ax + 4) tx = ax + 4;
    int ty = ay + (w->bounds.h - WGT_FONT_H) / 2;

    gfx_text(tx, ty, s, C_DISP_TXT, C_DISP_BG);
}

/* ── Button factory ──────────────────────────────────────────────────────── */

static void make_btn(widget_t *w, int x, int y, int bw, int bh,
                     const char *lbl, char action)
{
    widget_init(w, WIDGET_BUTTON, x, y, bw, bh);
    w->draw_fn   = calc_btn_draw;
    w->event_fn  = calc_btn_event;
    w->focusable = 1;
    w->userdata  = (void *)(uintptr_t)(unsigned char)action;

    int i = 0;
    while (lbl[i] && i < 127) { w->data.button.text[i] = lbl[i]; i++; }
    w->data.button.text[i] = '\0';
}

/* ── UI build ────────────────────────────────────────────────────────────── */

static void build_ui(void)
{
    widget_init_panel(&g_root, 0, 0, CONT_W, CONT_H, C_WIN_BG);

    widget_init_label(&g_disp_wgt, 0, 0, CONT_W, DISP_H, "0", WGT_ALIGN_RIGHT);
    g_disp_wgt.draw_fn = disp_draw;

    for (int r = 0; r < 5; r++)
        widget_init_panel(&g_row[r], 0, BTN_Y0 + r * ROW_STR, CONT_W, BTN_H, C_WIN_BG);

    /* Row 1: BS  CE  C   /  */
    make_btn(&g_btn[0],  COL0_X, 0, BTN_W, BTN_H, "BS", 'b');
    make_btn(&g_btn[1],  COL1_X, 0, BTN_W, BTN_H, "CE", 'e');
    make_btn(&g_btn[2],  COL2_X, 0, BTN_W, BTN_H, "C",  'c');
    make_btn(&g_btn[3],  COL3_X, 0, BTN_W, BTN_H, "/",  '/');

    /* Row 2: 7  8  9  x */
    make_btn(&g_btn[4],  COL0_X, 0, BTN_W, BTN_H, "7",  '7');
    make_btn(&g_btn[5],  COL1_X, 0, BTN_W, BTN_H, "8",  '8');
    make_btn(&g_btn[6],  COL2_X, 0, BTN_W, BTN_H, "9",  '9');
    make_btn(&g_btn[7],  COL3_X, 0, BTN_W, BTN_H, "x",  '*');

    /* Row 3: 4  5  6  - */
    make_btn(&g_btn[8],  COL0_X, 0, BTN_W, BTN_H, "4",  '4');
    make_btn(&g_btn[9],  COL1_X, 0, BTN_W, BTN_H, "5",  '5');
    make_btn(&g_btn[10], COL2_X, 0, BTN_W, BTN_H, "6",  '6');
    make_btn(&g_btn[11], COL3_X, 0, BTN_W, BTN_H, "-",  '-');

    /* Row 4: 1  2  3  + */
    make_btn(&g_btn[12], COL0_X, 0, BTN_W, BTN_H, "1",  '1');
    make_btn(&g_btn[13], COL1_X, 0, BTN_W, BTN_H, "2",  '2');
    make_btn(&g_btn[14], COL2_X, 0, BTN_W, BTN_H, "3",  '3');
    make_btn(&g_btn[15], COL3_X, 0, BTN_W, BTN_H, "+",  '+');

    /* Row 5: 0 (double-wide)  .  = */
    make_btn(&g_btn[16], COL0_X, 0, ZERO_W, BTN_H, "0", '0');
    make_btn(&g_btn[17], COL2_X, 0, BTN_W,  BTN_H, ".", '.');
    make_btn(&g_btn[18], COL3_X, 0, BTN_W,  BTN_H, "=", '=');

    /* Build tree: root ← display + 5 row panels ← buttons */
    widget_add_child(&g_root, &g_disp_wgt);
    for (int r = 0; r < 5; r++)
        widget_add_child(&g_root, &g_row[r]);

    widget_add_child(&g_row[0], &g_btn[0]);
    widget_add_child(&g_row[0], &g_btn[1]);
    widget_add_child(&g_row[0], &g_btn[2]);
    widget_add_child(&g_row[0], &g_btn[3]);

    widget_add_child(&g_row[1], &g_btn[4]);
    widget_add_child(&g_row[1], &g_btn[5]);
    widget_add_child(&g_row[1], &g_btn[6]);
    widget_add_child(&g_row[1], &g_btn[7]);

    widget_add_child(&g_row[2], &g_btn[8]);
    widget_add_child(&g_row[2], &g_btn[9]);
    widget_add_child(&g_row[2], &g_btn[10]);
    widget_add_child(&g_row[2], &g_btn[11]);

    widget_add_child(&g_row[3], &g_btn[12]);
    widget_add_child(&g_row[3], &g_btn[13]);
    widget_add_child(&g_row[3], &g_btn[14]);
    widget_add_child(&g_row[3], &g_btn[15]);

    widget_add_child(&g_row[4], &g_btn[16]);
    widget_add_child(&g_row[4], &g_btn[17]);
    widget_add_child(&g_row[4], &g_btn[18]);
}

/* ── Window frame ────────────────────────────────────────────────────────── */

static void draw_frame(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H,
                            TITLE_H, "Calculator", 0);
}

static void on_reposition(void *ud)
{
    (void)ud;
    draw_frame();
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "Calculator");

    draw_frame();
    build_ui();

    widget_ctx_t ctx;
    ctx.win_x         = &g_win_x;
    ctx.win_y         = &g_win_y;
    ctx.content_dx    = SIDE_PAD;
    ctx.content_dy    = TITLE_H + CONT_PAD;
    ctx.win_id        = (int)g_win_id;
    ctx.win_w         = WIN_W;
    ctx.win_h         = WIN_H;
    ctx.on_reposition = on_reposition;
    ctx.userdata      = NULL;
    ctx.running       = 1;

    widget_run(&g_root, &ctx);

    sys_wm_request_close(g_win_id);
    return 0;
}
