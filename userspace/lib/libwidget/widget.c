/*
 * AetherOS libwidget — Core engine (Phase 5.3)
 * File: userspace/lib/libwidget/widget.c
 *
 * Implements:
 *   widget_run()          — non-blocking poll event loop (WM events + mouse)
 *   widget_dispatch_key() — route key events to focused widget, handle Tab
 *   widget_dispatch_mouse()— hit-test + route mouse events, update hover/focus
 *   draw_recursive()      — pre-order draw, propagates dirty flag to children
 *   focus management      — g_focused + widget_set_focused() + focus_next()
 */

#include <widget.h>
#include <string.h>
#include <stdlib.h>
#include <sys.h>
#include <input.h>
#include <gfx.h>

/* ── Global focus state ──────────────────────────────────────────────────── */

static widget_t *g_focused;
static widget_t *g_hovered;
static unsigned int g_last_mouse_buttons;

widget_t *widget_get_focused(void) { return g_focused; }

void widget_set_focused(widget_t *w)
{
    if (g_focused == w) return;

    /* Notify old focus owner */
    if (g_focused) {
        if (g_focused->state == WS_FOCUSED)
            g_focused->state = WS_NORMAL;
        g_focused->dirty = 1;
        if (g_focused->event_fn) {
            widget_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = WEV_FOCUS_OUT;
            g_focused->event_fn(g_focused, &ev);
        }
    }

    g_focused = w;

    /* Notify new focus owner */
    if (g_focused) {
        g_focused->state = WS_FOCUSED;
        g_focused->dirty = 1;
        if (g_focused->event_fn) {
            widget_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = WEV_FOCUS_IN;
            g_focused->event_fn(g_focused, &ev);
        }
    }
}

/* ── Widget tree management ──────────────────────────────────────────────── */

void widget_init(widget_t *w, widget_type_t type,
                 int x, int y, int width, int height)
{
    memset(w, 0, sizeof(*w));
    w->bounds.x  = x;
    w->bounds.y  = y;
    w->bounds.w  = width;
    w->bounds.h  = height;
    w->type      = type;
    w->state     = WS_NORMAL;
    w->dirty     = 1;
}

void widget_add_child(widget_t *parent, widget_t *child)
{
    if (parent->nchildren >= WIDGET_MAX_CHILDREN) return;
    child->parent = parent;
    parent->children[parent->nchildren++] = child;
}

void widget_invalidate(widget_t *w)
{
    w->dirty = 1;
}

void widget_invalidate_all(widget_t *w)
{
    w->dirty = 1;
    for (int i = 0; i < w->nchildren; i++)
        widget_invalidate_all(w->children[i]);
}

/* ── Focus cycling (Tab key) ─────────────────────────────────────────────── */

#define FOCUS_MAX 32
static widget_t *s_focus_list[FOCUS_MAX];
static int       s_focus_count;

static void collect_focusable(widget_t *w)
{
    if (w->hidden) return;
    if (w->focusable && s_focus_count < FOCUS_MAX)
        s_focus_list[s_focus_count++] = w;
    for (int i = 0; i < w->nchildren; i++)
        collect_focusable(w->children[i]);
}

static void focus_cycle(widget_t *root, int direction)
{
    s_focus_count = 0;
    collect_focusable(root);
    if (s_focus_count == 0) return;

    int cur = -1;
    for (int i = 0; i < s_focus_count; i++)
        if (s_focus_list[i] == g_focused) { cur = i; break; }

    int next = (cur + direction + s_focus_count) % s_focus_count;
    widget_set_focused(s_focus_list[next]);
}

/* ── Hit testing ─────────────────────────────────────────────────────────── */

/*
 * Find deepest focusable/interactive widget hit at (mx,my).
 * parent_ax/ay: absolute position of w's parent's top-left.
 */
static widget_t *hit_test(widget_t *w, int mx, int my,
                           int parent_ax, int parent_ay)
{
    if (w->hidden) return NULL;

    int ax = parent_ax + w->bounds.x;
    int ay = parent_ay + w->bounds.y;

    if (mx < ax || mx >= ax + w->bounds.w ||
        my < ay || my >= ay + w->bounds.h)
        return NULL;

    /* Check children last-to-first so topmost (highest index) wins */
    for (int i = w->nchildren - 1; i >= 0; i--) {
        widget_t *h = hit_test(w->children[i], mx, my, ax, ay);
        if (h) return h;
    }

    if (w->focusable || w->event_fn)
        return w;

    return NULL;
}

/* ── Draw engine ─────────────────────────────────────────────────────────── */

/*
 * Pre-order traversal: draw self first (if dirty), then children.
 * If parent was dirty (force=1), children must also redraw because parent
 * may have overwritten their pixels with its background fill.
 */
static void draw_recursive(widget_t *w, int parent_ax, int parent_ay, int force)
{
    if (w->hidden) return;

    int ax = parent_ax + w->bounds.x;
    int ay = parent_ay + w->bounds.y;

    int was_dirty = (w->dirty || force);
    if (was_dirty && w->draw_fn) {
        w->draw_fn(w, ax, ay);
        w->dirty = 0;
    }

    for (int i = 0; i < w->nchildren; i++)
        draw_recursive(w->children[i], ax, ay, was_dirty);
}

/* ── Mouse dispatch ──────────────────────────────────────────────────────── */

static void dispatch_mouse(widget_t *root, int content_x, int content_y,
                           int mx, int my, unsigned int buttons,
                           unsigned int prev_buttons)
{
    /* Hover tracking */
    widget_t *under = hit_test(root, mx, my, content_x, content_y);
    if (under != g_hovered) {
        if (g_hovered && g_hovered->state == WS_HOVERED) {
            g_hovered->state = WS_NORMAL;
            g_hovered->dirty = 1;
        }
        g_hovered = under;
        if (g_hovered && g_hovered->state == WS_NORMAL) {
            g_hovered->state = WS_HOVERED;
            g_hovered->dirty = 1;
        }
    }

    int left_down    = (buttons & 1) && !(prev_buttons & 1);
    int left_up      = !(buttons & 1) && (prev_buttons & 1);

    if (left_down && under) {
        /* Focus the clicked widget */
        if (under->focusable)
            widget_set_focused(under);

        /* Set pressed state */
        under->state = WS_PRESSED;
        under->dirty = 1;

        if (under->event_fn) {
            widget_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type    = WEV_MOUSE_DOWN;
            ev.mx      = mx;
            ev.my      = my;
            ev.buttons = buttons;
            under->event_fn(under, &ev);
        }
    }

    if (left_up) {
        /* Deliver MOUSE_UP to the previously pressed widget (may differ from under) */
        widget_t *target = under ? under : g_hovered;
        if (target) {
            if (target->state == WS_PRESSED) {
                target->state = (target == g_focused) ? WS_FOCUSED : WS_NORMAL;
                target->dirty = 1;
            }
            if (target->event_fn) {
                widget_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type    = WEV_MOUSE_UP;
                ev.mx      = mx;
                ev.my      = my;
                ev.buttons = buttons;
                target->event_fn(target, &ev);
            }
        }
    }
}

/* ── Key dispatch ────────────────────────────────────────────────────────── */

static void dispatch_key(widget_t *root, const widget_event_t *ev)
{
    /* Tab cycles focus */
    if (ev->keycode == KEY_TAB) {
        int dir = (ev->modifiers & MOD_SHIFT) ? -1 : 1;
        focus_cycle(root, dir);
        return;
    }

    /* Route to focused widget */
    if (g_focused && g_focused->event_fn)
        g_focused->event_fn(g_focused, ev);
}

/* ── Tick dispatch (blink, animations) ──────────────────────────────────── */

static void tick_recursive(widget_t *w, long tick)
{
    if (w->event_fn && w->focusable) {
        widget_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = WEV_TICK;
        ev.tick = tick;
        w->event_fn(w, &ev);
    }
    for (int i = 0; i < w->nchildren; i++)
        tick_recursive(w->children[i], tick);
}

/* ── Main event loop ─────────────────────────────────────────────────────── */

#define BLINK_TICKS 30  /* 300 ms at 100 Hz */

void widget_run(widget_t *root, widget_ctx_t *ctx)
{
    long last_tick_event = 0;

    /* Initial draw */
    int cx = *ctx->win_x + ctx->content_dx;
    int cy = *ctx->win_y + ctx->content_dy;
    draw_recursive(root, cx, cy, 1);

    while (ctx->running) {
        int had_event = 0;

        /* ── Drain WM / key event ring ─────────────────────────────────── */
        unsigned long long raw;
        while ((raw = sys_wm_event_poll()) != 0) {
            had_event = 1;

            if (wm_event_is_redraw(raw)) {
                /* Window dragged — update position, notify app, redraw all */
                int new_wx = wm_event_redraw_x(raw);
                int new_wy = wm_event_redraw_y(raw);
                *ctx->win_x = new_wx;
                *ctx->win_y = new_wy;
                if (ctx->on_reposition)
                    ctx->on_reposition(ctx->userdata);
                cx = *ctx->win_x + ctx->content_dx;
                cy = *ctx->win_y + ctx->content_dy;
                widget_invalidate_all(root);
                draw_recursive(root, cx, cy, 1);
                continue;
            }

            /* Mouse events forwarded by init via SYS_WM_PUSH_EVENT */
            if (wm_event_is_mouse(raw)) {
                mouse_event_t mev = wm_event_mouse_unpack(raw);
                dispatch_mouse(root, cx, cy,
                               (int)mev.x, (int)mev.y,
                               mev.buttons, g_last_mouse_buttons);
                g_last_mouse_buttons = mev.buttons;
                continue;
            }

            key_event_t kev = key_event_unpack(raw);
            if (!kev.is_press) continue;

            widget_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type      = WEV_KEY_DOWN;
            ev.keycode   = kev.keycode;
            ev.modifiers = kev.modifiers;
            dispatch_key(root, &ev);
        }

        /* ── Periodic tick (blink, etc.) ───────────────────────────────── */
        long now = sys_get_ticks();
        if (now - last_tick_event >= BLINK_TICKS) {
            last_tick_event = now;
            tick_recursive(root, now);
        }

        /* ── Redraw dirty widgets ───────────────────────────────────────── */
        cx = *ctx->win_x + ctx->content_dx;
        cy = *ctx->win_y + ctx->content_dy;
        draw_recursive(root, cx, cy, 0);

        if (!had_event)
            sys_sched_yield();
    }
}
