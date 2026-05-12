/*
 * AetherOS libwidget — WIDGET_TREEVIEW (Phase 5.6)
 * File: userspace/lib/libwidget/treeview.c
 *
 * Collapsible directory tree widget backed by a caller-owned treeview_data_t.
 * The node pool is fixed at 128 entries; no dynamic allocation.
 *
 * Layout per row (20 px tall):
 *   [depth × 14 px indent] [10 px expand triangle] [2 px gap]
 *   [14×14 icon] [2 px gap] [label clipped to right edge]
 *
 * Lazy-load contract:
 *   When a node is expanded for the first time, on_expand fires.
 *   The callback must call treeview_add_child() for each sub-entry.
 *   On collapse, treeview_remove_children() removes those entries so the pool
 *   slot is reused on the next expand.
 */

#include <widget.h>
#include <gfx.h>
#include <string.h>

#define TV_ROW_H    20
#define TV_INDENT   14
#define TV_TRI_W    10   /* expand triangle area width */
#define TV_ICON_W   14   /* icon cell width */
#define TV_ICON_H   14
#define TV_GAP       2

/* Selection background — dark purple at ~40% alpha over C_PANEL */
#define C_TV_SEL     GFX_RGB( 50,  42,  99)
#define C_TV_HOVER   GFX_RGB( 35,  33,  60)

/* ── DFS helper for treeview_rebuild_visible ──────────────────────────── */

static void rebuild_dfs(treeview_data_t *td, int idx)
{
    if (td->visible_count >= 128) return;
    td->visible[td->visible_count++] = idx;

    if (!td->nodes[idx].expanded) return;

    for (int i = 0; i < td->node_count; i++) {
        if (td->nodes[i].parent_idx == idx && td->nodes[i].label[0])
            rebuild_dfs(td, i);
    }
}

void treeview_rebuild_visible(widget_t *w)
{
    treeview_data_t *td = (treeview_data_t *)w->userdata;
    td->visible_count = 0;

    /* Add all root nodes in order */
    for (int i = 0; i < td->node_count; i++) {
        if (td->nodes[i].parent_idx == -1 && td->nodes[i].label[0])
            rebuild_dfs(td, i);
    }
    w->dirty = 1;
}

/* ── Draw ─────────────────────────────────────────────────────────────── */

static void treeview_draw(widget_t *w, int ax, int ay)
{
    treeview_data_t *td = (treeview_data_t *)w->userdata;

    /* Store absolute origin so event handler can hit-test rows */
    td->draw_ax = ax;
    td->draw_ay = ay;

    int pw = w->bounds.w;
    int ph = w->bounds.h;

    unsigned pane_bg = td->bg_color ? td->bg_color : C_PANEL;
    gfx_fill(ax, ay, pw, ph, pane_bg);

    int max_rows = ph / TV_ROW_H;

    for (int vi = td->scroll_offset; vi < td->visible_count; vi++) {
        int screen_row = vi - td->scroll_offset;
        if (screen_row >= max_rows) break;

        int row_y = ay + screen_row * TV_ROW_H;
        int ni    = td->visible[vi];
        tv_node_t *node = &td->nodes[ni];

        /* Selection / hover background */
        unsigned row_bg = pane_bg;
        if (vi == td->selected) {
            gfx_fill(ax, row_y, pw, TV_ROW_H, C_TV_SEL);
            row_bg = C_TV_SEL;
        }

        int cx = ax + node->depth * TV_INDENT;

        /* Expand / collapse triangle */
        if (node->has_children) {
            if (node->expanded) {
                /* ▼ filled triangle pointing down */
                gfx_fill(cx + 1, row_y + 7, 8, 1, C_TEXT_DIM);
                gfx_fill(cx + 2, row_y + 8, 6, 1, C_TEXT_DIM);
                gfx_fill(cx + 3, row_y + 9, 4, 1, C_TEXT_DIM);
                gfx_fill(cx + 4, row_y + 10, 2, 1, C_TEXT_DIM);
            } else {
                /* ▶ filled triangle pointing right */
                gfx_fill(cx + 3, row_y + 5,  2, 10, C_TEXT_DIM);
                gfx_fill(cx + 5, row_y + 6,  2,  8, C_TEXT_DIM);
                gfx_fill(cx + 7, row_y + 7,  2,  6, C_TEXT_DIM);
                gfx_fill(cx + 9, row_y + 8,  1,  4, C_TEXT_DIM);
            }
        }
        cx += TV_TRI_W + TV_GAP;

        /* Icon (14×14) */
        switch (node->icon_type) {
        case TVICON_DRIVE_FAT32:   gfx_icon_drive_fat32(cx, row_y + 2, TV_ICON_W);   break;
        case TVICON_DRIVE_INITRD:  gfx_icon_drive_initrd(cx, row_y + 2, TV_ICON_W);  break;
        case TVICON_DRIVE_AFS:     gfx_icon_drive_afs(cx, row_y + 2, TV_ICON_W);     break;
        case TVICON_FOLDER_OPEN:   gfx_icon_folder_open(cx, row_y + 2, TV_ICON_W);   break;
        case TVICON_FOLDER_CLOSED: gfx_icon_folder(cx, row_y + 2, TV_ICON_W);        break;
        default:                   gfx_icon_file_generic(cx, row_y + 2, TV_ICON_W);  break;
        }
        cx += TV_ICON_W + TV_GAP;

        /* Label — pixel-clipped to right edge of pane */
        int label_px = ax + pw - cx - 2;
        if (label_px > 0) {
            char clip[64];
            int j = 0;
            while (node->label[j] && j < 63 &&
                   gfx_text_prefix_width(node->label, j + 1) <= label_px) {
                clip[j] = node->label[j];
                j++;
            }
            clip[j] = '\0';
            unsigned fg = C_TEXT;
            gfx_text((unsigned)cx, (unsigned)(row_y + 6), clip, fg, row_bg);
        }
    }

    /* Right border (1 px separator) */
    gfx_vline((unsigned)(ax + pw - 1), (unsigned)ay, (unsigned)ph, C_SEP);
}

/* ── Event ────────────────────────────────────────────────────────────── */

static int treeview_event(widget_t *w, const widget_event_t *ev)
{
    treeview_data_t *td = (treeview_data_t *)w->userdata;
    int ax = td->draw_ax;
    int ay = td->draw_ay;

    if (ev->type == WEV_FOCUS_IN || ev->type == WEV_FOCUS_OUT) {
        w->dirty = 1;
        return 0;
    }

    if (ev->type == WEV_MOUSE_DOWN) {
        int rel_y = ev->my - ay;
        if (rel_y < 0) return 0;

        int row   = rel_y / TV_ROW_H;
        int vi    = td->scroll_offset + row;
        if (vi < 0 || vi >= td->visible_count) return 0;

        int ni    = td->visible[vi];
        tv_node_t *node = &td->nodes[ni];

        /* Detect click in triangle area */
        int tri_x_start = ax + node->depth * TV_INDENT;
        int tri_x_end   = tri_x_start + TV_TRI_W;
        int in_triangle = (ev->mx >= tri_x_start && ev->mx < tri_x_end);

        if (in_triangle && node->has_children) {
            int was_expanded = node->expanded;
            node->expanded = !node->expanded;

            if (node->expanded && !was_expanded) {
                /* Lazy load — fire on_expand; callback adds children */
                if (td->on_expand)
                    td->on_expand(node, td->cb_ctx);
            } else if (!node->expanded) {
                /* Collapse — remove all descendants */
                treeview_remove_children(w, ni);
                /* remove_children already calls rebuild_visible */
                td->selected = vi < td->visible_count ? vi : td->visible_count - 1;
                if (td->selected < 0) td->selected = 0;
                w->dirty = 1;
                return 1;
            }

            treeview_rebuild_visible(w);
            td->selected = vi;
            w->dirty = 1;
        } else {
            /* Select row */
            td->selected = vi;
            w->dirty = 1;
            if (td->on_select)
                td->on_select(node, td->cb_ctx);
        }
        return 1;
    }

    if (ev->type == WEV_KEY_DOWN) {
        int max_rows = w->bounds.h / TV_ROW_H;

        switch (ev->keycode) {
        case KEY_UP:
            if (td->selected > 0) {
                td->selected--;
                if (td->selected < td->scroll_offset)
                    td->scroll_offset = td->selected;
                w->dirty = 1;
                if (td->on_select && td->selected >= 0) {
                    int ni = td->visible[td->selected];
                    td->on_select(&td->nodes[ni], td->cb_ctx);
                }
            }
            return 1;

        case KEY_DOWN:
            if (td->selected < td->visible_count - 1) {
                td->selected++;
                if (td->selected >= td->scroll_offset + max_rows)
                    td->scroll_offset = td->selected - max_rows + 1;
                w->dirty = 1;
                if (td->on_select && td->selected >= 0) {
                    int ni = td->visible[td->selected];
                    td->on_select(&td->nodes[ni], td->cb_ctx);
                }
            }
            return 1;

        case KEY_RIGHT: {
            if (td->selected < 0 || td->selected >= td->visible_count) return 1;
            int ni = td->visible[td->selected];
            tv_node_t *node = &td->nodes[ni];
            if (node->has_children && !node->expanded) {
                node->expanded = 1;
                if (td->on_expand) td->on_expand(node, td->cb_ctx);
                treeview_rebuild_visible(w);
                w->dirty = 1;
            }
            return 1;
        }

        case KEY_LEFT: {
            if (td->selected < 0 || td->selected >= td->visible_count) return 1;
            int ni = td->visible[td->selected];
            tv_node_t *node = &td->nodes[ni];
            if (node->expanded) {
                node->expanded = 0;
                treeview_remove_children(w, ni);
                w->dirty = 1;
            } else if (node->parent_idx >= 0) {
                /* Jump to parent */
                for (int vi = 0; vi < td->visible_count; vi++) {
                    if (td->visible[vi] == node->parent_idx) {
                        td->selected = vi;
                        if (td->selected < td->scroll_offset)
                            td->scroll_offset = td->selected;
                        w->dirty = 1;
                        break;
                    }
                }
            }
            return 1;
        }

        default:
            break;
        }
    }

    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────── */

void treeview_init(widget_t *w, int x, int y, int width, int height,
                   treeview_data_t *data)
{
    widget_init(w, WIDGET_TREEVIEW, x, y, width, height);
    memset(data, 0, sizeof(*data));
    data->selected = -1;
    data->bg_color = C_PANEL;   /* default; caller may override before first draw */
    w->userdata  = data;
    w->draw_fn   = treeview_draw;
    w->event_fn  = treeview_event;
    w->focusable = 1;
}

int treeview_add_root(widget_t *w, const char *label, unsigned char icon,
                      void *userdata)
{
    treeview_data_t *td = (treeview_data_t *)w->userdata;
    if (td->node_count >= 128) return -1;

    int idx = td->node_count++;
    tv_node_t *n = &td->nodes[idx];
    memset(n, 0, sizeof(*n));

    int j = 0;
    while (j < 63 && label && label[j]) { n->label[j] = label[j]; j++; }
    n->label[j]     = '\0';
    n->icon_type    = icon;
    n->depth        = 0;
    n->has_children = 1;
    n->expanded     = 0;
    n->parent_idx   = -1;
    n->userdata     = userdata;

    return idx;
}

int treeview_add_child(widget_t *w, int parent_idx, const char *label,
                       unsigned char icon, int has_children, void *userdata)
{
    treeview_data_t *td = (treeview_data_t *)w->userdata;
    if (td->node_count >= 128) return -1;
    if (parent_idx < 0 || parent_idx >= td->node_count) return -1;

    int idx = td->node_count++;
    tv_node_t *n   = &td->nodes[idx];
    tv_node_t *par = &td->nodes[parent_idx];
    memset(n, 0, sizeof(*n));

    int j = 0;
    while (j < 63 && label && label[j]) { n->label[j] = label[j]; j++; }
    n->label[j]     = '\0';
    n->icon_type    = (has_children) ? TVICON_FOLDER_CLOSED : icon;
    n->depth        = (unsigned char)(par->depth + 1);
    n->has_children = (unsigned char)(has_children ? 1 : 0);
    n->expanded     = 0;
    n->parent_idx   = parent_idx;
    n->userdata     = userdata;

    return idx;
}

void treeview_remove_children(widget_t *w, int parent_idx)
{
    treeview_data_t *td = (treeview_data_t *)w->userdata;

    /* Multi-pass BFS mark: mark direct children, then their children, etc. */
    char remove[128];
    int  i;
    for (i = 0; i < 128; i++) remove[i] = 0;

    int changed = 1;
    while (changed) {
        changed = 0;
        for (i = 0; i < td->node_count; i++) {
            if (remove[i]) continue;
            int pidx = td->nodes[i].parent_idx;
            if (pidx == parent_idx || (pidx >= 0 && remove[pidx])) {
                remove[i] = 1;
                changed = 1;
            }
        }
    }

    /* Compact the pool and build a remap table */
    int remap[128];
    int write = 0;
    for (i = 0; i < td->node_count; i++) {
        remap[i] = remove[i] ? -1 : write;
        if (!remove[i]) {
            if (write != i) td->nodes[write] = td->nodes[i];
            write++;
        }
    }

    /* Fix up parent_idx references */
    for (i = 0; i < write; i++) {
        int p = td->nodes[i].parent_idx;
        if (p >= 0) td->nodes[i].parent_idx = remap[p];
    }

    td->node_count = write;
    td->selected   = -1;

    treeview_rebuild_visible(w);
    w->dirty = 1;
}

void treeview_set_callbacks(widget_t *w,
                            void (*on_select)(tv_node_t *node, void *ctx),
                            void (*on_expand)(tv_node_t *node, void *ctx),
                            void *ctx)
{
    treeview_data_t *td = (treeview_data_t *)w->userdata;
    td->on_select = on_select;
    td->on_expand = on_expand;
    td->cb_ctx    = ctx;
}
