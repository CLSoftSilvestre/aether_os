/*
 * AetherOS libwidget — Shared File Dialog (filedlg)
 * File: userspace/lib/libwidget/filedlg.c
 *
 * filedlg_open(start_dir, filter, out_path) → 1 confirmed / 0 cancelled
 * filedlg_save(start_dir, filter, init_name, out_path) → 1 / 0
 *
 * Layout (640×424 window, 26px title bar, 4px side pad):
 *   ┌──────────────────────────────────────────────────────────────────┐
 *   │  ●  Open File                                                    │
 *   ├────────────┬─────────────────────────────────────────────────────┤
 *   │            │                                                     │
 *   │  tree      │  list view (files + dirs)                          │
 *   │  (160 px)  │  (470 px)                                          │
 *   │            │                                                     │
 *   ├────────────┴──────────────────────────────────────────────────────┤
 *   │  File name: [__________________________] [  OK  ] [ Cancel ]     │
 *   │  Filter:    [__________]                                         │
 *   └──────────────────────────────────────────────────────────────────┘
 */

#include <widget.h>
#include <gfx.h>
#include <gpu.h>
#include <sys.h>
#include <input.h>
#include <vfs_mounts.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Dialog geometry ─────────────────────────────────────────────────────── */

#define DLG_W        640
#define DLG_H        424
#define DLG_TITLE_H   26
#define DLG_SIDE       4

#define DLG_CONT_W   (DLG_W - 2 * DLG_SIDE)           /* 632 */
#define DLG_CONT_H   (DLG_H - DLG_TITLE_H - 2 * DLG_SIDE) /* 390 */

#define DLG_TREE_W   160
#define DLG_SEP_W      2
#define DLG_LIST_X   (DLG_TREE_W + DLG_SEP_W)         /* 162 */
#define DLG_LIST_W   (DLG_CONT_W - DLG_LIST_X)        /* 470 */

#define DLG_BTM_H     68
#define DLG_BODY_H   (DLG_CONT_H - DLG_BTM_H - 2)    /* 320 */
#define DLG_BTM_Y    (DLG_BODY_H + 2)                  /* 322 */

/* Bottom bar widget positions (relative to content area) */
#define BTN_W   80
#define BTN_H   26
#define BTN_GAP  4

#define BTN_CANCEL_X  (DLG_CONT_W - BTN_GAP - BTN_W)           /* 548 */
#define BTN_OK_X      (BTN_CANCEL_X - BTN_GAP - BTN_W)         /* 464 */
#define FNAME_X        62
#define FNAME_W       (BTN_OK_X - FNAME_X - BTN_GAP)           /* 398 */

/* Listview hit-test constants (mirror of listview.c internals) */
#define LV_PAD_Y  2
#define LV_ROW_H  20   /* WGT_FONT_H(16) + 4 */

/* ── Path pools ──────────────────────────────────────────────────────────── */

#define TREE_PATH_MAX   96
#define TREE_POOL_SZ   128
#define LIST_PATH_MAX  128
#define LIST_POOL_SZ    64

static char s_tree_paths[TREE_POOL_SZ][TREE_PATH_MAX];
static int  s_tree_path_n;

static char s_list_paths[LIST_POOL_SZ][LIST_PATH_MAX];
static int  s_list_path_n;

/* ── Dialog-local state ──────────────────────────────────────────────────── */

static int  s_dlg_win_x, s_dlg_win_y;
static long s_dlg_win_id = -1;

static int  s_dlg_running;
static int  s_dlg_confirmed;

static char s_dlg_cur_dir[128];
static char s_dlg_result[FILEDLG_PATH_MAX];
static int  s_dlg_is_save;

/* Listview absolute origin (computed from content origin + widget bounds) */
static int  s_list_ax, s_list_ay;
static long s_last_click_tick;
static int  s_last_click_idx;

/* Dialog-local focus / hover */
static widget_t *s_dlg_focused;
static widget_t *s_dlg_hovered;
static unsigned  s_dlg_last_btns;

/* ── Widget instances ────────────────────────────────────────────────────── */

static widget_t       g_dlg_root;
static widget_t       g_dlg_tree;
static treeview_data_t g_dlg_td;
static widget_t       g_dlg_list;
static widget_t       g_dlg_sep_v;
static widget_t       g_dlg_sep_h;
static widget_t       g_dlg_lbl_fname;
static widget_t       g_dlg_inp_fname;
static widget_t       g_dlg_lbl_filter;
static widget_t       g_dlg_inp_filter;
static widget_t       g_dlg_btn_ok;
static widget_t       g_dlg_btn_cancel;

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void dlg_populate_list(void);
static void dlg_try_confirm(void);

/* ── Filter matching ─────────────────────────────────────────────────────── */

static int dlg_matches_filter(const char *name, const char *filter)
{
    if (!filter || !filter[0]) return 1;
    if (filter[0] != '*') return 1;        /* unsupported pattern = accept all */
    if (filter[1] == '\0') return 1;       /* "*" = all */
    if (filter[1] != '.') return 1;

    const char *ext = filter + 2;
    if (ext[0] == '*' && ext[1] == '\0') return 1;   /* "*.*" = all */

    /* "*.xyz" — look for last dot in name */
    int nlen = 0; while (name[nlen]) nlen++;
    int elen = 0; while (ext[elen])  elen++;
    if (nlen < elen + 2) return 0;
    if (name[nlen - elen - 1] != '.') return 0;

    for (int i = 0; i < elen; i++) {
        char a = ext[i], b = name[nlen - elen + i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

/* ── Build full path (dir + "/" + name) ──────────────────────────────────── */

static void build_path(char *out, int max, const char *dir, const char *name)
{
    int dlen = 0; while (dir[dlen]) dlen++;
    if (dlen > 0 && dir[dlen - 1] == '/')
        snprintf(out, max, "%s%s", dir, name);
    else
        snprintf(out, max, "%s/%s", dir, name);
}

/* ── Dialog-local focus management ──────────────────────────────────────── */

static void dlg_set_focused(widget_t *w)
{
    if (s_dlg_focused == w) return;

    if (s_dlg_focused) {
        if (s_dlg_focused->state == WS_FOCUSED)
            s_dlg_focused->state = WS_NORMAL;
        s_dlg_focused->dirty = 1;
        if (s_dlg_focused->event_fn) {
            widget_event_t ev; memset(&ev, 0, sizeof(ev));
            ev.type = WEV_FOCUS_OUT;
            s_dlg_focused->event_fn(s_dlg_focused, &ev);
        }
    }
    s_dlg_focused = w;
    if (s_dlg_focused) {
        s_dlg_focused->state = WS_FOCUSED;
        s_dlg_focused->dirty = 1;
        if (s_dlg_focused->event_fn) {
            widget_event_t ev; memset(&ev, 0, sizeof(ev));
            ev.type = WEV_FOCUS_IN;
            s_dlg_focused->event_fn(s_dlg_focused, &ev);
        }
    }
}

/* ── Dialog-local hit test ───────────────────────────────────────────────── */

static widget_t *dlg_hit_test(widget_t *w, int mx, int my, int pax, int pay)
{
    if (w->hidden) return NULL;
    int ax = pax + w->bounds.x;
    int ay = pay + w->bounds.y;
    if (mx < ax || mx >= ax + w->bounds.w || my < ay || my >= ay + w->bounds.h)
        return NULL;
    for (int i = w->nchildren - 1; i >= 0; i--) {
        widget_t *h = dlg_hit_test(w->children[i], mx, my, ax, ay);
        if (h) return h;
    }
    if (w->focusable || w->event_fn) return w;
    return NULL;
}

/* ── Dialog-local draw ───────────────────────────────────────────────────── */

static void dlg_draw_recursive(widget_t *w, int pax, int pay, int force)
{
    if (w->hidden) return;
    int ax = pax + w->bounds.x;
    int ay = pay + w->bounds.y;
    int was_dirty = (w->dirty || force);
    if (was_dirty && w->draw_fn) {
        w->draw_fn(w, ax, ay);
        w->dirty = 0;
    }
    for (int i = 0; i < w->nchildren; i++)
        dlg_draw_recursive(w->children[i], ax, ay, was_dirty);
}

/* ── Dialog chrome ───────────────────────────────────────────────────────── */

static void dlg_draw_chrome(const char *title)
{
    int wx = s_dlg_win_x, wy = s_dlg_win_y;

    /* Window fill */
    gfx_fill(wx, wy, DLG_W, DLG_H, C_WIN_BG);

    /* Title bar */
    gfx_fill(wx, wy, DLG_W, DLG_TITLE_H, C_TITLEBAR);
    gfx_hline(wx, wy, DLG_W, GFX_RGB(90, 84, 148));
    gfx_fill(wx, wy + 1, DLG_W, 2, GFX_RGB(60, 56, 100));

    /* Accent separator under title */
    gfx_hline(wx, wy + DLG_TITLE_H - 1, DLG_W, C_ACCENT);

    /* Outer rim */
    gfx_rect(wx, wy, DLG_W, DLG_H, GFX_RGB(160, 145, 230));

    /* Title text */
    gfx_text_center_transparent((unsigned)wx, DLG_W,
                                (unsigned)(wy + (DLG_TITLE_H - WGT_FONT_H) / 2),
                                title, C_TEXT);
}

/* ── Mouse dispatch ──────────────────────────────────────────────────────── */

static void dlg_dispatch_mouse(int cx, int cy, int mx, int my,
                                unsigned btns, unsigned prev_btns)
{
    int left_dn = (btns & 1) && !(prev_btns & 1);
    int left_up = !(btns & 1) && (prev_btns & 1);

    /* Hover update */
    widget_t *under = dlg_hit_test(&g_dlg_root, mx, my, cx, cy);
    if (under != s_dlg_hovered) {
        if (s_dlg_hovered && s_dlg_hovered->state == WS_HOVERED) {
            s_dlg_hovered->state = WS_NORMAL;
            s_dlg_hovered->dirty = 1;
        }
        s_dlg_hovered = under;
        if (s_dlg_hovered && s_dlg_hovered->state == WS_NORMAL) {
            s_dlg_hovered->state = WS_HOVERED;
            s_dlg_hovered->dirty = 1;
        }
    }

    /* Mouse move — forward to hovered for hover effects */
    if (s_dlg_hovered && s_dlg_hovered->event_fn) {
        widget_event_t ev; memset(&ev, 0, sizeof(ev));
        ev.type = WEV_MOUSE_MOVE; ev.mx = mx; ev.my = my; ev.buttons = btns;
        s_dlg_hovered->event_fn(s_dlg_hovered, &ev);
    }

    /* Listview mouse selection (listview widget doesn't compute rows itself) */
    if (left_dn) {
        int lv_ax = s_list_ax, lv_ay = s_list_ay;
        int lv_w  = g_dlg_list.bounds.w;
        int lv_h  = g_dlg_list.bounds.h;

        if (mx >= lv_ax && mx < lv_ax + lv_w &&
            my >= lv_ay && my < lv_ay + lv_h) {

            wdata_listview_t *d = &g_dlg_list.data.listview;
            int row = (my - lv_ay - LV_PAD_Y) / LV_ROW_H;
            int idx = d->scroll_top + row;
            if (idx >= 0 && idx < d->n_items) {
                long now = sys_get_ticks();
                int dbl = (idx == s_last_click_idx &&
                           now - s_last_click_tick < 30);
                d->selected = idx;
                g_dlg_list.dirty = 1;
                dlg_set_focused(&g_dlg_list);

                /* Update filename input with selected item */
                const char *lbl = d->items[idx].label;
                /* Directories shown as "[name]" — don't put brackets in input */
                if (lbl[0] == '[') {
                    /* entering a directory: navigate into it */
                    if (dbl) {
                        const char *path = (const char *)d->items[idx].userdata;
                        if (path && path[0]) {
                            strncpy(s_dlg_cur_dir, path, sizeof(s_dlg_cur_dir) - 1);
                            s_dlg_cur_dir[sizeof(s_dlg_cur_dir) - 1] = '\0';
                            dlg_populate_list();
                        }
                    }
                } else {
                    /* File: put name in filename input */
                    textinput_set_text(&g_dlg_inp_fname, lbl);
                    if (dbl) dlg_try_confirm();
                }
                s_last_click_idx = idx;
                s_last_click_tick = now;
            }
            return;   /* consumed by listview handler above */
        }
    }

    /* Normal widget dispatch */
    if (left_dn && under) {
        if (under->focusable) dlg_set_focused(under);
        under->state = WS_PRESSED;
        under->dirty = 1;
        if (under->event_fn) {
            widget_event_t ev; memset(&ev, 0, sizeof(ev));
            ev.type = WEV_MOUSE_DOWN; ev.mx = mx; ev.my = my; ev.buttons = btns;
            under->event_fn(under, &ev);
        }
    }
    if (left_up) {
        widget_t *tgt = under ? under : s_dlg_hovered;
        if (tgt) {
            if (tgt->state == WS_PRESSED) {
                tgt->state = (tgt == s_dlg_focused) ? WS_FOCUSED : WS_NORMAL;
                tgt->dirty = 1;
            }
            if (tgt->event_fn) {
                widget_event_t ev; memset(&ev, 0, sizeof(ev));
                ev.type = WEV_MOUSE_UP; ev.mx = mx; ev.my = my; ev.buttons = btns;
                tgt->event_fn(tgt, &ev);
            }
        }
    }
}

/* ── Key dispatch ────────────────────────────────────────────────────────── */

#define DLG_FOCUS_MAX 8
static widget_t *s_focus_list[DLG_FOCUS_MAX];
static int       s_focus_n;

static void dlg_collect_focusable(widget_t *w)
{
    if (w->hidden || s_focus_n >= DLG_FOCUS_MAX) return;
    if (w->focusable) s_focus_list[s_focus_n++] = w;
    for (int i = 0; i < w->nchildren; i++) dlg_collect_focusable(w->children[i]);
}

static void dlg_focus_cycle(int dir)
{
    s_focus_n = 0;
    dlg_collect_focusable(&g_dlg_root);
    if (!s_focus_n) return;
    int cur = -1;
    for (int i = 0; i < s_focus_n; i++)
        if (s_focus_list[i] == s_dlg_focused) { cur = i; break; }
    dlg_set_focused(s_focus_list[(cur + dir + s_focus_n) % s_focus_n]);
}

static void dlg_dispatch_key(const widget_event_t *ev)
{
    if (ev->keycode == KEY_TAB) {
        dlg_focus_cycle((ev->modifiers & MOD_SHIFT) ? -1 : 1);
        return;
    }
    if (s_dlg_focused && s_dlg_focused->event_fn)
        s_dlg_focused->event_fn(s_dlg_focused, ev);
}

/* ── Treeview populate / callbacks ───────────────────────────────────────── */

static void dlg_tree_expand(tv_node_t *node, void *ctx)
{
    (void)ctx;
    treeview_data_t *td = &g_dlg_td;
    int ni = (int)(node - td->nodes);

    const char *path = (const char *)node->userdata;
    if (!path) return;

    char dir_buf[2048];
    long n = sys_fs_readdir(path, dir_buf, sizeof(dir_buf) - 1);
    if (n < 0) return;
    dir_buf[n] = '\0';

    char *s = dir_buf;
    while (*s) {
        char *end = s;
        while (*end && *end != '\n') end++;

        if (*s == '[' && end > s + 2 && *(end - 1) == ']') {
            int dlen = (int)(end - s - 2);
            if (dlen > 0 && s_tree_path_n < TREE_POOL_SZ) {
                char dirname[64];
                if (dlen > 63) dlen = 63;
                for (int i = 0; i < dlen; i++) dirname[i] = s[i + 1];
                dirname[dlen] = '\0';

                char *fullpath = s_tree_paths[s_tree_path_n++];
                build_path(fullpath, TREE_PATH_MAX, path, dirname);
                treeview_add_child(&g_dlg_tree, ni, dirname,
                                   TVICON_FOLDER_CLOSED, 1, fullpath);
            }
        }
        if (!*end) break;
        s = end + 1;
    }
    treeview_rebuild_visible(&g_dlg_tree);
}

static void dlg_tree_select(tv_node_t *node, void *ctx)
{
    (void)ctx;
    const char *path = (const char *)node->userdata;
    if (!path) return;
    strncpy(s_dlg_cur_dir, path, sizeof(s_dlg_cur_dir) - 1);
    s_dlg_cur_dir[sizeof(s_dlg_cur_dir) - 1] = '\0';
    dlg_populate_list();
}

/* ── List population ─────────────────────────────────────────────────────── */

static void dlg_populate_list(void)
{
    listview_clear(&g_dlg_list);
    s_list_path_n = 0;

    char dir_buf[4096];
    long n = sys_fs_readdir(s_dlg_cur_dir, dir_buf, sizeof(dir_buf) - 1);
    if (n < 0) {
        g_dlg_list.dirty = 1;
        return;
    }
    dir_buf[n] = '\0';

    const char *filter = textinput_get_text(&g_dlg_inp_filter);

    /* First pass: subdirectories */
    char *s = dir_buf;
    while (*s) {
        char *end = s;
        while (*end && *end != '\n') end++;

        if (*s == '[' && end > s + 2 && *(end - 1) == ']') {
            int dlen = (int)(end - s - 2);
            if (dlen > 0 && s_list_path_n < LIST_POOL_SZ) {
                char dirname[WGT_LISTITEM_LABEL - 2];
                if (dlen > (int)sizeof(dirname) - 1) dlen = (int)sizeof(dirname) - 1;
                for (int i = 0; i < dlen; i++) dirname[i] = s[i + 1];
                dirname[dlen] = '\0';

                char label[WGT_LISTITEM_LABEL];
                snprintf(label, sizeof(label), "[%s]", dirname);
                char *fp = s_list_paths[s_list_path_n++];
                build_path(fp, LIST_PATH_MAX, s_dlg_cur_dir, dirname);
                listview_add_item(&g_dlg_list, label, fp);
            }
        }
        if (!*end) break;
        s = end + 1;
    }

    /* Second pass: files matching filter */
    s = dir_buf;
    while (*s) {
        char *end = s;
        while (*end && *end != '\n') end++;

        if (*s != '[') {
            char fname[WGT_LISTITEM_LABEL];
            int fi = 0;
            char *p = s;
            while (p < end && *p != ' ' && fi < (int)sizeof(fname) - 1)
                fname[fi++] = *p++;
            fname[fi] = '\0';

            if (fi > 0 && dlg_matches_filter(fname, filter) &&
                s_list_path_n < LIST_POOL_SZ) {
                char *fp = s_list_paths[s_list_path_n++];
                build_path(fp, LIST_PATH_MAX, s_dlg_cur_dir, fname);
                listview_add_item(&g_dlg_list, fname, fp);
            }
        }
        if (!*end) break;
        s = end + 1;
    }

    g_dlg_list.dirty = 1;
}

/* ── Confirm selection ───────────────────────────────────────────────────── */

static void dlg_try_confirm(void)
{
    const char *fname = textinput_get_text(&g_dlg_inp_fname);
    if (!fname || !fname[0]) {
        /* If nothing in text input, use selected list item if it's a file */
        wdata_listview_t *d = &g_dlg_list.data.listview;
        int sel = d->selected;
        if (sel >= 0 && sel < d->n_items) {
            const char *lbl = d->items[sel].label;
            if (lbl[0] != '[') {  /* it's a file, not a dir */
                const char *fp = (const char *)d->items[sel].userdata;
                if (fp && fp[0]) {
                    strncpy(s_dlg_result, fp, FILEDLG_PATH_MAX - 1);
                    s_dlg_result[FILEDLG_PATH_MAX - 1] = '\0';
                    s_dlg_confirmed = 1;
                    s_dlg_running   = 0;
                }
            }
        }
        return;
    }

    /* Compose full path from current dir + typed name */
    build_path(s_dlg_result, FILEDLG_PATH_MAX, s_dlg_cur_dir, fname);
    s_dlg_confirmed = 1;
    s_dlg_running   = 0;
}

/* ── Button callbacks ────────────────────────────────────────────────────── */

static void on_btn_ok(widget_t *w)     { (void)w; dlg_try_confirm(); }
static void on_btn_cancel(widget_t *w) { (void)w; s_dlg_running = 0; s_dlg_confirmed = 0; }

static void on_filter_change(widget_t *w) { (void)w; dlg_populate_list(); }
static void on_fname_submit(widget_t *w)  { (void)w; dlg_try_confirm(); }

/* ── Build dialog widget tree ────────────────────────────────────────────── */

static void dlg_build_ui(const char *start_dir, const char *filter,
                          const char *init_name)
{
    /* Reset path pools */
    s_tree_path_n = 0;
    s_list_path_n = 0;
    s_dlg_focused = NULL;
    s_dlg_hovered = NULL;
    s_last_click_idx  = -1;
    s_last_click_tick = 0;

    strncpy(s_dlg_cur_dir, start_dir ? start_dir : "/",
            sizeof(s_dlg_cur_dir) - 1);

    widget_init_panel(&g_dlg_root, 0, 0, DLG_CONT_W, DLG_CONT_H, C_WIN_BG);

    /* Treeview */
    memset(&g_dlg_td, 0, sizeof(g_dlg_td));
    treeview_init(&g_dlg_tree, 0, 0, DLG_TREE_W, DLG_BODY_H, &g_dlg_td);
    g_dlg_td.bg_color = C_PANEL;
    treeview_set_callbacks(&g_dlg_tree, dlg_tree_select, dlg_tree_expand, NULL);

    /* Populate treeview with available mounts */
    mount_info_t mounts[8];
    vfs_probe_mounts();
    int nm = vfs_get_mounts(mounts, 8);
    for (int i = 0; i < nm; i++) {
        if (s_tree_path_n < TREE_POOL_SZ) {
            char *fp = s_tree_paths[s_tree_path_n++];
            strncpy(fp, mounts[i].path, TREE_PATH_MAX - 1);
            fp[TREE_PATH_MAX - 1] = '\0';
            treeview_add_root(&g_dlg_tree, mounts[i].label,
                              mounts[i].icon_type, fp);
        }
    }
    treeview_rebuild_visible(&g_dlg_tree);

    /* Vertical separator */
    widget_init_panel(&g_dlg_sep_v, DLG_TREE_W, 0, DLG_SEP_W, DLG_BODY_H, C_SEP);

    /* Listview */
    widget_init_listview(&g_dlg_list, DLG_LIST_X, 0, DLG_LIST_W, DLG_BODY_H,
                         LIST_POOL_SZ, NULL);

    /* Horizontal separator */
    widget_init_panel(&g_dlg_sep_h, 0, DLG_BODY_H, DLG_CONT_W, 2, C_SEP);

    /* Bottom bar */
    widget_init_label(&g_dlg_lbl_fname,
                      2, DLG_BTM_Y + 7, FNAME_X - 4, 16,
                      "File name:", WGT_ALIGN_LEFT);
    widget_init_textinput(&g_dlg_inp_fname,
                          FNAME_X, DLG_BTM_Y + 2, FNAME_W, BTN_H,
                          NULL, on_fname_submit);
    widget_init_button(&g_dlg_btn_ok,
                       BTN_OK_X, DLG_BTM_Y + 2, BTN_W, BTN_H,
                       "OK", on_btn_ok);
    widget_init_button(&g_dlg_btn_cancel,
                       BTN_CANCEL_X, DLG_BTM_Y + 2, BTN_W, BTN_H,
                       "Cancel", on_btn_cancel);

    widget_init_label(&g_dlg_lbl_filter,
                      2, DLG_BTM_Y + 39, FNAME_X - 4, 16,
                      "Filter:", WGT_ALIGN_LEFT);
    widget_init_textinput(&g_dlg_inp_filter,
                          FNAME_X, DLG_BTM_Y + 34, 200, BTN_H,
                          on_filter_change, NULL);

    /* Build tree */
    widget_add_child(&g_dlg_root, &g_dlg_tree);
    widget_add_child(&g_dlg_root, &g_dlg_sep_v);
    widget_add_child(&g_dlg_root, &g_dlg_list);
    widget_add_child(&g_dlg_root, &g_dlg_sep_h);
    widget_add_child(&g_dlg_root, &g_dlg_lbl_fname);
    widget_add_child(&g_dlg_root, &g_dlg_inp_fname);
    widget_add_child(&g_dlg_root, &g_dlg_btn_ok);
    widget_add_child(&g_dlg_root, &g_dlg_btn_cancel);
    widget_add_child(&g_dlg_root, &g_dlg_lbl_filter);
    widget_add_child(&g_dlg_root, &g_dlg_inp_filter);

    /* Pre-fill filter */
    if (filter && filter[0])
        textinput_set_text(&g_dlg_inp_filter, filter);
    else
        textinput_set_text(&g_dlg_inp_filter, "*.*");

    /* Pre-fill filename (save mode) */
    if (init_name && init_name[0])
        textinput_set_text(&g_dlg_inp_fname, init_name);

    /* Populate list for starting directory */
    dlg_populate_list();

    /* Start focus on filename input */
    dlg_set_focused(&g_dlg_inp_fname);
}

/* ── Core dialog runner ──────────────────────────────────────────────────── */

static int dlg_run(const char *title, const char *start_dir,
                   const char *filter, const char *init_name,
                   char *out_path)
{
    s_dlg_running   = 1;
    s_dlg_confirmed = 0;
    s_dlg_last_btns = 0;
    s_dlg_result[0] = '\0';
    s_dlg_is_save   = (init_name != NULL);

    /* Center on screen */
    unsigned sw = gfx_width(), sh = gfx_height();
    s_dlg_win_x = (int)((sw > DLG_W) ? (sw - DLG_W) / 2 : 0);
    s_dlg_win_y = (int)((sh > DLG_H) ? (sh - DLG_H) / 2 : 0);

    s_dlg_win_id = sys_wm_register(s_dlg_win_x, s_dlg_win_y,
                                    DLG_W, DLG_H, title);
    sys_wm_focus_set(sys_getpid());

    /* Build widget tree */
    dlg_build_ui(start_dir, filter, init_name);

    /* Content area origin */
    int cx = s_dlg_win_x + DLG_SIDE;
    int cy = s_dlg_win_y + DLG_TITLE_H + DLG_SIDE;

    /* Listview absolute position (fixed for the lifetime of the dialog) */
    s_list_ax = cx + g_dlg_list.bounds.x;
    s_list_ay = cy + g_dlg_list.bounds.y;

    /* Save the caller's damage target (the parent app's win_id) so we can
     * restore it after the dialog exits.  Without this, gfx_end_frame() would
     * signal sys_wm_damage() for the PARENT window on every dialog frame,
     * and the dialog's own buffer would never reach the screen. */
    int saved_damage_win = gfx_current_damage_win();
    gfx_clear_damage_target();

    /* Allocate a GPU BO for the dialog window (mirrors widget_run() approach).
     * In GPU-BO mode the compositor reads from the BO; we must call
     * sys_wm_set_buffer() to associate the BO, then gfx_set_damage_target()
     * so gfx_end_frame() signals sys_wm_damage() for the dialog's win_id. */
    unsigned dlg_frame_bytes = (unsigned)DLG_W * DLG_H * sizeof(unsigned);
    gpu_bo_t  dlg_bo  = gpu_alloc(dlg_frame_bytes);
    unsigned *fb      = NULL;
    int       use_bo  = 0;

    if (dlg_bo != GPU_BO_INVALID) {
        fb = (unsigned *)gpu_map(dlg_bo);
        if (fb) {
            sys_wm_set_buffer(s_dlg_win_id, dlg_bo);
            gfx_set_damage_target((int)s_dlg_win_id);
            use_bo = 1;
        } else {
            gpu_free(dlg_bo);
            dlg_bo = GPU_BO_INVALID;
        }
    }
    if (!use_bo) {
        fb = (unsigned *)malloc(dlg_frame_bytes);
        if (!fb) {
            if (saved_damage_win >= 0) gfx_set_damage_target(saved_damage_win);
            sys_wm_request_close(s_dlg_win_id);
            return 0;
        }
    }

    /* Initial draw */
    gfx_begin_frame(fb, DLG_W, DLG_H, s_dlg_win_x, s_dlg_win_y);
    dlg_draw_chrome(title);
    dlg_draw_recursive(&g_dlg_root, cx, cy, 1);
    gfx_end_frame();

    /* Event loop */
    while (s_dlg_running) {
        unsigned long long raw;
        while ((raw = sys_wm_event_poll()) != 0) {
            if ((raw >> 56) == WM_EV_CLOSE_REQUEST) {
                s_dlg_running = 0;
                break;
            }

            if (wm_event_is_redraw(raw)) {
                s_dlg_win_x = wm_event_redraw_x(raw);
                s_dlg_win_y = wm_event_redraw_y(raw);
                cx = s_dlg_win_x + DLG_SIDE;
                cy = s_dlg_win_y + DLG_TITLE_H + DLG_SIDE;
                s_list_ax = cx + g_dlg_list.bounds.x;
                s_list_ay = cy + g_dlg_list.bounds.y;
                widget_invalidate_all(&g_dlg_root);
                gfx_begin_frame(fb, DLG_W, DLG_H, s_dlg_win_x, s_dlg_win_y);
                dlg_draw_chrome(title);
                dlg_draw_recursive(&g_dlg_root, cx, cy, 1);
                gfx_end_frame();
                continue;
            }

            if (wm_event_is_mouse(raw)) {
                mouse_event_t mev = wm_event_mouse_unpack(raw);
                dlg_dispatch_mouse(cx, cy, (int)mev.x, (int)mev.y,
                                   mev.buttons, s_dlg_last_btns);
                s_dlg_last_btns = mev.buttons;
                continue;
            }

            key_event_t kev = key_event_unpack(raw);
            if (!kev.is_press) continue;

            if (kev.keycode == KEY_ESC) {
                s_dlg_confirmed = 0;
                s_dlg_running   = 0;
                break;
            }
            if (kev.keycode == KEY_ENTER) {
                dlg_try_confirm();
                continue;
            }

            widget_event_t ev; memset(&ev, 0, sizeof(ev));
            ev.type      = WEV_KEY_DOWN;
            ev.keycode   = kev.keycode;
            ev.modifiers = kev.modifiers;
            dlg_dispatch_key(&ev);
        }

        /* Redraw dirty widgets */
        gfx_begin_frame(fb, DLG_W, DLG_H, s_dlg_win_x, s_dlg_win_y);
        dlg_draw_recursive(&g_dlg_root, cx, cy, 0);
        gfx_end_frame();

        sys_sched_yield();
    }

    /* Tear down the dialog's frame buffer and WM window. */
    if (use_bo) {
        gfx_clear_damage_target();
        sys_wm_set_buffer(s_dlg_win_id, GPU_BO_INVALID);
        gpu_free(dlg_bo);
    } else {
        free(fb);
    }
    sys_wm_request_close(s_dlg_win_id);

    /* Restore the parent app's damage target so its event loop keeps working. */
    if (saved_damage_win >= 0)
        gfx_set_damage_target(saved_damage_win);

    if (s_dlg_confirmed && out_path)
        strncpy(out_path, s_dlg_result, FILEDLG_PATH_MAX - 1);
    if (out_path) out_path[FILEDLG_PATH_MAX - 1] = '\0';

    return s_dlg_confirmed;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int filedlg_open(const char *start_dir, const char *filter, char *out_path)
{
    return dlg_run("Open File", start_dir, filter, NULL, out_path);
}

int filedlg_save(const char *start_dir, const char *filter,
                 const char *init_name, char *out_path)
{
    return dlg_run("Save File", start_dir, filter, init_name, out_path);
}
