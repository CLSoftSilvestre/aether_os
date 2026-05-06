/*
 * AetherOS — Files App  (Phase 6.1 — Glassmorphism Redesign)
 * File: userspace/apps/files/main.c
 *
 * Two-pane file explorer with Lumina glassmorphism UI:
 *   Titlebar (32px)  — frosted-glass gradient with specular highlight
 *   Toolbar  (36px)  — nav buttons (< Back, ^ Up, ~ Home) + breadcrumb path
 *   Left  (220px)    — "DRIVES" header + treeview on glass sidebar
 *   Divider (2px)    — accent glow separator
 *   Right (678px)    — icon grid; 80×88 px cells, glass selection highlight
 *   Status (26px)    — glass status bar with item count
 *
 * Navigation: Back history (8 levels), Up, Home.
 * Double-click: folder → navigate in; .txt → textviewer; .as → aether_editor.
 * Keyboard (right pane): arrows, Enter, Backspace (up), PgUp/PgDn.
 */

#include <gfx.h>
#include <gpu.h>
#include <sys.h>
#include <widget.h>
#include <vfs_mounts.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <input.h>

/* ── Window geometry ──────────────────────────────────────────────────── */

#define WIN_W         900
#define WIN_H         636
#define TITLE_H        32   /* glass titlebar */
#define CONT_H        (WIN_H - TITLE_H)    /* 604 — content including toolbar */
#define TOOLBAR_H      36   /* nav toolbar */
#define STATUS_H       26   /* status bar */
#define PANE_H        (CONT_H - TOOLBAR_H - STATUS_H)  /* 542 */

#define SIDEBAR_HDR_H  20   /* "DRIVES" header above tree */
#define TREE_W        220
#define TREE_PANE_H   (PANE_H - SIDEBAR_HDR_H)  /* 522 */
#define DIV_W           2   /* accent glow pane divider */
#define RIGHT_W       (WIN_W - TREE_W - DIV_W)  /* 678 */

/* Icon grid cell */
#define CELL_W         80
#define CELL_H         88
#define ICON_SZ        48
#define ICON_OFF_X     ((CELL_W - ICON_SZ) / 2)   /* 16 */
#define ICON_OFF_Y      4
#define CELLS_PER_ROW  (RIGHT_W / CELL_W)          /* 8 */

/* Toolbar button layout (x-offsets within toolbar widget) */
#define TBTN_BACK_X     8
#define TBTN_BACK_W    56   /* "< Back" */
#define TBTN_UP_X      70
#define TBTN_UP_W      40   /* " ^ Up " */
#define TBTN_HOME_X   116
#define TBTN_HOME_W    56   /* "~ Home" */
#define TBTN_SEP_X    178
#define TBTN_PATH_X   190   /* breadcrumb display start */
#define TBTN_H         24   /* button height */

/* Double-click: 50 × 10 ms ticks = 500 ms */
#define DCLICK_TICKS   50

/* Back history depth */
#define BHIST_MAX       8

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

/* ── Glassmorphism color palette ──────────────────────────────────────── */
/*
 * These are computed as lerp(base_color, C_ACCENT, alpha) using integer math.
 * They simulate frosted glass: dark background tinted with the purple accent.
 */
#define C_GLASS_TITLE    GFX_RGB( 42,  38,  86)  /* titlebar glass fill       */
#define C_GLASS_SPEC     GFX_RGB(190, 170, 255)  /* top specular line (1px)   */
#define C_GLASS_HIGH     GFX_RGB( 82,  70, 158)  /* soft highlight band (2px) */
#define C_GLASS_EDGE     GFX_RGB(  8,   6,  16)  /* bottom shadow edge        */
#define C_GLASS_TOOLBAR  GFX_RGB( 24,  22,  46)  /* toolbar glass             */
#define C_GLASS_SIDEBAR  GFX_RGB( 15,  13,  27)  /* tree sidebar (darker)     */
#define C_GLASS_STATUS   GFX_RGB( 20,  18,  36)  /* status bar glass          */
#define C_GLASS_SEL      GFX_RGB( 55,  46, 112)  /* icon grid selection fill  */
#define C_BTN_BG         GFX_RGB( 36,  32,  70)  /* toolbar button bg         */
#define C_BTN_DISABLED   GFX_RGB( 26,  24,  48)  /* disabled button bg        */
#define C_GLOW_DIV       GFX_RGB( 68,  56, 132)  /* pane divider glow         */
#define C_GRID_LINE      GFX_RGB( 26,  24,  44)  /* subtle icon grid lines    */
#define C_SIDEBAR_HDR    GFX_RGB( 18,  16,  32)  /* sidebar section header    */

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
static char g_first_mount[128];     /* target for Home button */

/* Back navigation history */
static char g_bhist[BHIST_MAX][128];
static int  g_bhist_top;

/* Double-click tracking */
static long g_last_click_tick;
static int  g_last_click_entry;

/* Absolute origin of each panel (set during draw, used in events) */
static int g_right_ax,   g_right_ay;
static int g_toolbar_ax, g_toolbar_ay;

/* ── Window state ─────────────────────────────────────────────────────── */

static int  g_win_x = 50;
static int  g_win_y = 50;
static long g_win_id = -1;

static widget_ctx_t g_ctx;

/* ── Widget tree ──────────────────────────────────────────────────────── */

static widget_t g_root;
static widget_t g_toolbar;
static widget_t g_sidebar_hdr;
static widget_t g_tree;
static widget_t g_divider;
static widget_t g_right;
static widget_t g_statusbar;

/* ── Forward declarations ─────────────────────────────────────────────── */

static void files_navigate_core(const char *path);
static void files_navigate_to(const char *path);
static void draw_frame(void);
static void draw_title_text(void);

/* ── Path pool allocator ──────────────────────────────────────────────── */

static char *alloc_tree_path(const char *src)
{
    if (g_path_pool_idx >= PATH_POOL_SIZE) return (void *)0;
    char *slot = g_path_pool[g_path_pool_idx++];
    int i = 0;
    while (i < PATH_POOL_LEN - 1 && src[i]) { slot[i] = src[i]; i++; }
    slot[i] = '\0';
    return slot;
}

/* ── FS label from path prefix ────────────────────────────────────────── */

static void set_fs_label(const char *path)
{
    if (path[0]=='/' && path[1]=='a' && path[2]=='f' && path[3]=='s')
        strncpy(g_current_fs_label, "AetherFS", 31);
    else if (path[0]=='/' && path[1]=='i')
        strncpy(g_current_fs_label, "InitRD", 31);
    else
        strncpy(g_current_fs_label, "FAT32 (/)", 31);
    g_current_fs_label[31] = '\0';
}

/* ── Extension-based icon type ────────────────────────────────────────── */

static unsigned char detect_icon(const char *name)
{
    int n = (int)strlen(name);
    if (n>=4 && name[n-4]=='.' && (name[n-3]=='t' || name[n-3]=='T') && (name[n-2]=='x' || name[n-2]=='X') && (name[n-1]=='t' || name[n-1]=='T'))
        return FICON_TXT;
    if (n>=3 && name[n-3]=='.' && (name[n-2]=='a' || name[n-2]=='A') &&
        (name[n-1]=='s' || name[n-1]=='S'))
        return FICON_AS;
    if (n>=4 && name[n-4]=='.' && name[n-3]=='e' && name[n-2]=='l' && name[n-1]=='f')
        return FICON_EXEC;
    if (n>=4 && name[n-4]=='.' && (name[n-3]=='a' || name[n-3]=='A') && (name[n-2]=='p' || name[n-2]=='P') && (name[n-1]=='p' || name[n-1]=='P'))
        return FICON_EXEC;
    return FICON_GENERIC;
}

/* ── Status bar text ──────────────────────────────────────────────────── */

static void update_statusbar(void)
{
    int dirs = 0, files = 0;
    for (int i = 0; i < g_entry_count; i++) {
        if (g_entries[i].is_dir) dirs++; else files++;
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "  %d folder%s, %d file%s    %s",
             dirs,  dirs  == 1 ? "" : "s",
             files, files == 1 ? "" : "s",
             g_current_fs_label);
    /* Store text in label data; statusbar_draw will render it */
    int i = 0;
    while (msg[i] && i < 255) { g_statusbar.data.label.text[i] = msg[i]; i++; }
    g_statusbar.data.label.text[i] = '\0';
    g_statusbar.dirty = 1;
}

/* ── Glass drawing helpers ────────────────────────────────────────────── */

/*
 * Draw a frosted-glass horizontal strip: base fill + specular top line +
 * soft highlight band + shadow bottom line.
 */
static void draw_glass_strip(int x, int y, int w, int h, unsigned base)
{
    gfx_fill((unsigned)x, (unsigned)y,   (unsigned)w, (unsigned)h, base);
    gfx_hline((unsigned)x, (unsigned)y,                (unsigned)w, C_GLASS_SPEC);
    gfx_fill((unsigned)x, (unsigned)(y+1),(unsigned)w, 2u,          C_GLASS_HIGH);
    gfx_hline((unsigned)x, (unsigned)(y+h-1),(unsigned)w, C_GLASS_EDGE);
}

/*
 * Draw a glass-style toolbar button with text centered inside.
 * enabled=0: dimmed appearance.
 */
static void draw_glass_btn(int x, int y, int w, int h,
                            const char *label, int enabled)
{
    unsigned bg = enabled ? C_BTN_BG : C_BTN_DISABLED;
    unsigned fg = enabled ? C_TEXT   : C_TEXT_DIM;
    gfx_fill((unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h, bg);
    gfx_hline((unsigned)x, (unsigned)y, (unsigned)w, C_GLASS_HIGH); /* top glow */
    gfx_rect((unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h,
              enabled ? GFX_RGB(55,48,100) : GFX_RGB(35,32,58));
    int lw = (int)strlen(label) * 8;
    int tx = x + (w - lw) / 2;
    int ty = y + (h - 8) / 2;
    gfx_text((unsigned)tx, (unsigned)ty, label, fg, bg);
}

/*
 * Draw breadcrumb path in the toolbar.
 * Prefix up to last '/' in C_TEXT_DIM, final segment in C_TEXT.
 * Truncates from left with "... " if too long.
 */
static void draw_breadcrumb(int ax, int ay, int max_w)
{
    const char *path = g_current_path[0] ? g_current_path : "/";
    unsigned bg      = C_GLASS_TOOLBAR;
    int max_chars    = (max_w - 4) / 8;
    if (max_chars < 1) return;
    int pathlen      = (int)strlen(path);

    char disp[132];
    int  displen;
    if (pathlen <= max_chars) {
        for (int i = 0; i <= pathlen; i++) disp[i] = path[i];
        displen = pathlen;
    } else {
        int show  = max_chars - 4;
        if (show < 1) show = 1;
        int start = pathlen - show;
        disp[0]='.'; disp[1]='.'; disp[2]='.'; disp[3]=' ';
        for (int i = 0; i < show; i++) disp[4+i] = path[start+i];
        disp[4+show] = '\0';
        displen = 4 + show;
    }

    /* Find last '/' to split into dim prefix + bright last segment */
    int last_slash = -1;
    for (int i = displen-1; i >= 0; i--) {
        if (disp[i] == '/') { last_slash = i; break; }
    }

    if (last_slash >= 0 && last_slash < displen-1) {
        char prefix[132];
        for (int i = 0; i <= last_slash; i++) prefix[i] = disp[i];
        prefix[last_slash+1] = '\0';
        gfx_text((unsigned)ax, (unsigned)ay, prefix, C_TEXT_DIM, bg);
        int px = ax + (last_slash+1) * 8;
        gfx_text((unsigned)px, (unsigned)ay, disp+last_slash+1, C_TEXT, bg);
    } else {
        gfx_text((unsigned)ax, (unsigned)ay, disp, C_TEXT, bg);
    }
}

/* ── Navigate parent ──────────────────────────────────────────────────── */

static void navigate_parent(void)
{
    char parent[128];
    strncpy(parent, g_current_path, 127);
    parent[127] = '\0';
    int len = (int)strlen(parent);
    if (len <= 1) return;
    if (parent[len-1] == '/') { parent[--len] = '\0'; }
    while (len > 0 && parent[len-1] != '/') len--;
    if (len > 1) len--;
    parent[len > 0 ? len : 1] = '\0';
    if (parent[0] == '\0') { parent[0]='/'; parent[1]='\0'; }
    files_navigate_to(parent);
}

/* ── Back history ─────────────────────────────────────────────────────── */

static void bhist_push(const char *path)
{
    if (g_bhist_top < BHIST_MAX) {
        strncpy(g_bhist[g_bhist_top], path, 127);
        g_bhist[g_bhist_top][127] = '\0';
        g_bhist_top++;
    } else {
        /* Shift out oldest entry */
        for (int i = 0; i < BHIST_MAX-1; i++)
            strncpy(g_bhist[i], g_bhist[i+1], 128);
        strncpy(g_bhist[BHIST_MAX-1], path, 127);
        g_bhist[BHIST_MAX-1][127] = '\0';
    }
}

static void navigate_back(void)
{
    if (g_bhist_top <= 0) return;
    g_bhist_top--;
    files_navigate_core(g_bhist[g_bhist_top]);
    widget_invalidate(&g_toolbar);
}

/* ── Activate an entry (double-click or Enter) ────────────────────────── */

static void activate_entry(int idx)
{
    if (idx < 0 || idx >= g_entry_count) return;
    file_entry_t *e = &g_entries[idx];
    if (e->is_dir) { files_navigate_to(e->path); return; }
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
    default: break;
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
    g_tv_data.selected = -1;
    widget_invalidate(&g_tree);
}

/* ── Core navigate (no history push) ─────────────────────────────────── */

static void files_navigate_core(const char *path)
{
    strncpy(g_current_path, path, 127);
    g_current_path[127] = '\0';
    set_fs_label(path);

    g_entry_count    = 0;
    g_grid_scroll_y  = 0;
    g_selected_entry = -1;

    static char dir_buf[8192];
    long n = sys_fs_readdir(path, dir_buf, sizeof(dir_buf)-1);
    if (n > 0) {
        dir_buf[n] = '\0';
        char *s = dir_buf;
        while (*s && g_entry_count < MAX_ENTRIES) {
            char *end = s;
            while (*end && *end != '\n') end++;

            if (s < end) {
                file_entry_t *e = &g_entries[g_entry_count];
                if (*s == '[') {
                    char *close = s+1;
                    while (*close && *close != ']') close++;
                    int len = (int)(close-(s+1));
                    if (len > 0 && len < 64) {
                        int is_dot = (len==1 && s[1]=='.') ||
                                     (len==2 && s[1]=='.' && s[2]=='.');
                        if (!is_dot) {
                            int j;
                            for (j=0; j<len; j++) e->name[j] = s[1+j];
                            e->name[j] = '\0';
                            if (path[0]=='/' && path[1]=='\0')
                                snprintf(e->path, 128, "/%s", e->name);
                            else
                                snprintf(e->path, 128, "%s/%s", path, e->name);
                            e->icon_type = FICON_FOLDER;
                            e->is_dir    = 1;
                            g_entry_count++;
                        }
                    }
                } else {
                    int j = 0;
                    char *p = s;
                    while (p < end && *p != ' ' && j < 63) e->name[j++] = *p++;
                    e->name[j] = '\0';
                    if (e->name[0] && !(e->name[0]=='.' &&
                        (e->name[1]=='\0' || (e->name[1]=='.' && e->name[2]=='\0')))) {
                        if (path[0]=='/' && path[1]=='\0')
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
            s = end+1;
        }
    }

    draw_title_text();
    update_statusbar();
    tree_sync_selection(path);
    widget_invalidate(&g_right);
    widget_invalidate(&g_toolbar);
}

/* ── Navigate (with history push) ────────────────────────────────────── */

static void files_navigate_to(const char *path)
{
    if (g_current_path[0] && strcmp(g_current_path, path) != 0)
        bhist_push(g_current_path);
    files_navigate_core(path);
}

/* ── Toolbar custom draw ──────────────────────────────────────────────── */

static void toolbar_draw(widget_t *w, int ax, int ay)
{
    g_toolbar_ax = ax;
    g_toolbar_ay = ay;
    int pw = w->bounds.w;
    int ph = w->bounds.h;

    draw_glass_strip(ax, ay, pw, ph, C_GLASS_TOOLBAR);

    /* Bottom separator */
    gfx_hline((unsigned)ax, (unsigned)(ay+ph-1), (unsigned)pw, C_SEP);

    /* Nav buttons */
    int by = ay + (ph - TBTN_H) / 2;
    draw_glass_btn(ax+TBTN_BACK_X, by, TBTN_BACK_W, TBTN_H,
                   "< Back", g_bhist_top > 0);
    draw_glass_btn(ax+TBTN_UP_X,   by, TBTN_UP_W,   TBTN_H, "^ Up",   1);
    draw_glass_btn(ax+TBTN_HOME_X, by, TBTN_HOME_W, TBTN_H, "~ Home", 1);

    /* Vertical separator before breadcrumb */
    gfx_vline((unsigned)(ax+TBTN_SEP_X), (unsigned)(ay+4),
              (unsigned)(ph-8), C_SEP);

    /* Breadcrumb */
    int bcy = ay + (ph - 8) / 2;
    draw_breadcrumb(ax+TBTN_PATH_X, bcy, pw-TBTN_PATH_X-8);
}

static int toolbar_event(widget_t *w, const widget_event_t *ev)
{
    (void)w;
    if (ev->type != WEV_MOUSE_DOWN) return 0;
    int rx = ev->mx - g_toolbar_ax;
    int ry = ev->my - g_toolbar_ay;
    int by = (TOOLBAR_H - TBTN_H) / 2;
    if (ry < by || ry >= by + TBTN_H) return 0;

    if (rx >= TBTN_BACK_X && rx < TBTN_BACK_X+TBTN_BACK_W) {
        navigate_back(); return 1;
    }
    if (rx >= TBTN_UP_X && rx < TBTN_UP_X+TBTN_UP_W) {
        navigate_parent(); return 1;
    }
    if (rx >= TBTN_HOME_X && rx < TBTN_HOME_X+TBTN_HOME_W) {
        if (g_first_mount[0]) files_navigate_to(g_first_mount);
        return 1;
    }
    return 0;
}

/* ── Sidebar header draw ──────────────────────────────────────────────── */

static void sidebar_hdr_draw(widget_t *w, int ax, int ay)
{
    int pw = w->bounds.w;
    int ph = w->bounds.h;
    gfx_fill((unsigned)ax, (unsigned)ay, (unsigned)pw, (unsigned)ph, C_SIDEBAR_HDR);
    /* Bottom separator */
    gfx_hline((unsigned)ax, (unsigned)(ay+ph-1), (unsigned)pw, C_SEP);
    /* Label "DRIVES" — uppercase section title */
    int ty = ay + (ph-8)/2;
    gfx_text((unsigned)(ax+14), (unsigned)ty, "DRIVES", C_TEXT_DIM, C_SIDEBAR_HDR);
}

/* ── Status bar custom draw ───────────────────────────────────────────── */

static void statusbar_draw(widget_t *w, int ax, int ay)
{
    int pw = w->bounds.w;
    int ph = w->bounds.h;

    gfx_fill((unsigned)ax, (unsigned)ay, (unsigned)pw, (unsigned)ph, C_GLASS_STATUS);
    /* Top glow */
    gfx_hline((unsigned)ax, (unsigned)ay,   (unsigned)pw, C_GLASS_HIGH);
    gfx_hline((unsigned)ax, (unsigned)(ay+1),(unsigned)pw, GFX_RGB(35,30,65));

    int ty = ay + (ph-8)/2;
    gfx_text((unsigned)(ax+10), (unsigned)ty,
             w->data.label.text, C_TEXT_DIM, C_GLASS_STATUS);

    /* Right side: FS label in accent */
    int fslen = (int)strlen(g_current_fs_label);
    int fx = ax + pw - fslen*8 - 14;
    if (fx > ax + pw/2)
        gfx_text((unsigned)fx, (unsigned)ty,
                 g_current_fs_label, C_ACCENT2, C_GLASS_STATUS);
}

/* ── Right panel draw ─────────────────────────────────────────────────── */

static void right_panel_draw(widget_t *w, int ax, int ay)
{
    g_right_ax = ax;
    g_right_ay = ay;
    int pw = w->bounds.w;
    int ph = w->bounds.h;

    gfx_fill((unsigned)ax, (unsigned)ay, (unsigned)pw, (unsigned)ph, C_WIN_BG);

    /* Subtle column separators */
    for (int col = 1; col < CELLS_PER_ROW; col++) {
        int gx = ax + col * CELL_W;
        if (gx < ax + pw)
            gfx_vline((unsigned)gx, (unsigned)ay, (unsigned)ph, C_GRID_LINE);
    }

    /* Subtle row separators */
    int total_rows = (g_entry_count + CELLS_PER_ROW-1) / CELLS_PER_ROW;
    for (int row = 0; row <= total_rows; row++) {
        int gy = ay + row*CELL_H - g_grid_scroll_y;
        if (gy >= ay && gy < ay+ph)
            gfx_hline((unsigned)ax, (unsigned)gy, (unsigned)pw, C_GRID_LINE);
    }

    for (int i = 0; i < g_entry_count; i++) {
        int row = i / CELLS_PER_ROW;
        int col = i % CELLS_PER_ROW;
        int cx  = ax + col * CELL_W;
        int cy  = ay + row * CELL_H - g_grid_scroll_y;

        if (cy + CELL_H <= ay || cy >= ay + ph) continue;

        /* Glass selection highlight */
        if (i == g_selected_entry) {
            gfx_fill((unsigned)cx, (unsigned)cy, CELL_W, CELL_H, C_GLASS_SEL);
            gfx_hline((unsigned)cx, (unsigned)cy, CELL_W, C_GLASS_SPEC);
            gfx_fill((unsigned)cx, (unsigned)(cy+1), CELL_W, 2u, C_GLASS_HIGH);
            gfx_rect((unsigned)cx, (unsigned)cy, CELL_W, CELL_H, C_ACCENT);
        }

        /* Icon */
        int icon_x = cx + ICON_OFF_X;
        int icon_y = cy + ICON_OFF_Y;
        switch (g_entries[i].icon_type) {
        case FICON_FOLDER:  gfx_icon_folder(icon_x, icon_y, ICON_SZ);       break;
        case FICON_TXT:     gfx_icon_file_txt(icon_x, icon_y, ICON_SZ);     break;
        case FICON_AS:      gfx_icon_file_as(icon_x, icon_y, ICON_SZ);      break;
        case FICON_EXEC:    gfx_icon_file_exec(icon_x, icon_y, ICON_SZ);    break;
        default:            gfx_icon_file_generic(icon_x, icon_y, ICON_SZ); break;
        }

        /* Label (up to 2 lines, 10 chars each) */
        unsigned lbg = (i == g_selected_entry) ? C_GLASS_SEL : C_WIN_BG;
        int max_chars = CELL_W / 8;
        char line1[12], line2[12];
        int nlen = (int)strlen(g_entries[i].name);

        if (nlen <= max_chars) {
            int j; for (j=0; j<nlen; j++) line1[j] = g_entries[i].name[j];
            line1[j]='\0'; line2[0]='\0';
        } else {
            int j; for (j=0; j<max_chars; j++) line1[j] = g_entries[i].name[j];
            line1[j]='\0';
            int rem = nlen-max_chars; if (rem>max_chars) rem=max_chars;
            for (j=0; j<rem; j++) line2[j] = g_entries[i].name[max_chars+j];
            line2[j]='\0';
        }

        int lx1 = cx + (CELL_W-(int)strlen(line1)*8)/2;
        gfx_text((unsigned)lx1, (unsigned)(cy+56), line1, C_TEXT, lbg);
        if (line2[0]) {
            int lx2 = cx + (CELL_W-(int)strlen(line2)*8)/2;
            gfx_text((unsigned)lx2, (unsigned)(cy+66), line2, C_TEXT_DIM, lbg);
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

        int idx = row*CELLS_PER_ROW + col;
        if (idx < 0 || idx >= g_entry_count) {
            g_selected_entry = -1;
            widget_invalidate(w);
            return 1;
        }

        long now   = sys_get_ticks();
        int  is_dc = (idx == g_last_click_entry &&
                      now - g_last_click_tick <= DCLICK_TICKS);
        g_last_click_tick  = now;
        g_last_click_entry = idx;

        g_selected_entry = idx;
        widget_invalidate(w);
        if (is_dc) activate_entry(idx);
        return 1;
    }

    if (ev->type == WEV_KEY_DOWN) {
        switch (ev->keycode) {
        case KEY_LEFT:
            if (g_selected_entry > 0) { g_selected_entry--; widget_invalidate(w); }
            return 1;
        case KEY_RIGHT:
            if (g_selected_entry < g_entry_count-1) { g_selected_entry++; widget_invalidate(w); }
            return 1;
        case KEY_UP:
            if (g_selected_entry >= CELLS_PER_ROW) {
                g_selected_entry -= CELLS_PER_ROW;
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
            if (g_entry_count > 0 && g_selected_entry+CELLS_PER_ROW < g_entry_count) {
                g_selected_entry += CELLS_PER_ROW;
                int sel_row = g_selected_entry / CELLS_PER_ROW;
                int bot_row = (g_grid_scroll_y + PANE_H) / CELL_H;
                if (sel_row >= bot_row) g_grid_scroll_y += CELL_H;
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
            int total_rows = (g_entry_count+CELLS_PER_ROW-1)/CELLS_PER_ROW;
            int max_scroll = total_rows*CELL_H - PANE_H;
            g_grid_scroll_y += PANE_H;
            if (max_scroll > 0 && g_grid_scroll_y > max_scroll)
                g_grid_scroll_y = max_scroll;
            widget_invalidate(w);
            return 1;
        }
        default: break;
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
    long n = sys_fs_readdir(path, dir_buf, sizeof(dir_buf)-1);
    if (n <= 0) return;
    dir_buf[n] = '\0';

    char *s = dir_buf;
    while (*s) {
        char *end = s;
        while (*end && *end != '\n') end++;
        if (*s=='[' && s<end) {
            char *close = s+1;
            while (*close && *close!=']') close++;
            int len = (int)(close-(s+1));
            if (len > 0 && len < 64) {
                int is_dot = (len==1 && s[1]=='.') ||
                             (len==2 && s[1]=='.' && s[2]=='.');
                if (!is_dot) {
                    char dirname[64];
                    int j; for (j=0; j<len; j++) dirname[j]=s[1+j]; dirname[j]='\0';
                    char full[128];
                    if (path[0]=='/' && path[1]=='\0')
                        snprintf(full, sizeof(full), "/%s", dirname);
                    else
                        snprintf(full, sizeof(full), "%s/%s", path, dirname);
                    char *child_path = alloc_tree_path(full);
                    if (child_path)
                        treeview_add_child(&g_tree, node_idx, dirname,
                                           TVICON_FOLDER_CLOSED, 1, child_path);
                }
            }
        }
        if (!*end) break;
        s = end+1;
    }
}

/* ── Window frame ─────────────────────────────────────────────────────── */

static void draw_title_text(void)
{
    int wx = g_win_x, wy = g_win_y;
    char title[96];
    snprintf(title, sizeof(title), "Files  —  %s",
             g_current_path[0] ? g_current_path : "/");

    /* Refresh only the text area, preserving the glass gradient */
    gfx_fill((unsigned)(wx+36), (unsigned)(wy+3),
              WIN_W-72, TITLE_H-6, C_GLASS_TITLE);
    gfx_text_center((unsigned)wx, WIN_W,
                    (unsigned)(wy + (TITLE_H-8)/2),
                    title, C_TEXT, C_GLASS_TITLE);
}

static void draw_frame(void)
{
    int wx = g_win_x, wy = g_win_y;

    /* Drop shadow */
    gfx_fill((unsigned)(wx+4), (unsigned)(wy+4), WIN_W+2, WIN_H+2, GFX_RGB(4,4,8));

    /* Window base */
    gfx_fill((unsigned)wx, (unsigned)wy, WIN_W, WIN_H, C_WIN_BG);

    /* ── Glass titlebar gradient ──────────────────────────────────────── */
    gfx_fill((unsigned)wx, (unsigned)wy, WIN_W, TITLE_H, C_GLASS_TITLE);
    /* Top specular (1px) */
    gfx_hline((unsigned)wx, (unsigned)wy, WIN_W, C_GLASS_SPEC);
    /* Soft highlight band (px 1–2) */
    gfx_fill((unsigned)wx, (unsigned)(wy+1), WIN_W, 2u, C_GLASS_HIGH);
    /* Bottom shadow edge */
    gfx_hline((unsigned)wx, (unsigned)(wy+TITLE_H-1), WIN_W, C_GLASS_EDGE);

    /* Traffic light — vertically centred */
    gfx_draw_close_button((unsigned)(wx+10), (unsigned)(wy+(TITLE_H-12)/2), 0);

    /* Title text */
    draw_title_text();

    /* Accent underline below titlebar */
    gfx_hline((unsigned)wx, (unsigned)(wy+TITLE_H), WIN_W, C_ACCENT);

    /* Outer border */
    gfx_rect((unsigned)wx, (unsigned)wy, WIN_W, WIN_H, C_SEP);
    /* Inner top highlight */
    gfx_hline((unsigned)(wx+1), (unsigned)(wy+1), WIN_W-2, C_GLASS_HIGH);
}

static void on_reposition(void *ud)
{
    (void)ud;
    draw_frame();
}

/* ── Build widget tree ────────────────────────────────────────────────── */

static void build_ui(void)
{
    /* Root covers the full content area below the titlebar */
    widget_init_panel(&g_root, 0, 0, WIN_W, CONT_H, C_WIN_BG);

    /* Toolbar — spans full width at top of content */
    widget_init_panel(&g_toolbar, 0, 0, WIN_W, TOOLBAR_H, C_GLASS_TOOLBAR);
    g_toolbar.draw_fn  = toolbar_draw;
    g_toolbar.event_fn = toolbar_event;

    /* Sidebar section header */
    widget_init_panel(&g_sidebar_hdr,
                      0, TOOLBAR_H, TREE_W, SIDEBAR_HDR_H, C_SIDEBAR_HDR);
    g_sidebar_hdr.draw_fn = sidebar_hdr_draw;

    /* Tree pane — below sidebar header */
    treeview_init(&g_tree, 0, TOOLBAR_H+SIDEBAR_HDR_H, TREE_W, TREE_PANE_H,
                  &g_tv_data);
    g_tv_data.bg_color = C_GLASS_SIDEBAR;
    treeview_set_callbacks(&g_tree, tree_on_select, tree_on_expand, (void *)0);

    /* 2-px accent glow divider — spans full pane height */
    widget_init_panel(&g_divider, TREE_W, TOOLBAR_H, DIV_W, PANE_H, C_GLOW_DIV);

    /* Right icon grid panel */
    widget_init_panel(&g_right, TREE_W+DIV_W, TOOLBAR_H, RIGHT_W, PANE_H, C_WIN_BG);
    g_right.draw_fn  = right_panel_draw;
    g_right.event_fn = right_panel_event;
    g_right.focusable = 1;

    /* Status bar — glass with custom draw */
    widget_init_label(&g_statusbar, 0, TOOLBAR_H+PANE_H, WIN_W, STATUS_H,
                      "", WGT_ALIGN_LEFT);
    g_statusbar.draw_fn = statusbar_draw;

    widget_add_child(&g_root, &g_toolbar);
    widget_add_child(&g_root, &g_sidebar_hdr);
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
    gpu_init((void *)0);   /* probe GPU; software fallback if absent */

    g_selected_entry  = -1;
    g_last_click_entry = -1;
    g_bhist_top       = 0;
    g_first_mount[0]  = '\0';

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
        /* Remember first mount for Home button */
        if (i == 0 && mp) {
            strncpy(g_first_mount, mounts[i].path, 127);
            g_first_mount[127] = '\0';
        }
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
    g_ctx.userdata      = (void *)0;
    g_ctx.running       = 1;

    widget_run(&g_root, &g_ctx);

    sys_wm_unregister(g_win_id);
    return 0;
}
