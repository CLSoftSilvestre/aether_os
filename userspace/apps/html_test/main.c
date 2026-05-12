/*
 * html_test — Phase 7.3 integration test (Task 7.3.7)
 *
 * Verifies the NetSurf support libraries compile and link correctly by
 * exercising:
 *   1. libwapcaplet — string interning + caseless equality
 *   2. libhubbub    — HTML5 parser with a minimal tree-handler callback set
 *
 * Run in AetherOS:  html_test
 * Pass criteria:    sys_exit(0) + "=== All tests PASSED ==="
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <libwapcaplet/libwapcaplet.h>

#include <hubbub/hubbub.h>
#include <hubbub/parser.h>
/* hubbub/parser.h pulls in hubbub/tree.h, hubbub/types.h, hubbub/functypes.h */

/* ── Minimal tree handler — counts created elements ─────────────────────── */

static int g_elements;

static hubbub_error cb_create_comment(void *ctx,
        const hubbub_string *data, void **result)
{ (void)ctx; (void)data; *result = (void *)1; return HUBBUB_OK; }

static hubbub_error cb_create_doctype(void *ctx,
        const hubbub_doctype *doctype, void **result)
{ (void)ctx; (void)doctype; *result = (void *)1; return HUBBUB_OK; }

static hubbub_error cb_create_element(void *ctx,
        const hubbub_tag *tag, void **result)
{
    (void)ctx; (void)tag;
    g_elements++;
    *result = (void *)(uintptr_t)(g_elements + 10);
    return HUBBUB_OK;
}

static hubbub_error cb_create_text(void *ctx,
        const hubbub_string *data, void **result)
{ (void)ctx; (void)data; *result = (void *)2; return HUBBUB_OK; }

static hubbub_error cb_ref_node(void *ctx, void *node)
{ (void)ctx; (void)node; return HUBBUB_OK; }

static hubbub_error cb_unref_node(void *ctx, void *node)
{ (void)ctx; (void)node; return HUBBUB_OK; }

static hubbub_error cb_append_child(void *ctx,
        void *parent, void *child, void **result)
{ (void)ctx; (void)parent; *result = child; return HUBBUB_OK; }

static hubbub_error cb_insert_before(void *ctx,
        void *parent, void *child, void *ref, void **result)
{ (void)ctx; (void)parent; (void)ref; *result = child; return HUBBUB_OK; }

static hubbub_error cb_remove_child(void *ctx,
        void *parent, void *child, void **result)
{ (void)ctx; (void)parent; *result = child; return HUBBUB_OK; }

static hubbub_error cb_clone_node(void *ctx,
        void *node, bool deep, void **result)
{ (void)ctx; (void)deep; *result = node; return HUBBUB_OK; }

static hubbub_error cb_reparent_children(void *ctx, void *node, void *new_parent)
{ (void)ctx; (void)node; (void)new_parent; return HUBBUB_OK; }

static hubbub_error cb_get_parent(void *ctx,
        void *node, bool element_only, void **result)
{ (void)ctx; (void)node; (void)element_only; *result = NULL; return HUBBUB_OK; }

static hubbub_error cb_has_children(void *ctx, void *node, bool *result)
{ (void)ctx; (void)node; *result = false; return HUBBUB_OK; }

static hubbub_error cb_form_associate(void *ctx, void *form, void *node)
{ (void)ctx; (void)form; (void)node; return HUBBUB_OK; }

static hubbub_error cb_add_attributes(void *ctx, void *node,
        const hubbub_attribute *attrs, uint32_t n)
{ (void)ctx; (void)node; (void)attrs; (void)n; return HUBBUB_OK; }

static hubbub_error cb_set_quirks_mode(void *ctx, hubbub_quirks_mode mode)
{ (void)ctx; (void)mode; return HUBBUB_OK; }

static hubbub_error cb_encoding_change(void *ctx, const char *enc)
{ (void)ctx; (void)enc; return HUBBUB_OK; }

static hubbub_error cb_complete_script(void *ctx, void *script)
{ (void)ctx; (void)script; return HUBBUB_OK; }

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== AetherOS Phase 7.3 NetSurf libs integration test ===\n\n");
    int ok = 1;

    /* ── 1. libwapcaplet — string interning ─────────────────────────────── */
    {
        lwc_string *s1 = NULL, *s2 = NULL;
        lwc_error err;

        err = lwc_intern_string("AetherOS", 8, &s1);
        if (err != lwc_error_ok) {
            fprintf(stderr, "[wapcaplet] intern failed: %d\n", (int)err);
            ok = 0;
        } else {
            err = lwc_intern_string("AetherOS", 8, &s2);
            if (err != lwc_error_ok || s1 != s2) {
                fprintf(stderr, "[wapcaplet] identity check failed\n");
                ok = 0;
            } else {
                printf("[wapcaplet] interned \"%.*s\" len=%zu — same ptr OK\n",
                       (int)lwc_string_length(s1),
                       lwc_string_data(s1),
                       lwc_string_length(s1));
            }
            lwc_string_unref(s1);
            lwc_string_unref(s2);
        }

        /* Caseless comparison */
        lwc_string *upper = NULL, *lower = NULL;
        bool eq = false;
        if (lwc_intern_string("HELLO", 5, &upper) == lwc_error_ok &&
            lwc_intern_string("hello", 5, &lower) == lwc_error_ok) {
            err = lwc_string_caseless_isequal(upper, lower, &eq);
            if (err != lwc_error_ok || !eq) {
                fprintf(stderr, "[wapcaplet] caseless cmp failed\n");
                ok = 0;
            } else {
                printf("[wapcaplet] caseless isequal(HELLO, hello) = true OK\n");
            }
        }
        if (upper) lwc_string_unref(upper);
        if (lower) lwc_string_unref(lower);
    }

    /* ── 2. libhubbub — HTML5 parsing ───────────────────────────────────── */
    {
        static const char html[] =
            "<html><head><title>AetherOS</title></head>"
            "<body><p>Hello, <em>AetherOS!</em></p></body></html>";

        hubbub_parser *parser = NULL;
        hubbub_error err;
        g_elements = 0;

        err = hubbub_parser_create("UTF-8", false, &parser);
        if (err != HUBBUB_OK) {
            fprintf(stderr, "[hubbub] parser_create failed: %d\n", (int)err);
            ok = 0;
        } else {
            static const hubbub_tree_handler tree = {
                .create_comment    = cb_create_comment,
                .create_doctype    = cb_create_doctype,
                .create_element    = cb_create_element,
                .create_text       = cb_create_text,
                .ref_node          = cb_ref_node,
                .unref_node        = cb_unref_node,
                .append_child      = cb_append_child,
                .insert_before     = cb_insert_before,
                .remove_child      = cb_remove_child,
                .clone_node        = cb_clone_node,
                .reparent_children = cb_reparent_children,
                .get_parent        = cb_get_parent,
                .has_children      = cb_has_children,
                .form_associate    = cb_form_associate,
                .add_attributes    = cb_add_attributes,
                .set_quirks_mode   = cb_set_quirks_mode,
                .encoding_change   = cb_encoding_change,
                .complete_script   = cb_complete_script,
                .ctx               = NULL,
            };

            hubbub_parser_optparams params;

            params.tree_handler = (hubbub_tree_handler *)&tree;
            hubbub_parser_setopt(parser, HUBBUB_PARSER_TREE_HANDLER, &params);

            params.document_node = (void *)0x100; /* fake root document node */
            hubbub_parser_setopt(parser, HUBBUB_PARSER_DOCUMENT_NODE, &params);

            err = hubbub_parser_parse_chunk(parser,
                    (const uint8_t *)html, sizeof(html) - 1);
            if (err != HUBBUB_OK) {
                fprintf(stderr, "[hubbub] parse_chunk failed: %d\n", (int)err);
                ok = 0;
            }

            hubbub_parser_completed(parser);
            hubbub_parser_destroy(parser);

            /* html, head, title, body, p, em = 6 minimum */
            printf("[hubbub] parsed HTML5 — %d elements created\n", g_elements);
            if (g_elements < 4) {
                fprintf(stderr, "[hubbub] too few elements (expected >=4, got %d)\n",
                        g_elements);
                ok = 0;
            }
        }
    }

    if (ok) printf("\n=== All tests PASSED ===\n");
    else    printf("\n=== Some tests FAILED ===\n");
    return ok ? 0 : 1;
}
