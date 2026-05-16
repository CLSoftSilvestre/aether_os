/*
 * nslog filter stub for AetherOS.
 *
 * libnslog's filter.c requires bison-generated headers (filter-parser.h,
 * filter-lexer.h) that are not shipped in the source tree.  We exclude
 * filter.c from the build and replace it with these no-op stubs:
 *   - nslog__filter_matches() always returns true (log everything)
 *   - all public nslog_filter_*() functions return NSLOG_NO_ERROR
 */

#include <stddef.h>
#include <stdbool.h>
#include "nslog/nslog.h"

bool nslog__filter_matches(nslog_entry_context_t *ctx)
{
    (void)ctx;
    return true;
}

nslog_error nslog_filter_category_new(const char *catname,
                                      nslog_filter_t **filter)
{
    (void)catname; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_level_new(nslog_level level,
                                   nslog_filter_t **filter)
{
    (void)level; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_filename_new(const char *filename,
                                      nslog_filter_t **filter)
{
    (void)filename; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_dirname_new(const char *dirname,
                                     nslog_filter_t **filter)
{
    (void)dirname; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_funcname_new(const char *funcname,
                                      nslog_filter_t **filter)
{
    (void)funcname; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_and_new(nslog_filter_t *left,
                                  nslog_filter_t *right,
                                  nslog_filter_t **filter)
{
    (void)left; (void)right; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_or_new(nslog_filter_t *left,
                                 nslog_filter_t *right,
                                 nslog_filter_t **filter)
{
    (void)left; (void)right; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_xor_new(nslog_filter_t *left,
                                  nslog_filter_t *right,
                                  nslog_filter_t **filter)
{
    (void)left; (void)right; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_not_new(nslog_filter_t *input,
                                  nslog_filter_t **filter)
{
    (void)input; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_set_active(nslog_filter_t *filter,
                                     nslog_filter_t **previous)
{
    (void)filter; (void)previous;
    return NSLOG_NO_ERROR;
}

nslog_error nslog_filter_from_text(const char *input,
                                    nslog_filter_t **filter)
{
    (void)input; (void)filter;
    return NSLOG_NO_ERROR;
}

nslog_filter_t *nslog_filter_ref(nslog_filter_t *filter)
{
    return filter;
}

nslog_filter_t *nslog_filter_unref(nslog_filter_t *filter)
{
    (void)filter;
    return NULL;
}
