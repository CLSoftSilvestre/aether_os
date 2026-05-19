/*
 * AetherOS — AetherTop (Task Manager)
 * File: userspace/apps/aether_top/main.c
 *
 * htop-style interactive process monitor.
 * Shows per-process CPU% and memory usage with libwidget progress bars.
 *
 * Flicker-free rendering strategy:
 *   - draw_static_chrome()  — window frame + chrome.  Only on open/move.
 *   - draw_static_fixed()   — col headers + footer.   Only on open/move.
 *   - draw_header()         — stats section.           Every refresh.
 *   - draw_rows(force)      — process rows with dirty cache.  Every refresh.
 *     Rows whose content has not changed since last draw are skipped entirely.
 *   - sys_vsync_wait() is called before any multi-row repaint so updates land
 *     on a frame boundary and the user never sees a partial clear state.
 *
 * Controls:  Up/Down=navigate  K=kill  R=refresh  Q/Esc=quit
 */

#include <gfx.h>
#include <gpu.h>
#include <sys.h>
#include <input.h>
#include <widget.h>
#include <string.h>
#include <stdio.h>

/* ── Window geometry ─────────────────────────────────────────────────────── */

#define WIN_W      680
#define WIN_H      520
#define TITLE_H     32
#define SIDE_PAD     6

#define WIN_X_INIT  60
#define WIN_Y_INIT  50

#define CONT_W  (WIN_W - 2 * SIDE_PAD)   /* 668 */

/* ── Section heights (relative to content y-origin cy = win_y + TITLE_H) ── */

#define HDR_H       68   /* system stats: title/time + mem bar + cpu bar */
#define COLHDR_H    20   /* column header row                              */
#define ROW_H       26   /* height of each process row                     */
#define MAX_VISIBLE 14   /* max visible rows (14 × 26 = 364 px)            */
#define FOOTER_H    22   /* hint bar at bottom                              */

/* ── Column x-offsets (relative to cx = win_x + SIDE_PAD) ──────────────── */

#define COL_PID_X       0
#define COL_PPID_X     36
#define COL_NAME_X     72
#define COL_STATE_X   172
#define COL_BAR_CPU_X 232
#define BAR_CPU_W     110
#define COL_CPU_PCT_X 348
#define COL_BAR_MEM_X 392
#define BAR_MEM_W     110
#define COL_MEM_KB_X  508

/* ── Header bar geometry (relative to cx) ───────────────────────────────── */

#define HDR_BAR_X   40
#define HDR_BAR_W  200
#define HDR_BAR_H   14
#define HDR_VAL_X  246

/* ── Refresh cadence ─────────────────────────────────────────────────────── */

#define REFRESH_TICKS  100   /* 1 second at 100 Hz */

/* ── Colors ──────────────────────────────────────────────────────────────── */

#define C_HDR_BG      GFX_RGB( 22,  22,  36)
#define C_COL_HDR_BG  GFX_RGB( 34,  34,  54)
#define C_ROW_SEL     GFX_RGB( 46,  52,  88)
#define C_ROW_ODD     GFX_RGB( 24,  24,  36)
#define C_STATE_RUN   GFX_RGB( 80, 200,  75)
#define C_STATE_RDY   GFX_RGB(220, 190,  60)
#define C_STATE_SLP   GFX_RGB( 80, 140, 230)
#define C_STATE_WAIT  GFX_RGB(200, 130,  60)
#define C_STATE_DEAD  GFX_RGB(180,  70,  70)
#define C_BAR_MEM_FG  GFX_RGB(  0, 200, 200)
#define C_FOOTER_BG   GFX_RGB( 20,  20,  32)

/* ── Process data ────────────────────────────────────────────────────────── */

#define MAX_PROCS 32

static ps_entry_t g_prev[MAX_PROCS];
static ps_entry_t g_curr[MAX_PROCS];
static int        g_prev_cnt = 0;
static int        g_curr_cnt = 0;
static int        g_first    = 1;

static int g_cpu_pct[MAX_PROCS];
static int g_mem_pct[MAX_PROCS];
static int g_sys_cpu_pct = 0;

static unsigned long g_free_pages  = 0;
static unsigned long g_total_pages = 0;

/* ── UI state ────────────────────────────────────────────────────────────── */

static int  g_win_x   = WIN_X_INIT;
static int  g_win_y   = WIN_Y_INIT;
static long g_win_id  = -1;
static int  g_running = 1;
static int  g_sel     = 0;
static long g_last_refresh = 0;

/* GPU BO for compositor-driven rendering */
static gpu_bo_t  g_bo     = GPU_BO_INVALID;
static unsigned *g_bo_ptr = (unsigned *)0;

static void top_begin_frame(void)
{
    if (g_bo_ptr)
        gfx_begin_frame(g_bo_ptr, WIN_W, WIN_H, g_win_x, g_win_y);
}
static void top_end_frame(void) { if (g_bo_ptr) gfx_end_frame(); }

/* ── Row dirty cache ─────────────────────────────────────────────────────── */

typedef struct {
    unsigned int pid;
    int          state;
    int          cpu_pct;
    int          mem_pct;
    unsigned int mem_pages;
    int          selected;   /* 1 if this row was drawn as selected */
    int          valid;      /* 0 = must redraw unconditionally     */
} row_cache_t;

static row_cache_t g_cache[MAX_VISIBLE];
static int         g_cache_nrows = 0;   /* rows drawn in last draw_rows() */

static void invalidate_rows(void)
{
    for (int i = 0; i < MAX_VISIBLE; i++) g_cache[i].valid = 0;
    g_cache_nrows = 0;
}

/* ── Progress bar widgets ────────────────────────────────────────────────── */

static widget_t g_bar_mem_hdr;
static widget_t g_bar_cpu_hdr;
static widget_t g_row_cpu[MAX_VISIBLE];
static widget_t g_row_mem[MAX_VISIBLE];

/* Memory bars use cyan instead of the default purple accent */
static void mem_bar_draw(widget_t *w, int ax, int ay)
{
    int bw = w->bounds.w, bh = w->bounds.h;
    gfx_fill((unsigned)ax, (unsigned)ay, (unsigned)bw, (unsigned)bh, C_WIN_BG);
    gfx_rect((unsigned)ax, (unsigned)ay, (unsigned)bw, (unsigned)bh, C_SEP);
    int iw = bw - 4, ih = bh - 4;
    int fw = (w->data.progress.value * iw) / 100;
    if (fw > iw) fw = iw;
    if (fw > 0)
        gfx_fill((unsigned)(ax + 2), (unsigned)(ay + 2),
                 (unsigned)fw, (unsigned)ih, C_BAR_MEM_FG);
}

/* ── Epoch → calendar date ───────────────────────────────────────────────── */

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
        int diy  = leap ? 366 : 365;
        if (days < diy) break;
        days -= diy; y++;
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

/* ── Data sampling ───────────────────────────────────────────────────────── */

static unsigned long long sum_ticks(const ps_entry_t *p, int cnt)
{
    unsigned long long t = 0;
    for (int i = 0; i < cnt; i++) t += p[i].cpu_ticks;
    return t;
}

static unsigned long long ticks_for_pid(const ps_entry_t *p, int cnt,
                                         unsigned int pid)
{
    for (int i = 0; i < cnt; i++)
        if (p[i].pid == pid) return p[i].cpu_ticks;
    return 0;
}

static void do_refresh(void)
{
    g_prev_cnt = g_curr_cnt;
    for (int i = 0; i < g_prev_cnt; i++) g_prev[i] = g_curr[i];

    g_curr_cnt = (int)sys_ps(g_curr, MAX_PROCS);
    if (g_curr_cnt < 0) g_curr_cnt = 0;

    long ms       = sys_pmm_stats();
    g_free_pages  = (unsigned long)((unsigned long long)ms >> 32);
    g_total_pages = (unsigned long)((unsigned long long)ms & 0xFFFFFFFFUL);

    if (g_first) {
        g_first = 0;
        for (int i = 0; i < g_curr_cnt; i++) g_cpu_pct[i] = 0;
        g_sys_cpu_pct = 0;
    } else {
        unsigned long long total_d =
            sum_ticks(g_curr, g_curr_cnt) - sum_ticks(g_prev, g_prev_cnt);
        unsigned long long idle_d = 0;

        for (int i = 0; i < g_curr_cnt; i++) {
            unsigned long long pv =
                ticks_for_pid(g_prev, g_prev_cnt, g_curr[i].pid);
            unsigned long long d = (g_curr[i].cpu_ticks > pv)
                                   ? g_curr[i].cpu_ticks - pv : 0;
            g_cpu_pct[i] = (total_d > 0) ? (int)((d * 100) / total_d) : 0;
            if (g_curr[i].pid == 0) idle_d = d;
        }
        g_sys_cpu_pct = (total_d > 0)
            ? (int)(((total_d - idle_d) * 100) / total_d) : 0;
    }

    for (int i = 0; i < g_curr_cnt; i++) {
        unsigned long pk = (unsigned long)g_curr[i].mem_pages * 4;
        unsigned long tk = g_total_pages * 4;
        g_mem_pct[i] = (tk > 0) ? (int)((pk * 100) / tk) : 0;
        if (g_mem_pct[i] > 100) g_mem_pct[i] = 100;
    }

    if (g_sel >= g_curr_cnt) g_sel = g_curr_cnt > 0 ? g_curr_cnt - 1 : 0;
}

/* ── Drawing: static elements (only on open or window move) ─────────────── */

static void draw_static_chrome(void)
{
    gfx_glass_window_frame(g_win_x, g_win_y, WIN_W, WIN_H, TITLE_H,
                            "AetherTop", 0);
}

static void draw_static_fixed(void)
{
    int cx      = g_win_x + SIDE_PAD;
    int cy      = g_win_y + TITLE_H;
    int chy     = cy + HDR_H;
    int row_base = chy + COLHDR_H;

    /* Clear process-row area and footer once so they don't need per-row clears
     * when a row is redrawn — the background is already the right colour. */
    gfx_fill((unsigned)cx, (unsigned)row_base,
              (unsigned)CONT_W, (unsigned)(MAX_VISIBLE * ROW_H), C_WIN_BG);

    /* Column headers */
    gfx_fill((unsigned)cx, (unsigned)chy,
              (unsigned)CONT_W, (unsigned)COLHDR_H, C_COL_HDR_BG);
    gfx_text((unsigned)(cx + COL_PID_X),     (unsigned)(chy + 2), "PID",    C_TEXT_DIM, C_COL_HDR_BG);
    gfx_text((unsigned)(cx + COL_PPID_X),    (unsigned)(chy + 2), "PPID",   C_TEXT_DIM, C_COL_HDR_BG);
    gfx_text((unsigned)(cx + COL_NAME_X),    (unsigned)(chy + 2), "NAME",   C_TEXT_DIM, C_COL_HDR_BG);
    gfx_text((unsigned)(cx + COL_STATE_X),   (unsigned)(chy + 2), "STATE",  C_TEXT_DIM, C_COL_HDR_BG);
    gfx_text((unsigned)(cx + COL_BAR_CPU_X), (unsigned)(chy + 2), "CPU%",   C_TEXT_DIM, C_COL_HDR_BG);
    gfx_text((unsigned)(cx + COL_BAR_MEM_X), (unsigned)(chy + 2), "MEMORY", C_TEXT_DIM, C_COL_HDR_BG);
    gfx_hline((unsigned)cx, (unsigned)(chy + COLHDR_H - 1),
               (unsigned)CONT_W, C_SEP);

    /* Footer */
    int fy = row_base + MAX_VISIBLE * ROW_H;
    gfx_fill((unsigned)cx, (unsigned)fy,
              (unsigned)CONT_W, (unsigned)FOOTER_H, C_FOOTER_BG);
    gfx_hline((unsigned)cx, (unsigned)fy, (unsigned)CONT_W, C_SEP);
    gfx_text((unsigned)(cx + 8), (unsigned)(fy + 3),
              "Up/Dn=navigate   K=kill process   R=refresh   Q=quit",
              C_TEXT_DIM, C_FOOTER_BG);
}

/* ── Drawing: dynamic header (called every refresh) ─────────────────────── */

static void draw_header(void)
{
    int cx = g_win_x + SIDE_PAD;
    int cy = g_win_y + TITLE_H;

    gfx_fill((unsigned)cx, (unsigned)cy,
              (unsigned)CONT_W, (unsigned)HDR_H, C_HDR_BG);

    unsigned long epoch = sys_rtc_get();
    int yr, mo, dy, hr, mn, sc;
    epoch_to_datetime(epoch, &yr, &mo, &dy, &hr, &mn, &sc);
    char clockbuf[32];
    snprintf(clockbuf, sizeof(clockbuf), "%04d-%02d-%02d  %02d:%02d:%02d UTC",
             yr, mo, dy, hr, mn, sc);
    gfx_text((unsigned)cx, (unsigned)(cy + 6), "AetherTop", C_ACCENT, C_HDR_BG);
    gfx_text((unsigned)(cx + CONT_W - 176), (unsigned)(cy + 6),
              clockbuf, C_TEXT, C_HDR_BG);

    unsigned long used_p  = g_total_pages - g_free_pages;
    unsigned long used_mb = used_p * 4 / 1024;
    unsigned long tot_mb  = g_total_pages * 4 / 1024;
    int mem_pct = (g_total_pages > 0) ? (int)(used_p * 100 / g_total_pages) : 0;
    gfx_text((unsigned)cx, (unsigned)(cy + 28), "MEM", C_TEXT_DIM, C_HDR_BG);
    progress_set_value(&g_bar_mem_hdr, mem_pct);
    g_bar_mem_hdr.draw_fn(&g_bar_mem_hdr, cx + HDR_BAR_X, cy + 28);
    gfx_printf((unsigned)(cx + HDR_VAL_X), (unsigned)(cy + 28),
                C_TEXT, C_HDR_BG, "%lu/%lu MB  %d%%", used_mb, tot_mb, mem_pct);

    gfx_text((unsigned)cx, (unsigned)(cy + 50), "CPU", C_TEXT_DIM, C_HDR_BG);
    progress_set_value(&g_bar_cpu_hdr, g_sys_cpu_pct);
    g_bar_cpu_hdr.draw_fn(&g_bar_cpu_hdr, cx + HDR_BAR_X, cy + 50);
    gfx_printf((unsigned)(cx + HDR_VAL_X), (unsigned)(cy + 50),
                C_TEXT, C_HDR_BG, "%d%%", g_sys_cpu_pct);

    gfx_hline((unsigned)cx, (unsigned)(cy + HDR_H - 1),
               (unsigned)CONT_W, C_SEP);
}

/* ── Drawing: single process row ─────────────────────────────────────────── */

static void draw_one_row(int i, int cx, int row_base)
{
    static const char *state_str[] = {
        "unused", "ready", "running", "sleeping", "dead", "zombie", "waiting"
    };
    static const unsigned state_col[] = {
        0, C_STATE_RDY, C_STATE_RUN, C_STATE_SLP,
        C_STATE_DEAD,   C_STATE_DEAD, C_STATE_WAIT
    };

    int ry  = row_base + i * ROW_H;
    unsigned rbg = (i == g_sel) ? C_ROW_SEL
                  : (i & 1)     ? C_ROW_ODD
                  :               C_WIN_BG;

    gfx_fill((unsigned)cx, (unsigned)ry, (unsigned)CONT_W, (unsigned)ROW_H, rbg);

    const ps_entry_t *p = &g_curr[i];
    int st = (p->state >= 0 && p->state <= 6) ? p->state : 0;
    unsigned stcol = state_col[st] ? state_col[st] : C_TEXT_DIM;

    gfx_printf((unsigned)(cx + COL_PID_X),  (unsigned)(ry + 5), C_TEXT,     rbg, "%u", p->pid);
    gfx_printf((unsigned)(cx + COL_PPID_X), (unsigned)(ry + 5), C_TEXT_DIM, rbg, "%u", p->ppid);

    char nb[14]; int nl = 0;
    while (nl < 12 && p->name[nl]) { nb[nl] = p->name[nl]; nl++; }
    nb[nl] = '\0';
    gfx_text((unsigned)(cx + COL_NAME_X),  (unsigned)(ry + 5), nb,            C_TEXT, rbg);
    gfx_text((unsigned)(cx + COL_STATE_X), (unsigned)(ry + 5), state_str[st], stcol,  rbg);

    progress_set_value(&g_row_cpu[i], g_cpu_pct[i]);
    g_row_cpu[i].draw_fn(&g_row_cpu[i], cx + COL_BAR_CPU_X, ry + 8);
    gfx_printf((unsigned)(cx + COL_CPU_PCT_X), (unsigned)(ry + 5),
                C_TEXT, rbg, "%2d%%", g_cpu_pct[i]);

    progress_set_value(&g_row_mem[i], g_mem_pct[i]);
    g_row_mem[i].draw_fn(&g_row_mem[i], cx + COL_BAR_MEM_X, ry + 8);
    unsigned long proc_kb = (unsigned long)p->mem_pages * 4;
    if (proc_kb >= 1024)
        gfx_printf((unsigned)(cx + COL_MEM_KB_X), (unsigned)(ry + 5),
                    C_TEXT, rbg, "%luM", proc_kb / 1024);
    else if (proc_kb > 0)
        gfx_printf((unsigned)(cx + COL_MEM_KB_X), (unsigned)(ry + 5),
                    C_TEXT, rbg, "%luK", proc_kb);
    else
        gfx_text((unsigned)(cx + COL_MEM_KB_X), (unsigned)(ry + 5),
                  "-", C_TEXT_DIM, rbg);

    /* Update cache so this row is not redrawn until something changes */
    row_cache_t *c = &g_cache[i];
    c->pid      = p->pid;
    c->state    = p->state;
    c->cpu_pct  = g_cpu_pct[i];
    c->mem_pct  = g_mem_pct[i];
    c->mem_pages = p->mem_pages;
    c->selected = (i == g_sel);
    c->valid    = 1;
}

/* ── Drawing: all rows with dirty check ─────────────────────────────────── */

static void draw_rows(int force)
{
    int cx       = g_win_x + SIDE_PAD;
    int row_base = g_win_y + TITLE_H + HDR_H + COLHDR_H;
    int nrows    = (g_curr_cnt < MAX_VISIBLE) ? g_curr_cnt : MAX_VISIBLE;

    for (int i = 0; i < nrows; i++) {
        const row_cache_t *c = &g_cache[i];
        const ps_entry_t  *p = &g_curr[i];

        if (!force && c->valid
            && c->pid      == p->pid
            && c->state    == p->state
            && c->cpu_pct  == g_cpu_pct[i]
            && c->mem_pct  == g_mem_pct[i]
            && c->mem_pages == p->mem_pages
            && c->selected == (i == g_sel))
            continue;

        draw_one_row(i, cx, row_base);
    }

    /* Wipe rows that vanished since last render (process exited) */
    for (int i = nrows; i < g_cache_nrows; i++) {
        int ry = row_base + i * ROW_H;
        gfx_fill((unsigned)cx, (unsigned)ry, (unsigned)CONT_W, (unsigned)ROW_H, C_WIN_BG);
        g_cache[i].valid = 0;
    }
    g_cache_nrows = nrows;
}

/* ── Widget initialisation ───────────────────────────────────────────────── */

static void init_widgets(void)
{
    widget_init_progress(&g_bar_cpu_hdr, 0, 0, HDR_BAR_W, HDR_BAR_H,
                          WGT_PROGRESS_DETERMINATE);
    widget_init_progress(&g_bar_mem_hdr, 0, 0, HDR_BAR_W, HDR_BAR_H,
                          WGT_PROGRESS_DETERMINATE);
    g_bar_mem_hdr.draw_fn = mem_bar_draw;

    for (int i = 0; i < MAX_VISIBLE; i++) {
        widget_init_progress(&g_row_cpu[i], 0, 0, BAR_CPU_W, 10,
                              WGT_PROGRESS_DETERMINATE);
        widget_init_progress(&g_row_mem[i], 0, 0, BAR_MEM_W, 10,
                              WGT_PROGRESS_DETERMINATE);
        g_row_mem[i].draw_fn = mem_bar_draw;
    }
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    gfx_init();
    init_widgets();

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "AetherTop");

    if (g_win_id >= 0) {
        g_bo = gpu_alloc(WIN_W * WIN_H * 4u);
        if (g_bo != GPU_BO_INVALID) {
            g_bo_ptr = (unsigned *)gpu_map(g_bo);
            if (g_bo_ptr) {
                sys_wm_set_buffer(g_win_id, g_bo);
                gfx_set_damage_target((int)g_win_id);
            } else {
                gpu_free(g_bo);
                g_bo = GPU_BO_INVALID;
            }
        }
    }

    do_refresh();
    sys_vsync_wait();
    top_begin_frame();
    draw_static_chrome();
    draw_static_fixed();
    draw_header();
    draw_rows(1);
    top_end_frame();
    g_last_refresh = sys_get_ticks();

    while (g_running) {
        unsigned long long ev;
        while ((ev = sys_wm_event_poll()) != 0) {
            if (wm_event_is_redraw(ev)) {
                g_win_x = wm_event_redraw_x(ev);
                g_win_y = wm_event_redraw_y(ev);
                sys_vsync_wait();
                top_begin_frame();
                draw_static_chrome();
                draw_static_fixed();
                invalidate_rows();
                draw_header();
                draw_rows(1);
                top_end_frame();
                continue;
            }
            if (wm_is_window_closed(ev)) { g_running = 0; break; }
            if (wm_event_is_mouse(ev))   continue;

            key_event_t kev = key_event_unpack(ev);
            if (!kev.is_press) continue;

            switch (kev.keycode) {
            case KEY_UP:
                if (g_sel > 0) {
                    g_cache[g_sel].valid = 0;
                    g_sel--;
                    g_cache[g_sel].valid = 0;
                    top_begin_frame();
                    draw_rows(0);
                    top_end_frame();
                }
                break;
            case KEY_DOWN:
                if (g_sel < g_curr_cnt - 1 && g_sel < MAX_VISIBLE - 1) {
                    g_cache[g_sel].valid = 0;
                    g_sel++;
                    g_cache[g_sel].valid = 0;
                    top_begin_frame();
                    draw_rows(0);
                    top_end_frame();
                }
                break;
            case KEY_K:
                if (g_curr_cnt > 0 && g_sel < g_curr_cnt) {
                    unsigned int pid = g_curr[g_sel].pid;
                    if (pid > 1) {
                        sys_kill((long)pid);
                        do_refresh();
                        sys_vsync_wait();
                        top_begin_frame();
                        draw_header();
                        invalidate_rows();
                        draw_rows(1);
                        top_end_frame();
                        g_last_refresh = sys_get_ticks();
                    }
                }
                break;
            case KEY_R:
                do_refresh();
                sys_vsync_wait();
                top_begin_frame();
                draw_header();
                draw_rows(0);
                top_end_frame();
                g_last_refresh = sys_get_ticks();
                break;
            case KEY_Q:
            case KEY_ESC:
                g_running = 0;
                break;
            default:
                break;
            }
        }

        /* Periodic auto-refresh */
        long now = sys_get_ticks();
        if (now - g_last_refresh >= REFRESH_TICKS) {
            g_last_refresh = now;
            do_refresh();
            sys_vsync_wait();
            top_begin_frame();
            draw_header();
            draw_rows(0);
            top_end_frame();
        }

        sys_sched_yield();
    }

    if (g_bo != GPU_BO_INVALID) {
        gfx_clear_damage_target();
        sys_wm_set_buffer(g_win_id, GPU_BO_INVALID);
        gpu_free(g_bo);
    }
    sys_wm_request_close(g_win_id);
    sys_exit(0);
}
