/*
 * AetherOS NetSurf option defaults
 *
 * This file is #included by NetSurf's nsoption.c (via
 * -DNETSURF_OPTIONS_HEADER) to append platform-specific option
 * definitions to the global option table.
 *
 * For Phase 7.5 we use all NetSurf defaults and only override the
 * settings that would otherwise crash on bare metal.
 *
 * Syntax: NSOPTION_BOOL(name, default)
 *         NSOPTION_INTEGER(name, default)
 *         NSOPTION_STRING(name, default)  — default is char* or NULL
 */

/* JavaScript: enabled (Phase 7.5 QuickJS bridge compiles; DOM bindings
   arrive in Iteration 2 — set false until then for safe MVP) */
NSOPTION_BOOL(enable_javascript, false)

/* Memory limits — generous on Pi 5, sane in QEMU -m 1G */
NSOPTION_INTEGER(memory_cache_size, 16 * 1024 * 1024)   /* 16 MB */

/* Cache paths on FAT32 disk */
NSOPTION_STRING(disc_cache_path, "/tmp/nscache/")
NSOPTION_INTEGER(disc_cache_size, 64 * 1024 * 1024)     /* 64 MB */

/* Cookie storage — /tmp/ on FAT32 is writable */
NSOPTION_STRING(cookie_file,   "/tmp/cookies.txt")
NSOPTION_STRING(cookie_jar,    "/tmp/cookies.txt")

/* History / bookmarks — not yet implemented (Iteration 5) */
NSOPTION_STRING(url_file,       NULL)
NSOPTION_STRING(hotlist_path,   NULL)

/* Fonts: FreeType backend; Noto Sans on FAT32 /fonts/ */
NSOPTION_STRING(font_sans,      "NotoSans")
NSOPTION_STRING(font_serif,     "NotoSans")
NSOPTION_STRING(font_mono,      "NotoSansMono")
NSOPTION_STRING(font_cursive,   "NotoSans")
NSOPTION_STRING(font_fantasy,   "NotoSans")
NSOPTION_INTEGER(font_size,     14)   /* CSS px at 96 dpi */
NSOPTION_INTEGER(font_min_size, 10)

/* Display: 1280×720 default viewport (Phase 7.7 will query WM) */
NSOPTION_INTEGER(window_width,  1280)
NSOPTION_INTEGER(window_height, 720)

/* Network — Phase 7.6 will enable actual HTTP */
NSOPTION_INTEGER(max_fetchers,          4)
NSOPTION_INTEGER(max_fetchers_per_host, 2)

/* Scrollbar width in pixels */
NSOPTION_INTEGER(scroll_width, 12)

/* Send Accept-Language header */
NSOPTION_STRING(accept_language, "en-gb,en;q=0.9")

/* No proxy for MVP (HTTPS/HTTPS proxy in Iteration 3) */
NSOPTION_BOOL(http_proxy, false)
