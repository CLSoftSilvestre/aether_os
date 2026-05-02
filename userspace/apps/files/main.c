/*
 * AetherOS — Files App  (Phase 5.6)
 * File: userspace/apps/files/main.c
 *
 * Two-pane file explorer:
 *   Left  (220 px) — WIDGET_TREEVIEW over all VFS mounts; lazy-loads subdirs
 *   Right (679 px) — Icon grid; 80×88 px cells, per-type icons, double-click opens
 *   Bottom (24 px) — Status bar: "N folders · M files   FSLabel"
 *
 * Double-click: folder → navigate in; .txt → spawn textviewer argv[1];
 *               .as → spawn aether_editor argv[1]; others ignored
 * Keyboard (right pane focused): arrows navigate, Enter activates,
 *   Backspace goes up, PgUp/PgDn scroll the grid.
 */

#include <gfx.h>
#include <sys.h>
#include <widget.h>
#include <vfs_mounts.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <input.h>

/* ── Window geometry ──────────────────────────────────────────────────── */

#define WIN_W     900
#define WIN_H     600
#define TITLE_H    28

#define TREE_W    220
#define DIV_W       1
#define RIGHT_W   (WIN_W - TREE_W - DIV_W)   /* 679 */
#define CONT_H    (WIN_H - TITLE_H)           /* 572 */
#define STATUS_H   24
#define PANE_H    (CONT_H - STATUS_H)         /* 548 */

/* Icon grid cell */
#define CELL_W    80
#define CELL_H    88
#define ICON_SZ   48
#define ICON_OFF_X  ((CELL_W - ICON_SZ) / 2)   /* 16 */
#define ICON_OFF_Y   4
#define CELLS_PER_ROW  (RIGHT_W / CELL_W)        /* 8 */

/* Tree */
#define TREE_ROW_H  20

/* Double-click window: 50 × 10 ms = 500 ms */
#define DCLICK_TICKS  50

/* ── File entry icon types ────────────────────────────────────────────── */

#define FICON_FOLDER   0
#define FICON_TXT      1
#define FICON_AS       2
#define FICON_EXEC     3
#define FICON_GENERIC  4

typedef struct {
    char          name[64];
    char          path[128];
    unsigned char icon_type;
    unsigned char is_dir;
} file_entry_t;

/* ── Static pools ─────────────────────────────────────────────────────── */

#define MAX_ENTRIES     128
#define PATH_POOL_SIZE  128
#define PATH_POOL_LEN   128

static file_entry_t g_entries[MAX_ENTRIES];
static int          g_entry_count;
static int          g_grid_scroll_y;
static int          g_selected_entry;

static treeview_data_t g_tv_data;

static char g_path_pool[PATH_POOL_SIZE][PATH_POOL_LEN];
static int  g_path_pool_idx;

static char g_current_path[128];
static char g_current_fs_label[32];

/* Double-click tracking */
static long g_last_click_tick;
static int  g_last_click_entry;

/* Right panel absolute origin (set during draw, used during events) */
static int g_right_ax;
static int g_right_ay;

/* ── Window state ─────────────────────────────────────────────────────── */

static int  g_win_x = 50;
static int  g_win_y = 60;
static long g_win_id = -1;

static widget_ctx_t g_ctx;

/* ── Widget tree ──────────────────────────────────────────────────────── */

static widget_t g_root;
static widget_t g_tree;
static widget_t g_divider;
static widget_t g_right;
static widget_t g_statusbar;

/* ── Forward declarations ─────────────────────────────────────────────── */

static void files_navigate_to(const char *path);
static void draw_frame(void);
static void draw_title_text(void);

/* ── Path pool allocator ──────────────────────────────────────────────── */

static char *alloc_tree_path(const char *src)
{
    if (g_path_pool_idx >= PATH_POOL_SIZE) return NULL;
    char *slot = g_path_pool[g_path_pool_idx++];
    int i = 0;
    while (i < PATH_POOL_LEN - 1 && src[i]) { slot[i] = src[i]; i++; }
    slot[i] = '\0';
    return slot;
}

/* ── FS label from path prefix ────────────────────────────────────────── */

static void set_fs_label(const char *path)
{
    if (path[0] == '/' && path[1] == 'a' && path[2] == 'f' && path[3] == 's')
        strncpy(g_current_fs_label, "AetherFS", 31);
    else if (path[0] == '/' && path[1] == 'i')
        strncpy(g_current_fs_label, "InitRD", 31);
    else
        strncpy(g_current_fs_label, "FAT32 (/)", 31);
    g_current_fs_label[31] = '\0';
}

/* ── Extension-based icon type ────────────────────────────────────────── */

static unsigned char detect_icon(const char *name)
{
    int n = (int)strlen(name);
    if (n >= 4 && name[n-4]=='.' && name[n-3]=='t' &&
        name[n-2]=='x' && name[n-1]=='t')
        return FICON_TXT;
    if (n >= 3 && name[n-3]=='.' && name[n-2]=='a' && name[n-1]=='s')
        return FICON_AS;
    if (n >= 3 && name[n-3]=='.' && name[n-2]=='A' && name[n-1]=='S')
        return FICON_AS;
    if (n >= 4 && name[n-4]=='.' && name[n-3]=='e' &&
        name[n-2]=='l' && name[n-1]=='f')
        return FICON_EXEC;
    return FICON_GENERIC;
}

/* ── Status bar ───────────────────────────────────────────────────────── */

static void update_statusbar(void)
{
    int dirs = 0, files = 0;
    for (int i = 0; i < g_entry_count; i++) {
        if (g_entries[i].is_dir) dirs++;
        else files++;
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "  %d folder%s, %d file%s    %s",
             dirs,  dirs  == 1 ? "" : "s",
             files, files == 1 ? "" : "s",
             g_current_fs_label);
    label_set_text(&g_statusbar, msg);
}

/* ── Navigate parent ──────────────────────────────────────────────────── */

static void navigate_parent(void)
{
    char parent[128];
    strncpy(parent, g_current_path, 127);
    parent[127] = '\0';

    int len = (int)strlen(parent);
    if (len <= 1) return;   /* already at root */

    /* Strip trailing slash if present */
    if (parent[len - 1] == '/') { parent[--len] = '\0'; }
    /* Strip last path component */
    while (len > 0 && parent[len - 1] != '/') len--;
    if (len > 1) len--;    /* keep leading '/' */
    parent[len > 0 ? len : 1] = '\0';
    if (parent[0] == '\0') { parent[0] = '/'; parent[1] = '\0'; }

    files_navigate_to(parent);
}

/* ── Activate an entry (double-click or Enter) ────────────────────────── */

static void activate_entry(int idx)
{
    if (idx < 0 || idx >= g_entry_count) return;
    file_entry_t *e = &g_entries[idx];

    if (e->is_dir) {
        files_navigate_to(e->path);
        return;
    }

    switch (e->icon_type) {
    case FICON_TXT: {
        const char *argv[] = { "/textviewer", e->path, (const char *)0 };
        sys_spawn_args("/textviewer", argv, 2);
        break;
    }
    case FICON_AS: {
        const char *argv[] = { "/aether_editor", e->path, (const char *)0 };
        sys_spawn_args("/aether_editor", argv, 2);
        break;
    }
    default:
        break;
    }
}

/* ── Sync tree selection to the current path ──────────────────────────── */

static void tree_sync_selection(const char *path)
{
    for (int vi = 0; vi < g_tv_data.visible_count; vi++) {
        int ni = g_tv_data.visible[vi];
        const char *npath = (const char *)g_tv_data.nodes[ni].userdata;
        if (npath && strcmp(npath, path) == 0) {
            g_tv_data.selected = vi;
            widget_invalidate(&g_tree);
            return;
        }
    }
    /* Not visible in tree — just clear selection */
    g_tv_data.selected = -1;
    widget_invalidate(&g_tree);
}

/* ── Icon grid navigate ────────────────────────────────────────────────── */

static void files_navigate_to(const char *path)
{
    strncpy(g_current_path, path, 127);
    g_current_path[127] = '\0';
    set_fs_label(path);

    g_entry_count    = 0;
    g_grid_scroll_y  = 0;
    g_selected_entry = -1;

    static char dir_buf[8192];
    long n = sys_fs_readdir(path, dir_buf, sizeof(dir_buf) - 1);
    if (n > 0) {
        dir_buf[n] = '\0';
        char *s = dir_buf;
        while (*s && g_entry_count < MAX_ENTRIES) {
            char *end = s;
            while (*end && *end != '\n') end++;

            if (s < end) {
                file_entry_t *e = &g_entries[g_entry_count];

                if (*s == '[') {
                    /* Directory: [name] */
                    char *close = s + 1;
                    while (*close && *close != ']') close++;
                    int len = (int)(close - (s + 1));
                    if (len > 0 && len < 64) {
                        /* Skip "." and ".." */
                        int is_dot = (len == 1 && s[1] == '.') ||
                                     (len == 2 && s[1] == '.' && s[2] == '.');
                        if (!is_dot) {
                            int j;
                            for (j = 0; j < len; j++) e->name[j] = s[1 + j];
                            e->name[j] = '\0';
                            if (path[0] == '/' && path[1] == '\0')
                                snprintf(e->path, 128, "/%s", e->name);
                            else
                                snprintf(e->path, 128, "%s/%s", path, e->name);
                            e->icon_type = FICON_FOLDER;
                            e->is_dir    = 1;
                            g_entry_count++;
                        }
                    }
                } else {
                    /* File: name <size> bytes */
                    int j = 0;
                    char *p = s;
                    while (p < end && *p != ' ' && j < 63) e->name[j++] = *p++;
                    e->name[j] = '\0';

                    if (e->name[0] && !(e->name[0]=='.' && (e->name[1]=='\0' ||
                        (e->name[1]=='.' && e->name[2]=='\0')))) {
                        if (path[0] == '/' && path[1] == '\0')
                            snprintf(e->path, 128, "/%s", e->name);
                        else
                            snprintf(e->path, 128, "%s/%s", path, e->name);
                        e->icon_type = detect_icon(e->name);
                        e->is_dir    = 0;
                        g_entry_count++;
                    }
                }
            }

            if (!*end) break;
            s = end + 1;
        }
    }

    draw_title_text();
    update_statusbar();
    tree_sync_selection(path);
    widget_invalidate(&g_right);
}

/* ── Right panel draw ─────────────────────────────────────────────────── */

static void right_panel_draw(widget_t *w, int ax, int ay)
{
    g_right_ax = ax;
    g_right_ay = ay;

    int pw = w->bounds.w;
    int ph = w->bounds.h;

    gfx_fill((unsigned)ax, (unsigned)ay, (unsigned)pw, (unsigned)ph, C_WIN_BG);

    for (int i = 0; i < g_entry_count; i++) {
        int row = i / CELLS_PER_ROW;
        int col = i % CELLS_PER_ROW;
        int cx  = ax + col * CELL_W;
        int cy  = ay + row * CELL_H - g_grid_scroll_y;

        /* Vertical clipping */
        if (cy + CELL_H <= ay || cy >= ay + ph) continue;

        /* Selection / hover background */
        if (i == g_selected_entry) {
            gfx_fill((unsigned)cx, (unsigned)cy,
                     CELL_W, CELL_H, GFX_RGB(40, 34, 82));
        }

        /* Icon (48×48) centred horizontally in cell */
        int icon_x = cx + ICON_OFF_X;
        int icon_y = cy + ICON_OFF_Y;
        switch (g_entries[i].icon_type) {
        case FICON_FOLDER:  gfx_icon_folder(icon_x, icon_y, ICON_SZ);       break;
        case FICON_TXT:     gfx_icon_file_txt(icon_x, icon_y, ICON_SZ);     break;
        case FICON_AS:      gfx_icon_file_as(icon_x, icon_y, ICON_SZ);      break;
        case FICON_EXEC:    gfx_icon_file_exec(icon_x, icon_y, ICON_SZ);    break;
        default:            gfx_icon_file_generic(icon_x, icon_y, ICON_SZ); break;
        }

        /* Label — two-line clipped to CELL_W at 8px font */
        unsigned lbg = (i == g_selected_entry)
                     ? GFX_RGB(40, 34, 82) : C_WIN_BG;
        int max_chars = CELL_W / 8;          /* 10 chars */
        char line1[12], line2[12];
        int  nlen = (int)strlen(g_entries[i].name);

        if (nlen <= max_chars) {
            /* Fits on one line */
            int j;
            for (j = 0; j < nlen; j++) line1[j] = g_entries[i].name[j];
            line1[j] = '\0';
            line2[0] = '\0';
        } else {
            /* Split across two lines */
            int j;
            for (j = 0; j < max_chars; j++) line1[j] = g_entries[i].name[j];
            line1[j] = '\0';
            int rem = nlen - max_chars;
            if (rem > max_chars) rem = max_chars;
            for (j = 0; j < rem; j++) line2[j] = g_entries[i].name[max_chars + j];
            line2[j] = '\0';
        }

        int label_x = cx + (CELL_W - (int)strlen(line1) * 8) / 2;
        gfx_text((unsigned)label_x, (unsigned)(cy + 56), line1, C_TEXT, lbg);
        if (line2[0]) {
            int lx2 = cx + (CELL_W - (int)strlen(line2) * 8) / 2;
            gfx_text((unsigned)lx2, (unsigned)(cy + 66), line2, C_TEXT_DIM, lbg);
        }
    }
}

/* ── Right panel event ────────────────────────────────────────────────── */

static int right_panel_event(widget_t *w, const widget_event_t *ev)
{
    if (ev->type == WEV_FOCUS_IN || ev->type == WEV_FOCUS_OUT) {
        w->dirty = 1;
        return 0;
    }

    if (ev->type == WEV_MOUSE_DOWN) {
        int rel_x = ev->mx - g_right_ax;
        int rel_y = ev->my - g_right_ay + g_grid_scroll_y;
        if (rel_x < 0 || rel_y < 0) return 0;

        int col = rel_x / CELL_W;
        int row = rel_y / CELL_H;
        if (col < 0 || col >= CELLS_PER_ROW) return 0;

        int idx = row * CELLS_PER_ROW + col;
        if (idx < 0 || idx >= g_entry_count) {
            /* Clicked empty area — clear selection */
            g_selected_entry = -1;
            widget_invalidate(w);
            return 1;
        }

        /* Double-click detection */
        long now = sys_get_ticks();
        int is_dc = (idx == g_last_click_entry &&
                     now - g_last_click_tick <= DCLICK_TICKS);
        g_last_click_tick  = now;
        g_last_click_entry = idx;

        g_selected_entry = idx;
        widget_invalidate(w);

        if (is_dc) activate_entry(idx);

        return 1;
    }

    if (ev->type == WEV_KEY_DOWN) {
        /* Grid arrow navigation */
        switch (ev->keycode) {
        case KEY_LEFT:
            if (g_selected_entry > 0) {
                g_selected_entry--;
                widget_invalidate(w);
            }
            return 1;

        case KEY_RIGHT:
            if (g_selected_entry < g_entry_count - 1) {
                g_selected_entry++;
                widget_invalidate(w);
            }
            return 1;

        case KEY_UP:
            if (g_selected_entry >= CELLS_PER_ROW) {
                g_selected_entry -= CELLS_PER_ROW;
                /* Scroll up if selection moved above viewport */
                int sel_row = g_selected_entry / CELLS_PER_ROW;
                int top_row = g_grid_scroll_y / CELL_H;
                if (sel_row < top_row) {
                    g_grid_scroll_y -= CELL_H;
                    if (g_grid_scroll_y < 0) g_grid_scroll_y = 0;
                }
                widget_invalidate(w);
            }
            return 1;

        case KEY_DOWN:
            if (g_entry_count > 0 &&
                g_selected_entry + CELLS_PER_ROW < g_entry_count) {
                g_selected_entry += CELLS_PER_ROW;
                /* Scroll down if selection moved below viewport */
                int sel_row = g_selected_entry / CELLS_PER_ROW;
                int bot_row = (g_grid_scroll_y + PANE_H) / CELL_H;
                if (sel_row >= bot_row) {
                    g_grid_scroll_y += CELL_H;
                }
                widget_invalidate(w);
            }
            return 1;

        case KEY_ENTER:
            if (g_selected_entry >= 0) activate_entry(g_selected_entry);
            return 1;

        case KEY_BACKSPACE:
            navigate_parent();
            return 1;

        case KEY_PGUP:
            g_grid_scroll_y -= PANE_H;
            if (g_grid_scroll_y < 0) g_grid_scroll_y = 0;
            widget_invalidate(w);
            return 1;

        case KEY_PGDN: {
            int total_rows = (g_entry_count + CELLS_PER_ROW - 1) / CELLS_PER_ROW;
            int max_scroll = total_rows * CELL_H - PANE_H;
            g_grid_scroll_y += PANE_H;
            if (max_scroll > 0 && g_grid_scroll_y > max_scroll)
                g_grid_scroll_y = max_scroll;
            widget_invalidate(w);
            return 1;
        }

        default:
            break;
        }
    }

    return 0;
}

/* ── Tree callbacks ───────────────────────────────────────────────────── */

static void tree_on_select(tv_node_t *node, void *ctx)
{
    (void)ctx;
    if (node->userdata)
        files_navigate_to((const char *)node->userdata);
}

static void tree_on_expand(tv_node_t *node, void *ctx)
{
    (void)ctx;
    const char *path = (const char *)node->userdata;
    if (!path) return;

    int node_idx = (int)(node - &g_tv_data.nodes[0]);

    static char dir_buf[4096];
    long n = sys_fs_readdir(path, dir_buf, sizeof(dir_buf) - 1);
    if (n <= 0) return;
    dir_buf[n] = '\0';

    char *s = dir_buf;
    while (*s) {
        char *end = s;
        while (*end && *end != '\n') end++;

        if (*s == '[' && s < end) {
            char *close = s + 1;
            while (*close && *close != ']') close++;
            int len = (int)(close - (s + 1));

            if (len > 0 && len < 64) {
                /* Skip "." and ".." */
                int is_dot = (len == 1 && s[1] == '.') ||
                             (len == 2 && s[1] == '.' && s[2] == '.');
                if (!is_dot) {
                    char dirname[64];
                    int j;
                    for (j = 0; j < len; j++) dirname[j] = s[1 + j];
                    dirname[j] = '\0';

                    char full[128];
                    if (path[0] == '/' && path[1] == '\0')
                        snprintf(full, sizeof(full), "/%s", dirname);
                    else
                        snprintf(full, sizeof(full), "%s/%s", path, dirname);

                    char *child_path = alloc_tree_path(full);
                    if (child_path) {
                        treeview_add_child(&g_tree, node_idx, dirname,
                                           TVICON_FOLDER_CLOSED, 1, child_path);
                    }
                }
            }
        }

        if (!*end) break;
        s = end + 1;
    }
}

/* ── Window frame ─────────────────────────────────────────────────────── */

static void draw_title_text(void)
{
    int wx = g_win_x, wy = g_win_y;
    char title[96];
    snprintf(title, sizeof(title), "Files  --  %s",
             g_current_path[0] ? g_current_path : "/");
    gfx_fill((unsigned)(wx + 60), (unsigned)wy,
             WIN_W - 70, TITLE_H, C_TITLEBAR);
    gfx_text_center((unsigned)wx, WIN_W, (unsigned)(wy + 10),
                    title, C_TEXT, C_TITLEBAR);
}

static void draw_frame(void)
{
    int wx = g_win_x, wy = g_win_y;
    gfx_fill((unsigned)(wx + 4), (unsigned)(wy + 4), WIN_W, WIN_H,
             GFX_RGB(6, 6, 10));
    gfx_fill((unsigned)wx, (unsigned)wy, WIN_W, WIN_H, C_WIN_BG);
    gfx_fill((unsigned)wx, (unsigned)wy, WIN_W, TITLE_H, C_TITLEBAR);
    gfx_draw_close_button((unsigned)(wx + 10), (unsigned)(wy + 8), 0);
    draw_title_text();
    gfx_hline((unsigned)wx, (unsigned)(wy + TITLE_H), WIN_W, C_ACCENT);
    gfx_rect((unsigned)wx, (unsigned)wy, WIN_W, WIN_H, C_SEP);
}

static void on_reposition(void *ud)
{
    (void)ud;
    draw_frame();
}

/* ── Build widget tree ────────────────────────────────────────────────── */

static void build_ui(void)
{
    widget_init_panel(&g_root, 0, 0, WIN_W, CONT_H, C_WIN_BG);

    /* Left: tree pane */
    treeview_init(&g_tree, 0, 0, TREE_W, PANE_H, &g_tv_data);
    treeview_set_callbacks(&g_tree, tree_on_select, tree_on_expand, NULL);

    /* 1-px divider */
    widget_init_panel(&g_divider, TREE_W, 0, DIV_W, PANE_H, C_SEP);

    /* Right: icon grid panel with custom draw + event */
    widget_init_panel(&g_right, TREE_W + DIV_W, 0, RIGHT_W, PANE_H, C_WIN_BG);
    g_right.draw_fn   = right_panel_draw;
    g_right.event_fn  = right_panel_event;
    g_right.focusable = 1;

    /* Status bar */
    widget_init_label(&g_statusbar, 0, PANE_H, WIN_W, STATUS_H,
                      "", WGT_ALIGN_LEFT);

    widget_add_child(&g_root, &g_tree);
    widget_add_child(&g_root, &g_divider);
    widget_add_child(&g_root, &g_right);
    widget_add_child(&g_root, &g_statusbar);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(int argc, const char *const *argv)
{
    (void)argc;
    (void)argv;

    gfx_init();

    g_selected_entry = -1;
    g_last_click_entry = -1;

    g_win_id = sys_wm_register(g_win_x, g_win_y, WIN_W, WIN_H, "Files");
    sys_wm_focus_set(sys_getpid());

    draw_frame();
    build_ui();

    /* Probe VFS mounts and populate the tree */
    vfs_probe_mounts();
    mount_info_t mounts[4];
    int nm = vfs_get_mounts(mounts, 4);

    for (int i = 0; i < nm; i++) {
        char *mp = alloc_tree_path(mounts[i].path);
        treeview_add_root(&g_tree, mounts[i].label, mounts[i].icon_type, mp);
    }
    treeview_rebuild_visible(&g_tree);

    /* Navigate to root of first available mount */
    if (nm > 0) {
        strncpy(g_current_path, mounts[0].path, 127);
        g_current_path[127] = '\0';
        set_fs_label(g_current_path);
        files_navigate_to(g_current_path);
    } else {
        update_statusbar();
    }

    g_ctx.win_x         = &g_win_x;
    g_ctx.win_y         = &g_win_y;
    g_ctx.content_dx    = 0;
    g_ctx.content_dy    = TITLE_H;
    g_ctx.on_reposition = on_reposition;
    g_ctx.userdata      = NULL;
    g_ctx.running       = 1;

    widget_run(&g_root, &g_ctx);

    sys_wm_unregister(g_win_id);
    return 0;
}
