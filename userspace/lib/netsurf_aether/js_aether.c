/*
 * Phase 7.5.4 — QuickJS ↔ NetSurf JS engine bridge
 *
 * Implements the interface declared in:
 *   content/handlers/javascript/js.h
 *
 * NetSurf's JS model (3.11):
 *   jsheap   — one per browser window (shared by all browsing contexts)
 *   jsthread — one per HTML page (window object + document)
 *
 * QuickJS mapping:
 *   jsheap   → JSRuntime  (one QuickJS runtime per browser window)
 *   jsthread → JSContext  (one QuickJS context per page)
 *
 * Phase 7.5 scope:
 *   ✓  js_initialise / js_finalise
 *   ✓  js_newheap / js_destroyheap
 *   ✓  js_newthread / js_closethread / js_destroythread
 *   ✓  js_exec           — evaluates <script> content via JS_Eval
 *   ✓  console.log       → UART (minimal Web API)
 *   ✗  js_fire_event     — stubs (Iteration 2: DOM bindings)
 *   ✗  js_dom_event_add_listener, js_handle_new_element, js_event_cleanup
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "quickjs/quickjs.h"
#include "content/handlers/javascript/js.h"
#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf_aether.h"

/* ── opaque type implementations ─────────────────────────────────────────── */

struct jsheap {
    JSRuntime *rt;
    int        timeout; /* seconds, unused until Iteration 2 */
};

struct jsthread {
    JSContext *jsc;
    jsheap    *heap;
    bool       closed;
};

/* ── console.log → UART ──────────────────────────────────────────────────── */

static JSValue js_console_log(JSContext *jsc, JSValue this_val,
                              int argc, JSValue *argv)
{
    (void)this_val;
    char buf[512];
    int  pos = 0;
    for (int i = 0; i < argc; i++) {
        const char *s = JS_ToCString(jsc, argv[i]);
        if (s) {
            pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos - 2,
                            "%s%s", i ? " " : "", s);
            JS_FreeCString(jsc, s);
        }
    }
    buf[pos++] = '\n';
    buf[pos]   = '\0';
    NSLOG(netsurf, INFO, "JS console: %s", buf);
    return JS_UNDEFINED;
}

/* Wire a minimal console object into a fresh context */
static void setup_console(JSContext *jsc)
{
    JSValue global  = JS_GetGlobalObject(jsc);
    JSValue console = JS_NewObject(jsc);

    JSValue fn = JS_NewCFunction(jsc, js_console_log, "log", 1);
    JS_SetPropertyStr(jsc, console, "log",   JS_DupValue(jsc, fn));
    JS_SetPropertyStr(jsc, console, "warn",  JS_DupValue(jsc, fn));
    JS_SetPropertyStr(jsc, console, "error", fn);

    JS_SetPropertyStr(jsc, global, "console", console);
    JS_FreeValue(jsc, global);
}

/* ── global lifecycle ────────────────────────────────────────────────────── */

void js_initialise(void)
{
    NSLOG(netsurf, INFO, "QuickJS JS engine active (Phase 7.5)");
}

void js_finalise(void)
{
    /* nothing — every heap owns its runtime */
}

/* ── heap (JSRuntime) ────────────────────────────────────────────────────── */

nserror js_newheap(int timeout, jsheap **heap_out)
{
    jsheap *h = calloc(1, sizeof(*h));
    if (!h) return NSERROR_NOMEM;

    h->rt = JS_NewRuntime();
    if (!h->rt) { free(h); return NSERROR_NOMEM; }

    /* 16 MB per browser-window heap — generous for MVP */
    JS_SetMemoryLimit(h->rt, 16UL * 1024 * 1024);
    JS_SetMaxStackSize(h->rt, 64 * 1024);

    h->timeout = timeout;
    *heap_out  = h;
    return NSERROR_OK;
}

void js_destroyheap(jsheap *heap)
{
    if (!heap) return;
    JS_FreeRuntime(heap->rt);
    free(heap);
}

/* ── thread (JSContext = one page's global object) ───────────────────────── */

nserror js_newthread(jsheap *heap, void *win_priv, void *doc_priv,
                     jsthread **thread_out)
{
    (void)win_priv; /* TODO-I2: attach Window object to global */
    (void)doc_priv; /* TODO-I2: attach Document object         */

    jsthread *t = calloc(1, sizeof(*t));
    if (!t) return NSERROR_NOMEM;

    t->jsc = JS_NewContext(heap->rt);
    if (!t->jsc) { free(t); return NSERROR_NOMEM; }

    t->heap   = heap;
    t->closed = false;

    setup_console(t->jsc);

    *thread_out = t;
    return NSERROR_OK;
}

nserror js_closethread(jsthread *thread)
{
    if (!thread) return NSERROR_OK;
    thread->closed = true;
    /* TODO-I2: disconnect callbacks, event listeners */
    return NSERROR_OK;
}

void js_destroythread(jsthread *thread)
{
    if (!thread) return;
    JS_FreeContext(thread->jsc);
    free(thread);
}

/* ── script execution ────────────────────────────────────────────────────── */

bool js_exec(jsthread *thread, const uint8_t *txt, size_t txtlen,
             const char *name)
{
    if (!thread || thread->closed || !txt || txtlen == 0)
        return false;

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

/* ── event dispatch stubs (Iteration 2) ──────────────────────────────────── */

bool js_fire_event(jsthread *thread, const char *type,
                   struct dom_document *doc, struct dom_node *target)
{
    (void)thread; (void)type; (void)doc; (void)target;
    /* TODO-I2: look up listeners, synthesise Event, call handlers */
    return false;
}

bool js_dom_event_add_listener(jsthread *thread,
                               struct dom_document *document,
                               struct dom_node *node,
                               struct dom_string *event_type_dom,
                               void *js_funcval)
{
    (void)thread; (void)document; (void)node;
    (void)event_type_dom; (void)js_funcval;
    /* TODO-I2 */
    return false;
}

void js_handle_new_element(jsthread *thread, struct dom_element *node)
{
    (void)thread; (void)node;
    /* TODO-I2: scan on* attributes, register listeners */
}

void js_event_cleanup(jsthread *thread, struct dom_event *evt)
{
    (void)thread; (void)evt;
    /* TODO-I2 */
}
