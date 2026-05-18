/*
 * Iteration 2 — QuickJS DOM bindings for AetherOS NetSurf bridge
 *
 * Expands the Phase 7.5 skeleton with real Window / Document / Element
 * objects and a cooperative timer system.
 *
 * QuickJS class IDs:
 *   g_dom_node_class_id — wraps dom_node* / dom_element*
 *   g_dom_doc_class_id  — wraps dom_document*
 *
 * Lifecycle:
 *   wrap_node() calls dom_node_ref(); the QJS finalizer calls dom_node_unref().
 *   js_destroythread() calls JS_FreeContext() (runs GC / finalizers) BEFORE
 *   NetSurf's html_destroy() frees the DOM — so finalizer unrefs are safe.
 *   js_closethread() marks the thread closed so stale wrappers fail safely.
 *
 * Timers:
 *   js_timers_tick() is called once per frame from browser_per_frame() and
 *   fires any due setTimeout / setInterval callbacks.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "quickjs/quickjs.h"
#include "content/handlers/javascript/js.h"
#include "content/handlers/javascript/content.h"
#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf_aether.h"

/* libdom */
#include "dom/dom.h"
#include "dom/core/document.h"
#include "dom/core/element.h"
#include "dom/core/node.h"
#include "dom/core/nodelist.h"
#include "dom/core/string.h"
#include "dom/core/text.h"
#include "dom/events/event_listener.h"
#include "dom/events/event_target.h"
#include "dom/events/event.h"

/*
 * html/private.h gives us html_content which contains the dom_document*.
 * js_newthread receives doc_priv = hlcache_handle_get_content(c), i.e.
 * struct content* pointing at an html_content.  We cast and read ->document.
 *
 * html/box.h + html/box_construct.h: needed to update box->text in-place
 * after JS DOM mutations (NetSurf's DOMSubtreeModified handler only refreshes
 * TEXTAREA/INPUT/STYLE elements, not generic divs).
 */
#include "html/private.h"
#include "html/box.h"
#include "html/box_construct.h"
#include "html/box_inspect.h"

extern char nsaether_url[512];
extern volatile bool nsaether_dirty;

/* Direct UART for diagnostics (bypasses nslog filtering) */
static void js_uart(const char *s)
{
    long r; int len = 0;
    while (s[len]) len++;
    __asm__ volatile(
        "mov x8,#34\n mov x0,#1\n mov x1,%1\n mov x2,%2\n"
        "svc #0\n mov %0,x0\n"
        : "=r"(r) : "r"(s), "r"((long)len) : "x0","x1","x2","x8","memory");
}

/* ── QuickJS class IDs (global, assigned once in js_initialise) ──────────── */

static JSClassID g_dom_node_class_id;
static JSClassID g_dom_doc_class_id;

/* ── Forward declarations ────────────────────────────────────────────────── */

struct jsthread;
static JSValue wrap_node(JSContext *jsc, struct jsthread *t, struct dom_node *n);

/* ── Event listener store ────────────────────────────────────────────────── */

/* Context allocated per addEventListener call and freed in js_destroythread. */
typedef struct aether_listener_ctx {
    struct jsthread          *thread;
    JSContext                *jsc;
    JSValue                   fn;
} aether_listener_ctx_t;

typedef struct js_listener {
    struct dom_node          *node;       /* raw pointer, not ref'd */
    char                      type[32];
    JSValue                   fn;         /* duplicate for removeEventListener lookup */
    struct jsthread          *thread;
    dom_event_listener       *dom_listen; /* libdom listener (unref'd in destroy) */
    aether_listener_ctx_t    *ctx;        /* freed in destroy */
    struct js_listener       *next;
} js_listener_t;

/* ── Timer store ─────────────────────────────────────────────────────────── */

typedef struct js_timer {
    int             id;
    bool            repeating;     /* true = setInterval */
    int             ms;            /* interval (for repeating) */
    JSContext      *jsc;
    JSValue         fn;
    struct timeval  fire_at;
    struct js_timer *next;
} js_timer_t;

/* ── Node wrapper (stored as QJS opaque) ─────────────────────────────────── */

typedef struct {
    struct jsthread *thread;
    struct dom_node *node;
} node_wrapper_t;

/* ── opaque type implementations ─────────────────────────────────────────── */

struct jsheap {
    JSRuntime *rt;
    int        timeout;
};

struct jsthread {
    JSContext          *jsc;
    jsheap             *heap;
    bool                closed;
    struct dom_document *doc;       /* doc_priv from js_newthread */
    html_content       *html;       /* for box-tree text updates */
    js_timer_t         *timers;
    js_listener_t      *listeners;
    int                 next_timer_id;
    struct jsthread    *next;       /* global linked list */
};

static struct jsthread *g_thread_list = NULL;

/* ── Utilities: dom_string ───────────────────────────────────────────────── */

static dom_string *cstr_to_domstr(const char *s)
{
    dom_string *r = NULL;
    if (!s) return NULL;
    dom_string_create((const uint8_t *)s, strlen(s), &r);
    return r;
}

static JSValue domstr_to_jsval(JSContext *jsc, dom_string *ds)
{
    if (!ds) return JS_NULL;
    JSValue v = JS_NewStringLen(jsc, dom_string_data(ds), dom_string_byte_length(ds));
    dom_string_unref(ds);
    return v;
}

/* ── Node wrapping ───────────────────────────────────────────────────────── */

static JSValue wrap_node(JSContext *jsc, struct jsthread *t, struct dom_node *node);

static node_wrapper_t *unwrap_node_w(JSContext *jsc, JSValue v)
{
    if (!JS_IsObject(v)) return NULL;
    /* Try node class first, then doc class */
    node_wrapper_t *w = JS_GetOpaque(v, g_dom_node_class_id);
    if (!w) w = JS_GetOpaque(v, g_dom_doc_class_id);
    return w;
}

static struct dom_node *unwrap_node(JSContext *jsc, JSValue v)
{
    node_wrapper_t *w = unwrap_node_w(jsc, v);
    if (!w || !w->thread || w->thread->closed) return NULL;
    return w->node;
}

static void dom_node_js_finalizer(JSRuntime *rt, JSValue val)
{
    node_wrapper_t *w = JS_GetOpaque(val, g_dom_node_class_id);
    if (w) {
        if (w->node && w->thread && !w->thread->closed)
            dom_node_unref(w->node);
        free(w);
    }
}

static void dom_doc_js_finalizer(JSRuntime *rt, JSValue val)
{
    node_wrapper_t *w = JS_GetOpaque(val, g_dom_doc_class_id);
    if (w) {
        /* document is owned by NetSurf — don't unref it */
        free(w);
    }
}

/* ── DOM tree walker (DFS pre-order) ─────────────────────────────────────── */

typedef bool (*selector_fn)(struct dom_element *el, const void *data);

/* Returns element with +1 ref, or NULL.  Caller must dom_node_unref. */
static struct dom_element *dom_walk(struct dom_node *node,
                                    selector_fn match, const void *data)
{
    dom_node_type type = 0;
    if (dom_node_get_node_type(node, &type) != DOM_NO_ERR) return NULL;

    if (type == DOM_ELEMENT_NODE) {
        if (match((struct dom_element *)node, data)) {
            dom_node_ref(node);
            return (struct dom_element *)node;
        }
    }

    struct dom_node *child = NULL;
    if (dom_node_get_first_child(node, &child) != DOM_NO_ERR || !child)
        return NULL;

    while (child) {
        struct dom_element *found = dom_walk(child, match, data);
        if (found) {
            dom_node_unref(child);
            return found;
        }
        struct dom_node *next = NULL;
        dom_node_get_next_sibling(child, &next);
        dom_node_unref(child);
        child = next;
    }
    return NULL;
}

/* ── Selector matchers ───────────────────────────────────────────────────── */

static bool match_by_id(struct dom_element *el, const void *data)
{
    const char *id = (const char *)data;
    dom_string *dname = cstr_to_domstr("id");
    if (!dname) return false;
    dom_string *val = NULL;
    dom_element_get_attribute(el, dname, &val);
    dom_string_unref(dname);
    if (!val) return false;
    bool ok = (strncmp(dom_string_data(val), id, dom_string_byte_length(val)) == 0
               && id[dom_string_byte_length(val)] == '\0');
    dom_string_unref(val);
    return ok;
}

static bool match_by_class(struct dom_element *el, const void *data)
{
    const char *cls = (const char *)data;
    dom_string *dname = cstr_to_domstr("class");
    if (!dname) return false;
    dom_string *val = NULL;
    dom_element_get_attribute(el, dname, &val);
    dom_string_unref(dname);
    if (!val) return false;
    /* Simple substring match — works for single-class elements */
    bool ok = (strstr(dom_string_data(val), cls) != NULL);
    dom_string_unref(val);
    return ok;
}

static bool match_by_tag(struct dom_element *el, const void *data)
{
    const char *tag = (const char *)data;
    dom_string *name = NULL;
    if (dom_node_get_node_name((struct dom_node *)el, &name) != DOM_NO_ERR || !name)
        return false;
    /* Case-insensitive comparison */
    const char *n = dom_string_data(name);
    size_t nlen   = dom_string_byte_length(name);
    size_t tlen   = strlen(tag);
    bool ok = false;
    if (nlen == tlen) {
        ok = true;
        for (size_t i = 0; i < nlen; i++) {
            char a = (n[i] >= 'A' && n[i] <= 'Z') ? n[i] + 32 : n[i];
            char b = (tag[i] >= 'A' && tag[i] <= 'Z') ? tag[i] + 32 : tag[i];
            if (a != b) { ok = false; break; }
        }
    }
    dom_string_unref(name);
    return ok;
}

/* ── Element prototype methods ───────────────────────────────────────────── */

static JSValue js_el_getAttribute(JSContext *jsc, JSValue this_val,
                                   int argc, JSValue *argv)
{
    struct dom_node *n = unwrap_node(jsc, this_val);
    if (!n || argc < 1) return JS_NULL;
    const char *name = JS_ToCString(jsc, argv[0]);
    if (!name) return JS_NULL;
    dom_string *dname = cstr_to_domstr(name);
    JS_FreeCString(jsc, name);
    if (!dname) return JS_NULL;
    dom_string *val = NULL;
    dom_element_get_attribute((struct dom_element *)n, dname, &val);
    dom_string_unref(dname);
    if (!val) return JS_NULL;
    return domstr_to_jsval(jsc, val);
}

static JSValue js_el_setAttribute(JSContext *jsc, JSValue this_val,
                                   int argc, JSValue *argv)
{
    struct dom_node *n = unwrap_node(jsc, this_val);
    if (!n || argc < 2) return JS_UNDEFINED;
    const char *name = JS_ToCString(jsc, argv[0]);
    const char *val  = JS_ToCString(jsc, argv[1]);
    if (name && val) {
        dom_string *dn = cstr_to_domstr(name);
        dom_string *dv = cstr_to_domstr(val);
        if (dn && dv) dom_element_set_attribute((struct dom_element *)n, dn, dv);
        if (dn) dom_string_unref(dn);
        if (dv) dom_string_unref(dv);
    }
    if (name) JS_FreeCString(jsc, name);
    if (val)  JS_FreeCString(jsc, val);
    return JS_UNDEFINED;
}

static JSValue js_el_hasAttribute(JSContext *jsc, JSValue this_val,
                                   int argc, JSValue *argv)
{
    struct dom_node *n = unwrap_node(jsc, this_val);
    if (!n || argc < 1) return JS_FALSE;
    const char *name = JS_ToCString(jsc, argv[0]);
    if (!name) return JS_FALSE;
    dom_string *dname = cstr_to_domstr(name);
    JS_FreeCString(jsc, name);
    if (!dname) return JS_FALSE;
    dom_string *val = NULL;
    dom_element_get_attribute((struct dom_element *)n, dname, &val);
    dom_string_unref(dname);
    bool has = (val != NULL);
    if (val) dom_string_unref(val);
    return JS_NewBool(jsc, has);
}

/* ── Box-tree text update ────────────────────────────────────────────────────
 * NetSurf's DOMSubtreeModified handler only refreshes TEXTAREA/INPUT/STYLE.
 * For generic elements (div, p, span …) we must patch box->text in-place so
 * that browser_window_redraw() picks up the new text without a full reload.
 *
 * Strategy: DFS-walk the subtree rooted at the element's box.  The first
 * BOX_TEXT node found gets new_text; all others are cleared to "" (they
 * originated from removed child text nodes after set_text_content replaced
 * all children with a single new text node).
 */
static void sync_text_recursive(struct box *b, html_content *htmlc,
                                 const char *new_text, bool *first_done)
{
    if (!b) return;
    if (b->type == BOX_TEXT) {
        const char *t = *first_done ? "" : new_text;
        *first_done = true;
        b->text   = talloc_strdup(htmlc->bctx, t);
        b->length = b->text ? strlen(b->text) : 0;
    }
    sync_text_recursive(b->children, htmlc, new_text, first_done);
    sync_text_recursive(b->next,     htmlc, new_text, first_done);
}

static void update_box_text(html_content *htmlc, struct dom_node *n,
                             const char *new_text)
{
    if (!htmlc || !n || !new_text) return;
    struct box *b = box_for_node(n);
    if (!b) return;

    bool first_done = false;

    if (b->type == BOX_INLINE && b->inline_end) {
        /* Inline elements (span, a, em, ...): NetSurf places the text content
         * in sibling boxes between the BOX_INLINE opener and its
         * BOX_INLINE_END closer — not in b->children.  Walk that range. */
        for (struct box *s = b->next; s && s != b->inline_end; s = s->next) {
            if (s->type == BOX_TEXT) {
                const char *t = first_done ? "" : new_text;
                first_done = true;
                s->text   = talloc_strdup(htmlc->bctx, t);
                s->length = s->text ? strlen(s->text) : 0;
            }
            /* Nested inline elements: recurse into their children */
            sync_text_recursive(s->children, htmlc, new_text, &first_done);
        }
    } else {
        /* Block / inline-container: text content lives in the children subtree */
        sync_text_recursive(b->children, htmlc, new_text, &first_done);
    }

    html__redraw_a_box(htmlc, b);
}

static JSValue js_el_get_textContent(JSContext *jsc, JSValue this_val)
{
    struct dom_node *n = unwrap_node(jsc, this_val);
    if (!n) return JS_NewString(jsc, "");
    dom_string *text = NULL;
    dom_node_get_text_content(n, &text);
    if (!text) return JS_NewString(jsc, "");
    return domstr_to_jsval(jsc, text);
}

static JSValue js_el_set_textContent(JSContext *jsc, JSValue this_val, JSValue val)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->node) return JS_UNDEFINED;
    struct dom_node *n = w->node;
    const char *s = JS_ToCString(jsc, val);
    if (s) {
        dom_string *ds = cstr_to_domstr(s);
        if (ds) { dom_node_set_text_content(n, ds); dom_string_unref(ds); }
        if (w->thread && w->thread->html)
            update_box_text(w->thread->html, n, s);
        JS_FreeCString(jsc, s);
    }
    nsaether_dirty = true;
    return JS_UNDEFINED;
}

/* innerHTML: getter returns textContent; setter strips tags and sets textContent */
static JSValue js_el_get_innerHTML(JSContext *jsc, JSValue this_val)
{
    return js_el_get_textContent(jsc, this_val);
}

static JSValue js_el_set_innerHTML(JSContext *jsc, JSValue this_val, JSValue val)
{
    /* Simplified: strip HTML tags, set as textContent */
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->node) return JS_UNDEFINED;
    struct dom_node *n = w->node;
    const char *src = JS_ToCString(jsc, val);
    if (!src) return JS_UNDEFINED;

    /* Strip tags: copy non-tag characters */
    size_t slen = strlen(src);
    char *buf = malloc(slen + 1);
    if (buf) {
        size_t out = 0;
        bool in_tag = false;
        for (size_t i = 0; i < slen; i++) {
            if (src[i] == '<') { in_tag = true; continue; }
            if (src[i] == '>') { in_tag = false; continue; }
            if (!in_tag) buf[out++] = src[i];
        }
        buf[out] = '\0';
        dom_string *ds = cstr_to_domstr(buf);
        if (ds) { dom_node_set_text_content(n, ds); dom_string_unref(ds); }
        if (w->thread && w->thread->html)
            update_box_text(w->thread->html, n, buf);
        free(buf);
    }
    JS_FreeCString(jsc, src);
    nsaether_dirty = true;
    return JS_UNDEFINED;
}

static JSValue js_el_get_tagName(JSContext *jsc, JSValue this_val)
{
    struct dom_node *n = unwrap_node(jsc, this_val);
    if (!n) return JS_UNDEFINED;
    dom_string *name = NULL;
    dom_node_get_node_name(n, &name);
    if (!name) return JS_UNDEFINED;
    /* Upper-case it (HTML convention) */
    const char *raw = dom_string_data(name);
    size_t len = dom_string_byte_length(name);
    char *buf = malloc(len + 1);
    if (buf) {
        for (size_t i = 0; i < len; i++)
            buf[i] = (raw[i] >= 'a' && raw[i] <= 'z') ? raw[i] - 32 : raw[i];
        buf[len] = '\0';
    }
    dom_string_unref(name);
    if (!buf) return JS_UNDEFINED;
    JSValue r = JS_NewString(jsc, buf);
    free(buf);
    return r;
}

static JSValue js_el_get_id(JSContext *jsc, JSValue this_val)
{
    JSValue arg = JS_NewString(jsc, "id");
    JSValue r = js_el_getAttribute(jsc, this_val, 1, &arg);
    JS_FreeValue(jsc, arg);
    return JS_IsNull(r) ? JS_NewString(jsc, "") : r;
}

static JSValue js_el_set_id(JSContext *jsc, JSValue this_val, JSValue val)
{
    JSValue karg = JS_NewString(jsc, "id");
    JSValue args[2] = { karg, val };
    JSValue r = js_el_setAttribute(jsc, this_val, 2, args);
    JS_FreeValue(jsc, karg);
    return r;
}

static JSValue js_el_get_className(JSContext *jsc, JSValue this_val)
{
    JSValue arg = JS_NewString(jsc, "class");
    JSValue r = js_el_getAttribute(jsc, this_val, 1, &arg);
    JS_FreeValue(jsc, arg);
    return JS_IsNull(r) ? JS_NewString(jsc, "") : r;
}

static JSValue js_el_set_className(JSContext *jsc, JSValue this_val, JSValue val)
{
    JSValue karg = JS_NewString(jsc, "class");
    JSValue args[2] = { karg, val };
    JSValue r = js_el_setAttribute(jsc, this_val, 2, args);
    JS_FreeValue(jsc, karg);
    return r;
}

static JSValue js_el_appendChild(JSContext *jsc, JSValue this_val,
                                  int argc, JSValue *argv)
{
    struct dom_node *parent = unwrap_node(jsc, this_val);
    if (!parent || argc < 1) return JS_UNDEFINED;
    struct dom_node *child = unwrap_node(jsc, argv[0]);
    if (!child) return JS_UNDEFINED;
    struct dom_node *inserted = NULL;
    dom_node_append_child(parent, child, &inserted);
    if (inserted) dom_node_unref(inserted);
    return JS_DupValue(jsc, argv[0]);
}

static JSValue js_el_get_parentNode(JSContext *jsc, JSValue this_val)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->node || w->thread->closed) return JS_NULL;
    struct dom_node *parent = NULL;
    dom_node_get_parent_node(w->node, &parent);
    if (!parent) return JS_NULL;
    JSValue r = wrap_node(jsc, w->thread, parent);
    dom_node_unref(parent);
    return r;
}

static JSValue js_el_get_firstChild(JSContext *jsc, JSValue this_val)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->node || w->thread->closed) return JS_NULL;
    struct dom_node *child = NULL;
    dom_node_get_first_child(w->node, &child);
    if (!child) return JS_NULL;
    JSValue r = wrap_node(jsc, w->thread, child);
    dom_node_unref(child);
    return r;
}

static JSValue js_el_get_nextSibling(JSContext *jsc, JSValue this_val)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->node || w->thread->closed) return JS_NULL;
    struct dom_node *sib = NULL;
    dom_node_get_next_sibling(w->node, &sib);
    if (!sib) return JS_NULL;
    JSValue r = wrap_node(jsc, w->thread, sib);
    dom_node_unref(sib);
    return r;
}

/* ── DOM event → QuickJS bridge ──────────────────────────────────────────── */

/* Called by libdom when a DOM event fires on an element we registered on.
 * NOTE: libdom dispatch is bypassed for clicks; this path is kept for
 * potential future use with other event types. */
static void aether_js_event_handler(struct dom_event *evt, void *pw)
{
    aether_listener_ctx_t *ctx = (aether_listener_ctx_t *)pw;
    if (!ctx || !ctx->thread || ctx->thread->closed) return;

    JSContext *jsc = ctx->jsc;

    /* Build a minimal JS Event object */
    JSValue ev = JS_NewObject(jsc);
    dom_string *type_str = NULL;
    if (dom_event_get_type(evt, &type_str) == DOM_NO_ERR && type_str) {
        JS_SetPropertyStr(jsc, ev, "type",
            JS_NewStringLen(jsc, dom_string_data(type_str),
                            dom_string_byte_length(type_str)));
        dom_string_unref(type_str);
    }
    dom_event_target *targ = NULL;
    if (dom_event_get_target(evt, &targ) == DOM_NO_ERR && targ) {
        JS_SetPropertyStr(jsc, ev, "target",
            wrap_node(jsc, ctx->thread, (struct dom_node *)targ));
        dom_node_unref((struct dom_node *)targ);
    }

    JSValue args[1] = { ev };
    JSValue r = JS_Call(jsc, ctx->fn, JS_UNDEFINED, 1, args);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(jsc);
        const char *msg = JS_ToCString(jsc, exc);
        NSLOG(netsurf, WARNING, "JS event handler: %s", msg ? msg : "?");
        if (msg) JS_FreeCString(jsc, msg);
        JS_FreeValue(jsc, exc);
    }
    JS_FreeValue(jsc, r);
    JS_FreeValue(jsc, ev);

    /* DOM change may have happened — request repaint */
    nsaether_dirty = true;
}

/* Direct click dispatch: called from interaction.c after box hit-test.
 * Bypasses libdom event dispatch; fires matching JS listeners directly
 * and simulates DOM bubbling by walking up through ancestor nodes. */
void js_fire_click_event(struct dom_node *clicked_node)
{
    if (!clicked_node) return;

    for (struct jsthread *t = g_thread_list; t; t = t->next) {
        if (t->closed) continue;

        struct dom_node *cur = clicked_node;
        dom_node_ref(cur);
        while (cur) {
            for (js_listener_t *e = t->listeners; e; e = e->next) {
                if (strcmp(e->type, "click") != 0 || e->node != cur) continue;
                aether_listener_ctx_t *ctx = e->ctx;
                if (!ctx || t->closed) continue;

                JSContext *jsc = ctx->jsc;
                JSValue ev = JS_NewObject(jsc);
                JS_SetPropertyStr(jsc, ev, "type", JS_NewString(jsc, "click"));
                JS_SetPropertyStr(jsc, ev, "target",
                    wrap_node(jsc, t, clicked_node));

                JSValue args[1] = { ev };
                JSValue r = JS_Call(jsc, ctx->fn, JS_UNDEFINED, 1, args);
                if (JS_IsException(r)) {
                    JSValue exc = JS_GetException(jsc);
                    const char *msg = JS_ToCString(jsc, exc);
                    if (msg) { js_uart(msg); JS_FreeCString(jsc, msg); }
                    JS_FreeValue(jsc, exc);
                }
                JS_FreeValue(jsc, r);
                JS_FreeValue(jsc, ev);
                nsaether_dirty = true;
            }

            struct dom_node *parent = NULL;
            dom_node_get_parent_node(cur, &parent);
            dom_node_unref(cur);
            cur = parent;
        }
    }
}

/* Called from main.c on WEV_MOUSE_UP with document-space coordinates.
 * Replicates NetSurf's box hit-test to find the clicked DOM node, then
 * fires matching JS event listeners directly — keeping vendor files unmodified. */
void js_handle_mouse_click(int x, int y)
{
    for (struct jsthread *t = g_thread_list; t; t = t->next) {
        if (t->closed || !t->html || !t->html->layout) continue;
        html_content *html = t->html;

        struct box *box = html->layout;
        int box_x = 0, box_y = 0;
        struct dom_node *clicked = html->layout->node; /* fallback: <html> node */

        struct box *next;
        while ((next = box_at_point(&html->unit_len_ctx, box,
                                    x, y, &box_x, &box_y))) {
            if (next->node) clicked = next->node;
            box = next;
        }
        js_fire_click_event(clicked);
    }
}

/* Register a JS click listener on target_node.
 * We bypass libdom's event dispatch (it silently fails to reach our
 * callbacks) and instead use js_fire_click_event() called directly from
 * interaction.c after box hit-testing sets mas.node. */
static void register_dom_listener(JSContext *jsc, struct jsthread *thread,
                                   struct dom_node *target_node,
                                   const char *type, JSValue fn)
{
    aether_listener_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return;
    ctx->thread = thread;
    ctx->jsc    = jsc;
    ctx->fn     = JS_DupValue(jsc, fn);

    js_listener_t *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        JS_FreeValue(jsc, ctx->fn);
        free(ctx);
        return;
    }
    entry->node       = target_node;
    strncpy(entry->type, type, sizeof(entry->type) - 1);
    entry->fn         = JS_DupValue(jsc, fn);
    entry->thread     = thread;
    entry->dom_listen = NULL;
    entry->ctx        = ctx;
    entry->next       = thread->listeners;
    thread->listeners = entry;
}

/* ── Event listener methods on elements ──────────────────────────────────── */

static JSValue js_el_addEventListener(JSContext *jsc, JSValue this_val,
                                       int argc, JSValue *argv)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->node || w->thread->closed || argc < 2) return JS_UNDEFINED;
    if (!JS_IsFunction(jsc, argv[1])) return JS_UNDEFINED;

    const char *type = JS_ToCString(jsc, argv[0]);
    if (!type) return JS_UNDEFINED;
    register_dom_listener(jsc, w->thread, w->node, type, argv[1]);
    JS_FreeCString(jsc, type);
    return JS_UNDEFINED;
}

static JSValue js_el_removeEventListener(JSContext *jsc, JSValue this_val,
                                           int argc, JSValue *argv)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->node || w->thread->closed || argc < 2) return JS_UNDEFINED;

    const char *type = JS_ToCString(jsc, argv[0]);
    if (!type) return JS_UNDEFINED;

    js_listener_t **pp = &w->thread->listeners;
    while (*pp) {
        js_listener_t *e = *pp;
        if (e->node == w->node && strcmp(e->type, type) == 0) {
            *pp = e->next;
            JS_FreeValue(jsc, e->fn);
            free(e);
        } else {
            pp = &e->next;
        }
    }
    JS_FreeCString(jsc, type);
    return JS_UNDEFINED;
}

/* ── Element function list ───────────────────────────────────────────────── */

static const JSCFunctionListEntry dom_element_proto[] = {
    JS_CFUNC_DEF("getAttribute",         1, js_el_getAttribute),
    JS_CFUNC_DEF("setAttribute",         2, js_el_setAttribute),
    JS_CFUNC_DEF("hasAttribute",         1, js_el_hasAttribute),
    JS_CFUNC_DEF("appendChild",          1, js_el_appendChild),
    JS_CFUNC_DEF("addEventListener",     2, js_el_addEventListener),
    JS_CFUNC_DEF("removeEventListener",  2, js_el_removeEventListener),
    JS_CGETSET_DEF("textContent", js_el_get_textContent, js_el_set_textContent),
    JS_CGETSET_DEF("innerHTML",   js_el_get_innerHTML,   js_el_set_innerHTML),
    JS_CGETSET_DEF("tagName",     js_el_get_tagName,     NULL),
    JS_CGETSET_DEF("id",          js_el_get_id,          js_el_set_id),
    JS_CGETSET_DEF("className",   js_el_get_className,   js_el_set_className),
    JS_CGETSET_DEF("parentNode",  js_el_get_parentNode,  NULL),
    JS_CGETSET_DEF("firstChild",  js_el_get_firstChild,  NULL),
    JS_CGETSET_DEF("nextSibling", js_el_get_nextSibling, NULL),
};

/* ── wrap_node (needs proto defined — must come after proto list) ─────────── */

static JSValue wrap_node(JSContext *jsc, struct jsthread *t, struct dom_node *node)
{
    if (!node || !t) return JS_NULL;
    dom_node_ref(node);
    node_wrapper_t *w = calloc(1, sizeof(*w));
    if (!w) { dom_node_unref(node); return JS_NULL; }
    w->thread = t;
    w->node   = node;
    JSValue obj = JS_NewObjectClass(jsc, g_dom_node_class_id);
    if (JS_IsException(obj)) { dom_node_unref(node); free(w); return obj; }
    JS_SetOpaque(obj, w);
    return obj;
}

/* ── Document methods ────────────────────────────────────────────────────── */

static JSValue js_doc_getElementById(JSContext *jsc, JSValue this_val,
                                      int argc, JSValue *argv)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->thread->doc || w->thread->closed || argc < 1) return JS_NULL;

    const char *id = JS_ToCString(jsc, argv[0]);
    if (!id) return JS_NULL;

    /* Use libdom's built-in getElementById */
    dom_string *ds_id = cstr_to_domstr(id);
    JS_FreeCString(jsc, id);
    if (!ds_id) return JS_NULL;

    struct dom_element *el = NULL;
    dom_document_get_element_by_id(w->thread->doc, ds_id, &el);
    dom_string_unref(ds_id);

    if (!el) return JS_NULL;
    JSValue r = wrap_node(jsc, w->thread, (struct dom_node *)el);
    dom_node_unref(el);
    return r;
}

static JSValue js_doc_querySelector(JSContext *jsc, JSValue this_val,
                                     int argc, JSValue *argv)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->thread->doc || w->thread->closed || argc < 1) return JS_NULL;

    const char *sel = JS_ToCString(jsc, argv[0]);
    if (!sel) return JS_NULL;

    struct dom_element *found = NULL;

    if (sel[0] == '#') {
        /* #id selector */
        dom_string *ds_id = cstr_to_domstr(sel + 1);
        if (ds_id) {
            dom_document_get_element_by_id(w->thread->doc, ds_id, &found);
            dom_string_unref(ds_id);
        }
    } else if (sel[0] == '.') {
        /* .class selector */
        found = dom_walk((struct dom_node *)w->thread->doc, match_by_class, sel + 1);
    } else {
        /* tag selector */
        found = dom_walk((struct dom_node *)w->thread->doc, match_by_tag, sel);
    }

    JS_FreeCString(jsc, sel);

    if (!found) return JS_NULL;
    JSValue r = wrap_node(jsc, w->thread, (struct dom_node *)found);
    dom_node_unref(found);
    return r;
}

static JSValue js_doc_querySelectorAll(JSContext *jsc, JSValue this_val,
                                        int argc, JSValue *argv)
{
    /* Returns a JS Array for simplicity */
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->thread->doc || w->thread->closed || argc < 1) return JS_NewArray(jsc);

    const char *sel = JS_ToCString(jsc, argv[0]);
    if (!sel) return JS_NewArray(jsc);

    JSValue arr = JS_NewArray(jsc);
    uint32_t idx = 0;

    /* Only tag-based querySelectorAll is supported for now */
    if (sel[0] != '#' && sel[0] != '.') {
        /* Walk entire document collecting matching elements */
        struct dom_node *cur = NULL;
        dom_node_get_first_child((struct dom_node *)w->thread->doc, &cur);
        while (cur) {
            struct dom_element *found = dom_walk(cur, match_by_tag, sel);
            if (found) {
                JSValue v = wrap_node(jsc, w->thread, (struct dom_node *)found);
                dom_node_unref(found);
                JS_SetPropertyUint32(jsc, arr, idx++, v);
            }
            struct dom_node *next = NULL;
            dom_node_get_next_sibling(cur, &next);
            dom_node_unref(cur);
            cur = next;
        }
    }

    JS_FreeCString(jsc, sel);
    return arr;
}

static JSValue js_doc_createElement(JSContext *jsc, JSValue this_val,
                                     int argc, JSValue *argv)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->thread->doc || w->thread->closed || argc < 1) return JS_NULL;

    const char *tag = JS_ToCString(jsc, argv[0]);
    if (!tag) return JS_NULL;

    dom_string *ds_tag = cstr_to_domstr(tag);
    JS_FreeCString(jsc, tag);
    if (!ds_tag) return JS_NULL;

    struct dom_element *el = NULL;
    dom_document_create_element(w->thread->doc, ds_tag, &el);
    dom_string_unref(ds_tag);

    if (!el) return JS_NULL;
    JSValue r = wrap_node(jsc, w->thread, (struct dom_node *)el);
    dom_node_unref(el);
    return r;
}

static JSValue js_doc_createTextNode(JSContext *jsc, JSValue this_val,
                                      int argc, JSValue *argv)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->thread->doc || w->thread->closed || argc < 1) return JS_NULL;

    const char *txt = JS_ToCString(jsc, argv[0]);
    if (!txt) return JS_NULL;

    dom_string *ds_txt = cstr_to_domstr(txt);
    JS_FreeCString(jsc, txt);
    if (!ds_txt) return JS_NULL;

    struct dom_text *tn = NULL;
    dom_document_create_text_node(w->thread->doc, ds_txt, &tn);
    dom_string_unref(ds_txt);

    if (!tn) return JS_NULL;
    JSValue r = wrap_node(jsc, w->thread, (struct dom_node *)tn);
    dom_node_unref((struct dom_node *)tn);
    return r;
}

static JSValue js_doc_get_body(JSContext *jsc, JSValue this_val)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->thread->doc || w->thread->closed) return JS_NULL;
    struct dom_element *body = dom_walk(
            (struct dom_node *)w->thread->doc, match_by_tag, "body");
    if (!body) return JS_NULL;
    JSValue r = wrap_node(jsc, w->thread, (struct dom_node *)body);
    dom_node_unref(body);
    return r;
}

static JSValue js_doc_get_title(JSContext *jsc, JSValue this_val)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || !w->thread->doc || w->thread->closed) return JS_NewString(jsc, "");
    /* Find <title> element and return its text content */
    struct dom_element *title_el = dom_walk(
            (struct dom_node *)w->thread->doc, match_by_tag, "title");
    if (!title_el) return JS_NewString(jsc, "");
    dom_string *text = NULL;
    dom_node_get_text_content((struct dom_node *)title_el, &text);
    dom_node_unref(title_el);
    if (!text) return JS_NewString(jsc, "");
    return domstr_to_jsval(jsc, text);
}

static JSValue js_doc_get_readyState(JSContext *jsc, JSValue this_val)
{
    return JS_NewString(jsc, "complete");
}

static JSValue js_doc_addEventListener(JSContext *jsc, JSValue this_val,
                                        int argc, JSValue *argv)
{
    node_wrapper_t *w = unwrap_node_w(jsc, this_val);
    if (!w || w->thread->closed || argc < 2) return JS_UNDEFINED;
    if (!JS_IsFunction(jsc, argv[1])) return JS_UNDEFINED;

    const char *type = JS_ToCString(jsc, argv[0]);
    if (!type) return JS_UNDEFINED;
    register_dom_listener(jsc, w->thread,
                          (struct dom_node *)w->thread->doc, type, argv[1]);
    JS_FreeCString(jsc, type);
    return JS_UNDEFINED;
}

/* ── Document function list ──────────────────────────────────────────────── */

static const JSCFunctionListEntry dom_doc_proto[] = {
    JS_CFUNC_DEF("getElementById",   1, js_doc_getElementById),
    JS_CFUNC_DEF("querySelector",    1, js_doc_querySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, js_doc_querySelectorAll),
    JS_CFUNC_DEF("createElement",    1, js_doc_createElement),
    JS_CFUNC_DEF("createTextNode",   1, js_doc_createTextNode),
    JS_CFUNC_DEF("addEventListener", 2, js_doc_addEventListener),
    JS_CGETSET_DEF("body",           js_doc_get_body,       NULL),
    JS_CGETSET_DEF("title",          js_doc_get_title,      NULL),
    JS_CGETSET_DEF("readyState",     js_doc_get_readyState, NULL),
};

/* ── Timer implementation ────────────────────────────────────────────────── */

static void timer_add(struct jsthread *t, int id, bool repeating, int ms,
                      JSContext *jsc, JSValue fn)
{
    js_timer_t *e = calloc(1, sizeof(*e));
    if (!e) return;
    e->id        = id;
    e->repeating = repeating;
    e->ms        = ms;
    e->jsc       = jsc;
    e->fn        = JS_DupValue(jsc, fn);
    gettimeofday(&e->fire_at, NULL);
    e->fire_at.tv_sec  += (time_t)(ms / 1000);
    e->fire_at.tv_usec += (long)((ms % 1000) * 1000);
    if (e->fire_at.tv_usec >= 1000000) {
        e->fire_at.tv_sec++;
        e->fire_at.tv_usec -= 1000000;
    }
    e->next   = t->timers;
    t->timers = e;
}

static JSValue js_window_setTimeout(JSContext *jsc, JSValue this_val,
                                     int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsFunction(jsc, argv[0])) return JS_NewInt32(jsc, 0);
    int ms = (argc >= 2) ? (int)JS_VALUE_GET_INT(argv[1]) : 0;
    if (ms < 0) ms = 0;

    /* Find the thread owning this context */
    struct jsthread *t = g_thread_list;
    while (t && t->jsc != jsc) t = t->next;
    if (!t || t->closed) return JS_NewInt32(jsc, 0);

    int id = t->next_timer_id++;
    timer_add(t, id, false, ms, jsc, argv[0]);
    return JS_NewInt32(jsc, id);
}

static JSValue js_window_setInterval(JSContext *jsc, JSValue this_val,
                                      int argc, JSValue *argv)
{
    if (argc < 1 || !JS_IsFunction(jsc, argv[0])) {
        js_uart("setInterval: no fn\n");
        return JS_NewInt32(jsc, 0);
    }
    int ms = 100;
    if (argc >= 2)
        JS_ToInt32(jsc, &ms, argv[1]);
    if (ms < 1) ms = 1;

    struct jsthread *t = g_thread_list;
    while (t && t->jsc != jsc) t = t->next;
    if (!t || t->closed) return JS_NewInt32(jsc, 0);

    int id = t->next_timer_id++;
    timer_add(t, id, true, ms, jsc, argv[0]);
    return JS_NewInt32(jsc, id);
}

static JSValue js_window_clearTimeout(JSContext *jsc, JSValue this_val,
                                       int argc, JSValue *argv)
{
    if (argc < 1) return JS_UNDEFINED;
    int id = JS_VALUE_GET_INT(argv[0]);

    struct jsthread *t = g_thread_list;
    while (t && t->jsc != jsc) t = t->next;
    if (!t) return JS_UNDEFINED;

    js_timer_t **pp = &t->timers;
    while (*pp) {
        js_timer_t *e = *pp;
        if (e->id == id) {
            *pp = e->next;
            JS_FreeValue(jsc, e->fn);
            free(e);
            return JS_UNDEFINED;
        }
        pp = &e->next;
    }
    return JS_UNDEFINED;
}

/* clearInterval and clearTimeout are the same in this implementation */

static JSValue js_window_alert(JSContext *jsc, JSValue this_val,
                                int argc, JSValue *argv)
{
    if (argc >= 1) {
        const char *msg = JS_ToCString(jsc, argv[0]);
        if (msg) {
            NSLOG(netsurf, INFO, "JS alert: %s", msg);
            JS_FreeCString(jsc, msg);
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_console_log(JSContext *jsc, JSValue this_val,
                               int argc, JSValue *argv)
{
    (void)this_val;
    char buf[512];
    int pos = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(jsc, argv[i]);
        if (s) {
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos - 2,
                            "%s%s", i ? " " : "", s);
            JS_FreeCString(jsc, s);
        }
    }
    buf[pos] = '\0';
    NSLOG(netsurf, INFO, "JS console: %s", buf);
    return JS_UNDEFINED;
}

/* timeval_le: returns true if a <= b */
static bool timeval_le(const struct timeval *a, const struct timeval *b)
{
    if (a->tv_sec != b->tv_sec) return a->tv_sec < b->tv_sec;
    return a->tv_usec <= b->tv_usec;
}

/* ── js_timers_tick() — called once per frame from browser_per_frame() ────── */

void js_timers_tick(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);

    struct jsthread *t = g_thread_list;
    while (t) {
        if (t->closed) { t = t->next; continue; }

        js_timer_t **pp = &t->timers;
        while (*pp) {
            js_timer_t *e = *pp;
            if (timeval_le(&e->fire_at, &now)) {
                /* Unlink before firing */
                *pp = e->next;
                JSValue result = JS_Call(e->jsc, e->fn, JS_UNDEFINED, 0, NULL);
                if (JS_IsException(result)) {
                    JSValue exc = JS_GetException(e->jsc);
                    const char *msg = JS_ToCString(e->jsc, exc);
                    NSLOG(netsurf, WARNING, "JS timer exception: %s",
                          msg ? msg : "(unknown)");
                    if (msg) JS_FreeCString(e->jsc, msg);
                    JS_FreeValue(e->jsc, exc);
                }
                JS_FreeValue(e->jsc, result);
                nsaether_dirty = true;

                if (e->repeating) {
                    /* Re-arm: fire_at = now + ms */
                    e->fire_at = now;
                    e->fire_at.tv_sec  += (time_t)(e->ms / 1000);
                    e->fire_at.tv_usec += (long)((e->ms % 1000) * 1000);
                    if (e->fire_at.tv_usec >= 1000000) {
                        e->fire_at.tv_sec++;
                        e->fire_at.tv_usec -= 1000000;
                    }
                    e->next   = t->timers;
                    t->timers = e;
                    /* pp already advanced (e was unlinked) — continue from head */
                    pp = &t->timers;
                } else {
                    JS_FreeValue(e->jsc, e->fn);
                    free(e);
                    /* pp already advanced */
                }
            } else {
                pp = &e->next;
            }
        }
        t = t->next;
    }
}

/* ── Global setup per thread ─────────────────────────────────────────────── */

static void setup_window(JSContext *jsc, struct jsthread *thread,
                          struct dom_document *doc)
{
    JSValue global = JS_GetGlobalObject(jsc);

    /* console object */
    {
        JSValue con = JS_NewObject(jsc);
        JSValue fn = JS_NewCFunction(jsc, js_console_log, "log", 1);
        JS_SetPropertyStr(jsc, con, "log",   JS_DupValue(jsc, fn));
        JS_SetPropertyStr(jsc, con, "warn",  JS_DupValue(jsc, fn));
        JS_SetPropertyStr(jsc, con, "error", fn);
        JS_SetPropertyStr(jsc, global, "console", con);
    }

    /* window.alert */
    JS_SetPropertyStr(jsc, global, "alert",
                      JS_NewCFunction(jsc, js_window_alert, "alert", 1));

    /* window.setTimeout / setInterval / clearTimeout / clearInterval */
    JS_SetPropertyStr(jsc, global, "setTimeout",
                      JS_NewCFunction(jsc, js_window_setTimeout, "setTimeout", 2));
    JS_SetPropertyStr(jsc, global, "setInterval",
                      JS_NewCFunction(jsc, js_window_setInterval, "setInterval", 2));
    JS_SetPropertyStr(jsc, global, "clearTimeout",
                      JS_NewCFunction(jsc, js_window_clearTimeout, "clearTimeout", 1));
    JS_SetPropertyStr(jsc, global, "clearInterval",
                      JS_NewCFunction(jsc, js_window_clearTimeout, "clearInterval", 1));

    /* window.location */
    {
        JSValue loc = JS_NewObject(jsc);
        JS_SetPropertyStr(jsc, loc, "href", JS_NewString(jsc, nsaether_url));
        JS_SetPropertyStr(jsc, global, "location", loc);
    }

    /* document object — wraps dom_document* */
    if (doc) {
        node_wrapper_t *w = calloc(1, sizeof(*w));
        if (w) {
            w->thread = thread;
            w->node   = (struct dom_node *)doc;  /* not ref'd — owned by NetSurf */
            JSValue doc_obj = JS_NewObjectClass(jsc, g_dom_doc_class_id);
            if (!JS_IsException(doc_obj)) {
                JS_SetOpaque(doc_obj, w);
                JS_SetPropertyStr(jsc, global, "document", doc_obj);
            } else {
                free(w);
            }
        }
    }

    /* window === window.window (self-referential) */
    JS_SetPropertyStr(jsc, global, "window", JS_DupValue(jsc, global));

    JS_FreeValue(jsc, global);
}

/* ── global lifecycle ─────────────────────────────────────────────────────── */

void js_initialise(void)
{
    JS_NewClassID(&g_dom_node_class_id);
    JS_NewClassID(&g_dom_doc_class_id);
    /* Register text/javascript and application/javascript as CONTENT_JS.
     * dukky.c is excluded from our build so this call is missing otherwise. */
    javascript_init();
    NSLOG(netsurf, INFO, "QuickJS JS engine active (Iteration 2 — DOM bindings)");
}

void js_finalise(void) { }

/* ── heap (JSRuntime) ─────────────────────────────────────────────────────── */

nserror js_newheap(int timeout, jsheap **heap_out)
{
    js_uart("js_newheap: called\n");
    jsheap *h = calloc(1, sizeof(*h));
    if (!h) return NSERROR_NOMEM;

    h->rt = JS_NewRuntime();
    if (!h->rt) { free(h); return NSERROR_NOMEM; }

    JS_SetMemoryLimit(h->rt, 16UL * 1024 * 1024);
    JS_SetMaxStackSize(h->rt, 64 * 1024);
    h->timeout = timeout;

    /* Register DOM node class (finalizer) */
    static const JSClassDef dom_node_class_def = {
        "HTMLElement",
        .finalizer = dom_node_js_finalizer,
    };
    static const JSClassDef dom_doc_class_def = {
        "HTMLDocument",
        .finalizer = dom_doc_js_finalizer,
    };
    JS_NewClass(h->rt, g_dom_node_class_id, &dom_node_class_def);
    JS_NewClass(h->rt, g_dom_doc_class_id,  &dom_doc_class_def);

    *heap_out = h;
    return NSERROR_OK;
}

void js_destroyheap(jsheap *heap)
{
    if (!heap) return;
    JS_FreeRuntime(heap->rt);
    free(heap);
}

/* ── thread (JSContext = one page) ───────────────────────────────────────── */

nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv,
                     jsthread **thread_out)
{
    jsthread *t = calloc(1, sizeof(*t));
    if (!t) return NSERROR_NOMEM;

    t->jsc = JS_NewContext(heap->rt);
    if (!t->jsc) { free(t); return NSERROR_NOMEM; }

    t->heap           = heap;
    t->closed         = false;
    t->next_timer_id  = 1;

    /*
     * doc_priv is html_content* (struct content* that is actually html_content*).
     * win_priv is browser_window*.
     * Extract the real dom_document* from the html_content struct and
     * store html_content* for direct box-tree text updates.
     */
    {
        html_content *html = (html_content *)doc_priv;
        t->doc  = (html && html->document) ? html->document : NULL;
        t->html = html;
    }

    js_uart("js_newthread: called\n");
    NSLOG(netsurf, INFO, "js_newthread: win=%p doc_priv=%p dom_doc=%p",
          win_priv, doc_priv, (void *)t->doc);

    /* Set up element class prototype for this context */
    {
        JSValue proto = JS_NewObject(t->jsc);
        JS_SetPropertyFunctionList(t->jsc, proto, dom_element_proto,
                                   sizeof(dom_element_proto) / sizeof(dom_element_proto[0]));
        JS_SetClassProto(t->jsc, g_dom_node_class_id, proto);
    }
    /* Set up document class prototype for this context */
    {
        JSValue proto = JS_NewObject(t->jsc);
        JS_SetPropertyFunctionList(t->jsc, proto, dom_doc_proto,
                                   sizeof(dom_doc_proto) / sizeof(dom_doc_proto[0]));
        JS_SetClassProto(t->jsc, g_dom_doc_class_id, proto);
    }

    setup_window(t->jsc, t, t->doc);

    /* Add to global thread list */
    t->next       = g_thread_list;
    g_thread_list = t;

    *thread_out = t;
    return NSERROR_OK;
}

nserror js_closethread(jsthread *thread)
{
    if (!thread) return NSERROR_OK;
    thread->closed = true;

    /* Free all event listeners */
    js_listener_t *l = thread->listeners;
    while (l) {
        js_listener_t *next = l->next;
        if (l->dom_listen) dom_event_listener_unref(l->dom_listen);
        if (l->ctx) {
            JS_FreeValue(thread->jsc, l->ctx->fn);
            free(l->ctx);
        }
        JS_FreeValue(thread->jsc, l->fn);
        free(l);
        l = next;
    }
    thread->listeners = NULL;

    /* Free all pending timers */
    js_timer_t *timer = thread->timers;
    while (timer) {
        js_timer_t *next = timer->next;
        JS_FreeValue(thread->jsc, timer->fn);
        free(timer);
        timer = next;
    }
    thread->timers = NULL;

    return NSERROR_OK;
}

void js_destroythread(jsthread *thread)
{
    if (!thread) return;

    /* Remove from global list */
    jsthread **pp = &g_thread_list;
    while (*pp && *pp != thread) pp = &(*pp)->next;
    if (*pp) *pp = thread->next;

    /* Free event listeners — unref DOM listeners and free JS function refs */
    js_listener_t *l = thread->listeners;
    while (l) {
        js_listener_t *next = l->next;
        if (l->dom_listen) dom_event_listener_unref(l->dom_listen);
        if (l->ctx) {
            JS_FreeValue(thread->jsc, l->ctx->fn);
            free(l->ctx);
        }
        JS_FreeValue(thread->jsc, l->fn);
        free(l);
        l = next;
    }
    thread->listeners = NULL;

    /* GC runs here — finalizers call dom_node_unref on wrapped nodes */
    JS_FreeContext(thread->jsc);
    free(thread);
}

/* ── script execution ─────────────────────────────────────────────────────── */

bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen, const char *name)
{
    js_uart("js_exec: called\n");
    if (!thread || thread->closed || !txt || txtlen == 0) return false;

    JSValue val = JS_Eval(thread->jsc,
                          (const char *)txt, txtlen,
                          name ? name : "<script>",
                          JS_EVAL_TYPE_GLOBAL);

    if (JS_IsException(val)) {
        JSValue exc = JS_GetException(thread->jsc);
        const char *msg = JS_ToCString(thread->jsc, exc);
        NSLOG(netsurf, WARNING, "JS exception in %s: %s",
              name ? name : "<script>", msg ? msg : "(unknown)");
        if (msg) JS_FreeCString(thread->jsc, msg);
        JS_FreeValue(thread->jsc, exc);
        JS_FreeValue(thread->jsc, val);
        return false;
    }

    JS_FreeValue(thread->jsc, val);
    return true;
}

/* ── event dispatch ───────────────────────────────────────────────────────── */

bool js_fire_event(jsthread *thread, const char *type,
                   struct dom_document *doc, struct dom_node *target)
{
    if (!thread || thread->closed || !type) return false;

    /* Build a minimal Event object */
    JSValue event_obj = JS_NewObject(thread->jsc);
    JS_SetPropertyStr(thread->jsc, event_obj, "type",
                      JS_NewString(thread->jsc, type));
    JS_SetPropertyStr(thread->jsc, event_obj, "bubbles",   JS_FALSE);
    JS_SetPropertyStr(thread->jsc, event_obj, "cancelable", JS_FALSE);
    if (target)
        JS_SetPropertyStr(thread->jsc, event_obj, "target",
                          wrap_node(thread->jsc, thread, target));

    bool fired = false;
    js_listener_t *l = thread->listeners;
    while (l) {
        bool matches_node = (l->node == target) ||
                            (target == NULL && l->node == (struct dom_node *)doc);
        if (matches_node && strcmp(l->type, type) == 0) {
            JSValue args[1] = { JS_DupValue(thread->jsc, event_obj) };
            JSValue r = JS_Call(thread->jsc, l->fn, JS_UNDEFINED, 1, args);
            JS_FreeValue(thread->jsc, args[0]);
            if (JS_IsException(r)) {
                JSValue exc = JS_GetException(thread->jsc);
                const char *msg = JS_ToCString(thread->jsc, exc);
                NSLOG(netsurf, WARNING, "JS event '%s' exception: %s",
                      type, msg ? msg : "(unknown)");
                if (msg) JS_FreeCString(thread->jsc, msg);
                JS_FreeValue(thread->jsc, exc);
            }
            JS_FreeValue(thread->jsc, r);
            fired = true;
        }
        l = l->next;
    }

    JS_FreeValue(thread->jsc, event_obj);
    return fired;
}

bool js_dom_event_add_listener(jsthread *thread,
                               struct dom_document *document,
                               struct dom_node *node,
                               struct dom_string *event_type_dom,
                               void *js_funcval)
{
    if (!thread || thread->closed || !node || !event_type_dom || !js_funcval)
        return false;

    JSValue *fn_ptr = (JSValue *)js_funcval;
    if (!JS_IsFunction(thread->jsc, *fn_ptr)) return false;

    js_listener_t *entry = calloc(1, sizeof(*entry));
    if (!entry) return false;

    entry->node   = node;
    strncpy(entry->type, dom_string_data(event_type_dom),
            sizeof(entry->type) - 1);
    entry->fn     = JS_DupValue(thread->jsc, *fn_ptr);
    entry->thread = thread;
    entry->next   = thread->listeners;
    thread->listeners = entry;
    return true;
}

void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
    if (!thread || thread->closed || !node) return;

    /* Scan on* attributes and register handlers */
    static const struct { const char *attr; const char *type; } on_attrs[] = {
        { "onclick",    "click"    },
        { "onload",     "load"     },
        { "onchange",   "change"   },
        { "oninput",    "input"    },
        { "onsubmit",   "submit"   },
        { "onkeydown",  "keydown"  },
        { "onkeyup",    "keyup"    },
        { "onmouseover","mouseover"},
        { "onmouseout", "mouseout" },
        { NULL, NULL }
    };

    for (int i = 0; on_attrs[i].attr; i++) {
        dom_string *dname = cstr_to_domstr(on_attrs[i].attr);
        if (!dname) continue;
        dom_string *val = NULL;
        dom_element_get_attribute(node, dname, &val);
        dom_string_unref(dname);
        if (!val) continue;

        /* Compile attribute value as function body */
        const char *body = dom_string_data(val);
        size_t blen = dom_string_byte_length(val);
        char *src = malloc(blen + 32);
        if (src) {
            snprintf(src, blen + 32, "(function(event){%.*s})", (int)blen, body);
            JSValue fn = JS_Eval(thread->jsc, src, strlen(src),
                                  on_attrs[i].attr, JS_EVAL_TYPE_GLOBAL);
            if (!JS_IsException(fn) && JS_IsFunction(thread->jsc, fn)) {
                js_listener_t *entry = calloc(1, sizeof(*entry));
                if (entry) {
                    entry->node   = (struct dom_node *)node;
                    strncpy(entry->type, on_attrs[i].type, sizeof(entry->type) - 1);
                    entry->fn     = fn; /* ownership transferred */
                    entry->thread = thread;
                    entry->next   = thread->listeners;
                    thread->listeners = entry;
                } else {
                    JS_FreeValue(thread->jsc, fn);
                }
            } else {
                if (JS_IsException(fn))
                    JS_FreeValue(thread->jsc, JS_GetException(thread->jsc));
                JS_FreeValue(thread->jsc, fn);
            }
            free(src);
        }
        dom_string_unref(val);
    }
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
    (void)thread; (void)evt;
}
