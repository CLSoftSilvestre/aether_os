/*
 * downloads_aether.h — I5.4 Download manager API
 *
 * fetch_http_aether calls downloads_cache_response() after each 200 response.
 * main.c calls download_save_page() on Ctrl+S to flush the cache to FAT32.
 *
 * Storage: /downloads/<filename>  (directory pre-created by make_disk.sh)
 * Max body cached: 2 MB.  Larger pages are silently skipped.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Called from fetch_http_aether after each successful 200 response.
 * Copies body into an internal buffer (up to 2 MB). */
void downloads_cache_response(const char *url, const uint8_t *body,
                               size_t body_len, const char *mime);

/* Write the cached response to /downloads/<derived-filename>.
 * Returns 1 on success, 0 if no cached body or file write fails.
 * out_path receives the full path written (e.g. "/downloads/index.html"). */
int download_save_page(char *out_path, int out_max);
