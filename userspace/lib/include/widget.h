#ifndef AETHER_WIDGET_H
#define AETHER_WIDGET_H

/*
 * AetherOS libwidget — Retained widget toolkit (Phase 5.3 + 6.1.7)
 *
 * Usage pattern:
 *   1. Declare widget_t variables (global or local; they are ~500 bytes each).
 *   2. Call widget_init_<type>() to set bounds, draw/event hooks, and type data.
 *   3. Build tree with widget_add_child().
 *   4. Fill widget_ctx_t with window geometry and reposition callback.
 *   5. Call widget_run(root, &ctx) — runs the event loop until ctx.running == 0.
 */

#include <input.h>
#include <gfx.h>

/* ── Font metrics (shared with gfx.c) ───────────────────────────────────── */
#define WGT_FONT_W   8
#define WGT_FONT_H  16

/* ── Text alignment ─────────────────────────────────────────────────────── */
#define WGT_ALIGN_LEFT   0
#define WGT_ALIGN_CENTER 1
#define WGT_ALIGN_RIGHT  2

/* ── Buffer limits ──────────────────────────────────────────────────────── */
#define WGT_LABEL_MAX         256
#define WGT_TEXTINPUT_MAX     256
#define WGT_LISTITEM_LABEL    64

/* Textarea line buffer (malloc'd by widget_init_textarea) */
#define WGT_TEXTAREA_LINE_LEN 128

/* Progress Bar modes */
#define WGT_PROGRESS_DETERMINATE   0
#define WGT_PROGRESS_INDETERMINATE 1

/* ── Widget types ───────────────────────────────────────────────────────── */
typedef enum {
    WIDGET_PANEL,
    WIDGET_BUTTON,
    WIDGET_LABEL,
    WIDGET_TEXTINPUT,
    WIDGET_TEXTAREA,
    WIDGET_LISTVIEW,
    WIDGET_SCROLLBAR,
    WIDGET_CHECKBOX,
    WIDGET_TREEVIEW,    /* Phase 5.6: collapsible directory tree */
    WIDGET_PROGRESS,
} widget_type_t;

/* ── TreeView icon types (TVICON_*) ─────────────────────────────────────── */
#define TVICON_DRIVE_FAT32   0
#define TVICON_DRIVE_INITRD  1
#define TVICON_DRIVE_AFS     2
#define TVICON_FOLDER_CLOSED 3
#define TVICON_FOLDER_OPEN   4
#define TVICON_FILE          5

/* ── TreeView node ──────────────────────────────────────────────────────── */
typedef struct {
    char          label[64];
    unsigned char icon_type;     /* TVICON_* */
    unsigned char depth;         /* 0 = drive root, 1+ = subdirs */
    unsigned char has_children;
    unsigned char expanded;
    int           parent_idx;    /* -1 for roots */
    void         *userdata;      /* pointer to path string in app's pool */
} tv_node_t;

/* ── TreeView retained state (app declares this; treeview_init stores ptr) ─ */
typedef struct {
    tv_node_t  nodes[128];
    int        node_count;
    int        visible[128];     /* indices into nodes[] for visible rows */
    int        visible_count;
    int        selected;         /* index into visible[], -1 = none */
    int        scroll_offset;    /* first visible row index */
    int        draw_ax, draw_ay; /* last absolute draw origin (for events) */
    void     (*on_select)(tv_node_t *node, void *ctx);
    void     (*on_expand)(tv_node_t *node, void *ctx);
    void      *cb_ctx;
    unsigned int bg_color;   /* panel background; 0 → C_PANEL */
} treeview_data_t;

/* ── Visual state ───────────────────────────────────────────────────────── */
typedef enum {
    WS_NORMAL = 0,
    WS_HOVERED,
    WS_FOCUSED,
    WS_PRESSED,
    WS_DISABLED,
} widget_state_t;

/* ── Event types ────────────────────────────────────────────────────────── */
typedef enum {
    WEV_MOUSE_DOWN,
    WEV_MOUSE_UP,
    WEV_MOUSE_MOVE,
    WEV_KEY_DOWN,
    WEV_KEY_UP,
    WEV_FOCUS_IN,
    WEV_FOCUS_OUT,
    WEV_TICK,      /* periodic tick for blink/animations */
} widget_event_type_t;

typedef struct {
    widget_event_type_t type;
    int mx, my;           /* absolute screen coords (mouse events) */
    unsigned int buttons; /* bit 0 = left button */
    keycode_t keycode;
    unsigned int modifiers;
    long tick;            /* current sys_get_ticks() value (WEV_TICK) */
} widget_event_t;

/* ── Rectangle ──────────────────────────────────────────────────────────── */
typedef struct {
    int x, y, w, h;
} rect_t;

/* ── List item ──────────────────────────────────────────────────────────── */
typedef struct {
    char  label[WGT_LISTITEM_LABEL];
    void *userdata;
} list_item_t;

/* ── Forward declare widget_t ───────────────────────────────────────────── */
typedef struct widget_t widget_t;

/* ── Function pointer types ─────────────────────────────────────────────── */
/* draw_fn: draw widget at its absolute top-left (ax, ay) */
typedef void (*widget_draw_fn)(widget_t *w, int ax, int ay);
/* event_fn: handle event; return 1 if consumed (stops propagation) */
typedef int  (*widget_event_fn)(widget_t *w, const widget_event_t *ev);

/* ── Type-specific data structs ─────────────────────────────────────────── */

typedef struct {
    char  text[128];
    void (*on_click)(widget_t *w);
} wdata_button_t;

typedef struct {
    char text[WGT_LABEL_MAX];
    int  align;
} wdata_label_t;

typedef struct {
    char  buf[WGT_TEXTINPUT_MAX];
    int   len;
    int   cursor;      /* byte offset 0..len */
    long  blink_tick;  /* tick when blink last toggled */
    int   blink_on;
    void (*on_change)(widget_t *w);
    void (*on_submit)(widget_t *w);
} wdata_textinput_t;

typedef struct {
    char (*lines)[WGT_TEXTAREA_LINE_LEN]; /* malloc'd, n_lines_max entries */
    int   n_lines_max;
    int   n_lines;
    int   cur_row, cur_col;
    int   scroll_top;
} wdata_textarea_t;

typedef struct {
    list_item_t *items; /* malloc'd, n_items_max entries */
    int          n_items;
    int          n_items_max;
    int          selected;  /* -1 = none */
    int          scroll_top;
    void (*on_select)(widget_t *w, int index, void *userdata);
} wdata_listview_t;

typedef struct {
    int orientation; /* 0=vertical, 1=horizontal */
    int value;       /* current position (0..max) */
    int max;
    int page;        /* thumb size in value units */
} wdata_scrollbar_t;

typedef struct {
    unsigned int bg_color;
} wdata_panel_t;

typedef struct {
    char text[128];
    int  checked;
    void (*on_toggle)(widget_t *w, int checked);
} wdata_checkbox_t;

typedef struct {
    int value; /* 0-100 for determinate, or animation offset */
    int mode; /* DETERMINATE or INDETERMINATE */
} wdata_progress_t;

/* Union of all type-specific data (largest member ≈ 300 bytes for textinput) */
typedef union {
    wdata_button_t    button;
    wdata_label_t     label;
    wdata_textinput_t textinput;
    wdata_textarea_t  textarea;
    wdata_listview_t  listview;
    wdata_scrollbar_t scrollbar;
    wdata_panel_t     panel;
    wdata_checkbox_t  checkbox;
    wdata_progress_t  progress;
} widget_data_t;

/* ── Widget ─────────────────────────────────────────────────────────────── */
#define WIDGET_MAX_CHILDREN 16

struct widget_t {
    rect_t          bounds;     /* position+size relative to parent */
    widget_type_t   type;
    widget_state_t  state;
    int             dirty;      /* 1 = needs redraw */
    int             focusable;  /* 1 = Tab can land here */
    int             hidden;     /* 1 = skip draw + hit-test */

    widget_draw_fn  draw_fn;
    widget_event_fn event_fn;
    void           *userdata;   /* app-defined payload */

    widget_t       *parent;
    widget_t       *children[WIDGET_MAX_CHILDREN];
    int             nchildren;

    widget_data_t   data;
};

/* ── Run context ────────────────────────────────────────────────────────── */
typedef struct {
    int  *win_x;          /* pointer to current window abs-x (updated on drag) */
    int  *win_y;          /* pointer to current window abs-y */
    int   content_dx;     /* content_area.x = *win_x + content_dx */
    int   content_dy;     /* content_area.y = *win_y + content_dy */
    int   win_id;         /* WM window handle from sys_wm_register(); -1 = legacy */
    int   win_w;          /* full window width  (0 = use root bounds) */
    int   win_h;          /* full window height (0 = use root bounds) */
    void (*on_reposition)(void *userdata); /* draw chrome + handle new position */
    void *userdata;
    int   running;        /* set to 0 from a callback to exit widget_run() */
} widget_ctx_t;

/* ── Core API ───────────────────────────────────────────────────────────── */

void widget_init(widget_t *w, widget_type_t type,
                 int x, int y, int width, int height);
void widget_add_child(widget_t *parent, widget_t *child);
void widget_invalidate(widget_t *w);
void widget_invalidate_all(widget_t *w);

/* Main event loop — runs while ctx->running != 0 */
void widget_run(widget_t *root, widget_ctx_t *ctx);

/* ── Typed constructors ──────────────────────────────────────────────────── */

void widget_init_panel(widget_t *w, int x, int y, int width, int height,
                       unsigned int bg_color);

void widget_init_button(widget_t *w, int x, int y, int width, int height,
                        const char *text,
                        void (*on_click)(widget_t *w));

void widget_init_label(widget_t *w, int x, int y, int width, int height,
                       const char *text, int align);

void widget_init_textinput(widget_t *w, int x, int y, int width, int height,
                           void (*on_change)(widget_t *w),
                           void (*on_submit)(widget_t *w));

/* max_lines: number of lines to malloc; uses bump allocator (no free) */
void widget_init_textarea(widget_t *w, int x, int y, int width, int height,
                          int max_lines);

/* max_items: list capacity; uses bump allocator */
void widget_init_listview(widget_t *w, int x, int y, int width, int height,
                          int max_items,
                          void (*on_select)(widget_t *w, int idx, void *ud));

void widget_init_checkbox(widget_t *w, int x, int y, int width, int height,
                          const char *text,
                          void (*on_toggle)(widget_t *w, int checked));

void widget_init_scrollbar_v(widget_t *w, int x, int y, int width, int height,
                             int max, int page);
void widget_init_scrollbar_h(widget_t *w, int x, int y, int width, int height,
                             int max, int page);

void widget_init_progress(widget_t *w, int x, int y, int width, int height, int mode);

/* ── Widget helpers ─────────────────────────────────────────────────────── */

/* Label */
void label_set_text(widget_t *w, const char *text);

/* TextInput */
const char *textinput_get_text(widget_t *w);
void        textinput_set_text(widget_t *w, const char *text);
void        textinput_clear(widget_t *w);

/* Textarea */
void textarea_set_text(widget_t *w, const char *text);
void textarea_get_text(widget_t *w, char *buf, int max);
void textarea_scroll_to_bottom(widget_t *w);

/* ListView */
void listview_add_item(widget_t *w, const char *label, void *userdata);
void listview_clear(widget_t *w);
int  listview_get_selected(widget_t *w);

/* Checkbox */
int  checkbox_get_checked(widget_t *w);
void checkbox_set_checked(widget_t *w, int checked);

/* Progress Bar */
void progress_set_value(widget_t *w, int value);

/* ── Internal: called by individual widget .c files ────────────────────── */
/* (not part of public API but declared here for cross-file access) */
widget_t *widget_get_focused(void);
void      widget_set_focused(widget_t *w);  /* dispatches FOCUS_IN/OUT */

/* ── Spring interpolator (Phase 6.1.7b) ────────────────────────────────── */

/*
 * Damped spring for UI animations.
 * Euler-integrate with dt = 1/60 s per vsync frame.
 * Good defaults: k=600 d=35 (open, ~120ms), k=800 d=50 (close, ~80ms).
 */
typedef struct {
    float pos;      /* current value */
    float vel;      /* current velocity */
    float target;   /* destination value */
    float k;        /* stiffness (spring constant) */
    float d;        /* damping coefficient */
} spring_interp_t;

/* Initialise spring at pos with given target and constants. */
void spring_init(spring_interp_t *s, float pos, float target, float k, float d);
/* Move spring target (safe to call mid-animation). */
void spring_set_target(spring_interp_t *s, float target);
/* Advance spring by dt seconds (use 1.0f/60.0f for vsync cadence). */
void spring_step(spring_interp_t *s, float dt);
/* Returns 1 when spring has settled within threshold of target. */
int  spring_settled(const spring_interp_t *s);

/* ── Window animation (Phase 6.1.7c/d) ─────────────────────────────────── */

/*
 * win_anim_t — state for a single open or close window animation.
 * Allocates a GPU BO on start; free it when done with win_anim_free().
 */
typedef struct {
    int  active;           /* 1 while animation is running */
    int  is_open;          /* 1 = opening, 0 = closing */
    int  win_x, win_y;     /* window top-left on screen */
    int  win_w, win_h;     /* window natural size in pixels */
    int  content_bo;       /* GPU BO with captured window pixels */
    spring_interp_t scale_sp;   /* 0.0=invisible ↔ 1.0=full size */
    spring_interp_t alpha_sp;   /* 0.0=transparent ↔ 1.0=opaque   */
} win_anim_t;

/*
 * Capture window pixels from the framebuffer and start an open animation
 * (scale 0.85→1.0, alpha 0→1).  Returns 0 on success, -1 if BO unavailable
 * (animation module will simply not run; window is already visible).
 */
int  win_anim_open(win_anim_t *a, int wx, int wy, int ww, int wh);

/*
 * Capture window pixels and start a close animation (scale 1.0→0.85, alpha 1→0).
 * After win_anim_tick() returns 0, the caller should exit/hide the window.
 */
int  win_anim_close(win_anim_t *a, int wx, int wy, int ww, int wh);

/*
 * Advance animation by one vsync frame (calls sys_vsync_wait internally).
 * Returns 1 if animation is still running, 0 if it has settled.
 * Must be called in a loop until it returns 0.
 */
int  win_anim_tick(win_anim_t *a);

/* Release the GPU BO held by a win_anim_t (call after animation completes). */
void win_anim_free(win_anim_t *a);

/* ── Dock icon bounce (Phase 6.1.7e) ───────────────────────────────────── */

/*
 * dock_anim_t — spring-physics bounce for a dock/taskbar icon.
 * The caller re-renders the icon each tick using scale_sp.pos as scale.
 */
typedef struct {
    int  active;
    int  icon_x, icon_y;
    int  icon_size;       /* natural size in pixels */
    spring_interp_t scale_sp;   /* 1.0=normal, peaks at ~1.3 */
} dock_anim_t;

/* Start a bounce animation for a dock icon at (ix, iy) with given size. */
void dock_anim_start(dock_anim_t *a, int ix, int iy, int icon_size);
/* Advance one frame. Returns 1 if running, 0 when settled.
 * Read a->scale_sp.pos for the current render scale. */
int  dock_anim_tick(dock_anim_t *a);

/* Returns non-zero if any animation is currently running (used by widget_run). */
int  anim_any_active(void);

/* ── TreeView API (Phase 5.6) ───────────────────────────────────────────── */

/* Init treeview widget; data is the caller-allocated treeview_data_t */
void treeview_init(widget_t *w, int x, int y, int width, int height,
                   treeview_data_t *data);

/* Add a root drive node; returns index in nodes[] or -1 if pool full */
int  treeview_add_root(widget_t *w, const char *label, unsigned char icon,
                       void *userdata);

/* Add a child of parent_idx; returns node index or -1 */
int  treeview_add_child(widget_t *w, int parent_idx, const char *label,
                        unsigned char icon, int has_children, void *userdata);

/* Remove all descendants of parent_idx and compact the node pool */
void treeview_remove_children(widget_t *w, int parent_idx);

/* Wire up select/expand callbacks */
void treeview_set_callbacks(widget_t *w,
                            void (*on_select)(tv_node_t *node, void *ctx),
                            void (*on_expand)(tv_node_t *node, void *ctx),
                            void *ctx);

/* Rebuild visible[] from the current expand/collapse state */
void treeview_rebuild_visible(widget_t *w);

#endif /* AETHER_WIDGET_H */
