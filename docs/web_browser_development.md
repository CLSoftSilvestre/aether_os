# AetherOS Web Browser — Development Plan

**Approach:** Port NetSurf (framebuffer frontend) with QuickJS for JavaScript  
**Target hardware:** Raspberry Pi 4/5 (AArch64) + QEMU `-M virt`  
**Long-term goal:** Render modern web apps (GitHub, Wikipedia, news sites)  
**Current foundation:** Phase 6.2 complete — networking (TCP/UDP/DNS/HTTP), framebuffer, window manager, FreeType-ready graphics, Lumina UI  

---

## Strategic Overview

A web browser is the most dependency-heavy piece of software in any OS. Rather than building from scratch (12-24 months), we port **NetSurf** — a pure-C browser specifically designed for non-mainstream platforms (RISC OS, AmigaOS, Haiku). Its clean frontend/backend separation means we only implement ~15 callback functions to bridge AetherOS to a complete rendering engine.

### Dependency chain

```
AetherOS TCP/IP stack (Phase 5.1)
        │
        ▼
[ HTTP fetch bridge ]
        │
        ▼                 QuickJS (ES2020 JS engine)
[ NetSurf core ]  ◄──────────────────────────────
  libhubbub (HTML5 parser)        ▲
  libdom    (DOM)                 │
  libcss    (CSS parser)          │ Phase 7.5
  libnsbmp/libnsgif/libsvgtiny    │
        │
        ▼
[ AetherOS frontend ]  (15 plot callbacks → AetherOS framebuffer)
        │
        ▼
[ FreeType 2 ]  +  [ libpng ]  +  [ libjpeg-turbo ]  +  [ zlib ]
        │
        ▼
[ aether_browser app ]  (Lumina UI: address bar, tabs, navigation)
```

### Why NetSurf and not alternatives

| Option | C/C++ | No-OS ports | Framebuffer mode | Active | Verdict |
|---|---|---|---|---|---|
| **NetSurf** | C | AmigaOS, RISC OS, Haiku | Yes, native | Yes | **Best choice** |
| Links2 | C | Limited | Yes (`-g`) | Minimal | Fallback |
| Dillo | C++ | No | Needs FLTK | Minimal | Too many deps |
| Ladybird | C++ | No | No | Very active | Too POSIX-heavy |
| From scratch | C | — | Custom | — | 2+ years |

---

## Critical Pre-Requisite: POSIX Compatibility Shim (Phase 7.0)

NetSurf's core (and every future ported application) assumes a partial POSIX environment. AetherOS must provide a userspace `libaether_posix` layer. This is the single most important prerequisite and will benefit all future software ports.

**Required POSIX symbols:**
- `malloc/free/realloc/calloc` — bridge to AetherOS `kmalloc` via syscall
- `memcpy/memset/memmove/strcmp/strlen/strdup` etc. — string.h subset
- `printf/snprintf/vsnprintf` — output to WM console or debug UART
- `fopen/fclose/fread/fwrite/fseek` — bridge to AetherFS VFS syscalls
- `time/gettimeofday` — bridge to `CNTPCT_EL0`
- `pthread_create/mutex` — bridge to AetherOS task scheduler (simplified, single-threaded mode first)
- `select/poll` subset — single-fd polling via AetherOS socket API

**Deliverable:** `vendor/libaether_posix/` — static library linked into all ported apps.

---

## Phase 7.0 — POSIX Compatibility Shim ✅ COMPLETE (2026-05-11)

**Output:** `vendor/libaether_posix/` — 559 KB static library, zero errors

| Task | Description | Status |
|---|---|---|
| 7.0.1 | `memory.c` — 8 MB first-fit free-list allocator in user BSS | ✅ |
| 7.0.2 | `string_posix.c` — complete string.h + all str/mem functions | ✅ |
| 7.0.3 | `stdio_posix.c` — FILE* over VFS; vsnprintf with all format specs | ✅ |
| 7.0.4 | `time_posix.c` — clock_gettime/gettimeofday via CNTPCT_EL0 | ✅ |
| 7.0.5 | `pthread_stub.c` — mutex/cond no-ops; pthread_create runs inline | ✅ |
| 7.0.6 | `socket_posix.c` — POSIX socket/connect/send/recv → syscalls 700–707 | ✅ |
| 7.0.7 | `errno.c` — errno global; POSIX error constants | ✅ |
| 7.0.8 | `posix_test` — mini HTTP wget; heap/time/mutex/net stack all PASS | ✅ |

---

## Phase 7.1 — Image & Compression Libraries ✅ COMPLETE (2026-05-11)

**Output:** `vendor/zlib/`, `vendor/libpng/`, `vendor/libjpeg/`

| Task | Description | Status |
|---|---|---|
| 7.1.1 | **zlib 1.3.1** — `vendor_zlib` CMake target; inflate/deflate core (no gz files) | ✅ |
| 7.1.2 | **libpng 1.6.43** — `vendor_libpng`; `pnglibconf.h` from prebuilt; ARM NEON disabled | ✅ |
| 7.1.3 | **libjpeg 9f** (IJG reference, pure C) — `vendor_libjpeg`; hand-crafted `jconfig.h` | ✅ |
| 7.1.4 | `img_test` — decode `/images/test.png` + `/images/test.jpg`; verify 4-quadrant pixels | ✅ PASS |

**Notes:**
- `setjmp_aarch64.S` added to `libaether_posix` — saves x19–x30, SP, d8–d15 (168 bytes)
- Critical fix: `jmp_buf` in `setjmp.h` was `long[13]` (104 bytes); enlarged to `long[32]` (256 bytes) to match actual assembly layout — overflow was corrupting BSS and causing ELR=0x100 kernel panic
- `gen_test_images.py` generates 64×64 4-quadrant colour test images; called by `make_disk.sh`
- Fetch order: `fetch_zlib.sh` → `fetch_libpng.sh` → `fetch_libjpeg.sh` → `ninja -C build`
- libjpeg uses IJG 9f (not libjpeg-turbo) — simpler pure-C port; NEON SIMD deferred to later
- libpng uses explicit `png_set_read_fn` callback — avoids `PNG_STDIO_SUPPORTED` dependency

---

## Phase 7.2 — FreeType 2 & Font Integration ✅ COMPLETE (2026-05-12)

**Output:** `vendor/freetype/`, `assets/fonts/`, `lib/aether_font/`

Text rendering is a first-class requirement. FreeType renders TrueType/OpenType fonts to bitmaps.

| Task | Description | Status |
|---|---|---|
| 7.2.1 | **FreeType 2.13.3** — `scripts/fetch_freetype.sh`; modules: sfnt, truetype, smooth, autofit, psnames, raster | ✅ |
| 7.2.2 | **Noto Sans** + **Noto Sans Mono** — `scripts/fetch_fonts.sh` → `assets/fonts/` → `/fonts/` on disk | ✅ |
| 7.2.3 | `lib/aether_font/aether_font.h/c` — `aether_font_init`, `aether_font_load`, `aether_font_draw`, `aether_font_measure_width` | ✅ |
| 7.2.4 | Integrate with Lumina — replace bitmap font in `gfx.c` with FreeType calls | ✅ |
| 7.2.5 | `font_test` — renders "Hello, AetherOS!" at 12/18/24px + Greek "Γεια!" into pixel buffer; checks non-zero pixel count | ✅ |

**Setup order:**
```
scripts/fetch_freetype.sh
scripts/fetch_fonts.sh
ninja -C build
scripts/make_disk.sh
```

**Notes:**
- FreeType compiled with explicit source list (no autoconf) — same pattern as libjpeg
- `FT2_BUILD_LIBRARY` unlocks internal headers; each module dir added to PRIVATE includes
- `ftoption.h` patched to: disable LZW/BZip2/HarfBuzz/Brotli; enable `FT_CONFIG_OPTION_SYSTEM_ZLIB` (uses vendor_zlib)
- Math stubs (`sin/cos/pow`) are fine — FreeType uses its own CORDIC tables in `fttrigon.c`, never calls libm trig
- `aether_font_draw` blends FreeType grayscale glyph bitmaps onto a 32-bit ARGB buffer with linear alpha
- HarfBuzz (complex text shaping: Arabic, Hebrew, Devanagari) deferred to a later iteration
- **Critical bug fixed:** `FT_New_Face` uses `fseek(SEEK_END)` (unsupported in AetherOS VFS) to get file size. Replaced with `FT_New_Memory_Face` — font file is read sequentially via `sys_fs_read` into a heap buffer, bypassing the stream layer entirely.
- 7.2.4 deferred: Lumina `gfx.c` still uses the built-in bitmap font; FreeType integration pending Phase 7.4+

> **Note on HarfBuzz:** Latin text works without it.

---

## Phase 7.3 — NetSurf Support Libraries ✅ COMPLETE (2026-05-12)

**Duration:** 3–4 weeks  
**Output:** `userspace/vendor/` (8 sub-libraries as static `.a`)

NetSurf depends on its own set of pure-C support libraries, maintained by the NetSurf project.

| Task | Library | Purpose | Status |
|---|---|---|---|
| 7.3.1 | **libparserutils** 0.2.5 | Character encoding, input stream processing | ✅ |
| 7.3.2 | **libwapcaplet** 0.4.3 | String interning (reduces memory in DOM) | ✅ |
| 7.3.3 | **libhubbub** 0.3.8 | HTML5 conformant parser (outputs tree-building events) | ✅ |
| 7.3.4 | **libdom** 0.4.2 | W3C DOM implementation; depends on libhubbub + libwapcaplet | ✅ |
| 7.3.5 | **libcss** 0.9.2 | CSS2.1/3 parser and selection engine | ✅ |
| 7.3.6 | **libnsbmp** 0.1.7 / **libnsgif** 1.0.0 / **libnsutils** 0.1.1 | BMP/GIF decoders + utility functions | ✅ |
| 7.3.7 | Integration test (`html_test`) — libwapcaplet + libhubbub parse HTML5 | ✅ sys_exit(0) |

**Build notes:**
- `scripts/fetch_netsurf_libs.sh` downloads all 8 libs and generates required files (`aliases.inc`, `entities.inc`, `autogenerated-element-type.c`)
- `WITHOUT_ICONV_FILTER=1` on libparserutils (no iconv on AetherOS)
- `pread`/`pwrite` stubs added to libaether_posix (ENOSYS); `strings.h` shim added for `strcasecmp`/`strncasecmp`
- `css_property_parser_gen.c` excluded from libcss GLOB (host tool, not a library source)
- Terminal output not displayed (pipe relay bug — separate from test correctness; `sys_exit(0)` confirmed in kernel log)

---

## Phase 7.4 — QuickJS Port ✅ COMPLETE (2026-05-12)

**Duration:** 2–3 weeks  
**Output:** `userspace/vendor/quickjs/` (fetched by `scripts/fetch_quickjs.sh`)

QuickJS 2021-03-27 (Fabrice Bellard) — small embeddable ES2020 engine in pure C.

| Task | Description | Status |
|---|---|---|
| 7.4.1 | `scripts/fetch_quickjs.sh` — downloads + extracts QuickJS 2021-03-27 | ✅ |
| 7.4.2 | Configuration via CMake defines: `CONFIG_BIGNUM` (required for BigInt/ES2020), `CONFIG_ATOMICS` hardcoded in source → `stdatomic.h` stub provided; `-mno-outline-atomics` avoids outline GCC helper calls | ✅ |
| 7.4.3 | No OS module needed — `quickjs-libc.c` excluded; integration test uses raw `JS_Eval` API | ✅ |
| 7.4.4 | `vendor_quickjs` static library: `quickjs.c + libregexp.c + libunicode.c + cutils.c + libbf.c` | ✅ |
| 7.4.5 | `js_test`: string eval, arithmetic, arrow functions, closures, BigInt — confirmed `sys_exit(0)` | ✅ |
| 7.4.6 | `JS_SetMemoryLimit(rt, 16MB)` + `JS_SetMaxStackSize(rt, 64KB)` in `js_test/main.c` | ✅ |

**Build notes:**
- `scripts/fetch_quickjs.sh` downloads to `userspace/vendor/quickjs/` (gitignored — unmodified upstream)
- `-mno-outline-atomics` on vendor_quickjs avoids `__aarch64_ldadd*_acq_rel` libgcc calls
- libaether_posix additions: `fenv.h` stub, `stdatomic.h` stub, `malloc_usable_size` returning 0, `pthread_cond_timedwait`, `acosh/asinh/atanh/expm1/log1p/lrint/...` math stubs, `tm_gmtoff` in struct tm, `alloca` macro

---

## Phase 7.5 — NetSurf Core Port ✅ COMPLETE (2026-05-16)

**Output:** `vendor/netsurf/` (core, no frontend), `lib/netsurf_aether/` (AetherOS bridge)

| Task | Description | Status |
|---|---|---|
| 7.5.1 | `scripts/fetch_netsurf.sh` — shallow-clones NetSurf release/3.11 + libnslog into `vendor/` | ✅ |
| 7.5.2 | `fetch_file_aether.c` (file:// via AetherFS) + `fetch_http_stub.c` (http:// placeholder); curl excluded from GLOB | ✅ |
| 7.5.3 | Single-threaded cooperative model: `gui_misc_table.schedule` stores entries in a 64-slot ring queue; `nsaether_schedule_drain()` drains each event-loop tick | ✅ |
| 7.5.4 | `js_aether.c` — QuickJS bridge: real `JSRuntime`/`JSContext` per page; `js_addscript` evaluates via `JS_Eval`; `js_fire_event` stubbed (Iteration 2 DOM bindings); `console.log` wired to UART | ✅ |
| 7.5.5 | `nslog_aether.c` — registers `nslog_set_render_callback` → UART; shows level/category/file:line | ✅ |
| 7.5.6 | `options.h` + CMake defines: `WITH_RSVG=0`, `WITH_WEBP=0`, `NETSURF_USE_CURL=0`, `NETSURF_USE_DUKTAPE=0`; JS defaulted to false (Iteration 2 enables it) | ✅ |
| 7.5.7 | `vendor_libnslog` + `vendor_netsurf` CMake targets; `apps/ns_test/main.c` calls `netsurf_init()` → verifies `NSERROR_OK` → `netsurf_exit()` | ✅ |

**Build notes:**
- `scripts/fetch_netsurf.sh` clones two repos: `libnslog` (logging framework) and `netsurf` (release/3.11 — core engine)
- CMake GLOB_RECURSE collects all `*.c` from `vendor/netsurf/`; excluded via regex: `frontends/`, `javascript/duktape/`, `javascript/none.c`, `content/fetchers/curl.c`, `test/`, `tools/`
- Our bridge in `lib/netsurf_aether/` replaces those excluded files with AetherOS implementations
- `options.h` selects platform defaults; included by `nsoption.c` via `-DNETSURF_OPTIONS_HEADER=...`
- `-Wl,--allow-multiple-definition` on the test binary resolves the intentional libaether/libaether_posix symbol overlap (same pattern as other 7.x binaries)
- `js_aether.c` forward-declares `js_aether_console_log` as a static helper; signatures verified against QuickJS `JS_NewCFunction` API
- QuickJS `libgcc.a` path linked last (same `QJS_LIBGCC_PATH` CMake variable from Phase 7.4) for `__udivti3`/`__umodti3`

**Setup order:**
```
scripts/fetch_netsurf.sh
ninja -C build
```
Run inside AetherOS: `ns_test` → expect "ns_test PASS"

**Known adjustment points after first build:**
- If `netsurf_init()` signature differs from `netsurf_init(NULL)` → check `desktop/netsurf.h` in the cloned source
- If `guit` is not a bare global → check `desktop/netsurf.h` for the actual registration API
- If `fetcher_add` is named differently → check `content/fetch.h` for the registration function name
- `gui_window_table` field names may differ slightly — cross-check `desktop/gui_window.h`

---

## Phase 7.6 — HTTP Fetch Bridge ✅ COMPLETE (2026-05-16)

**Output:** `userspace/lib/netsurf_aether/fetch_file_aether.c`, `fetch_http_aether.c`

Bridges NetSurf's fetch API to AetherOS Phase 5.1 TCP/IP stack via libaether_posix POSIX sockets.

| Task | Description | Status |
|---|---|---|
| 7.6.1 | `fetch_file_register()` replaces built-in (excludes scandir-dependent `file/file.c`) | ✅ |
| 7.6.2 | `fetch_http_aether.c` — HTTP/1.1 over POSIX socket/connect/send/recv | ✅ |
| 7.6.3 | `Connection: close` — synchronous read-until-close model | ✅ |
| 7.6.4 | Gzip decompression via vendor_zlib (`inflateInit2` windowBits=47) | ✅ |
| 7.6.5 | Redirect following 301/302/303/307/308, max 5 hops, inline in `start()` | ✅ |
| 7.6.6 | MIME from `Content-Type:` header; fallback to `text/html` | ✅ |
| 7.6.7 | `apps/fetch_test` — `fetch_start()` → scheduler drain → UART dump | ✅ |

Key design decisions:
- Built-in `content/fetchers/file/file.c` excluded (uses `scandir`); replaced by `fetch_file_aether.c` which provides `fetch_file_register()` — the symbol `fetcher_init()` calls
- `fetch_http_aether_register()` must be called **after** `netsurf_init()` (requires lwc_intern_string, which needs libwapcaplet initialised)
- `start()` is synchronous (entire HTTP response buffered before returning); `poll()` delivers FETCH_HEADER + FETCH_DATA + FETCH_FINISHED on next scheduler tick
- https:// registers but uses plain TCP (no TLS until Phase 7.8+)

---

## Phase 7.7 — AetherOS Frontend (Plot API) ✅ COMPLETE (2026-05-16)

**Output:** `lib/netsurf_aether/plot_aether.c`, `lib/netsurf_aether/plot_aether.h`, `lib/netsurf_aether/gui_window_stub.c`, `lib/netsurf_aether/gui_bitmap_aether.c`, `lib/netsurf_aether/gui_layout_aether.c`, `apps/browser_test/main.c`

| Task | Description | Status |
|---|---|---|
| 7.7.1 | `plot_aether.c` — full `plotter_table`: clip, arc, disc, line, rectangle, polygon, path (Bézier), bitmap (with repeat-x/y tiling), text (FreeType) | ✅ |
| 7.7.2 | `aether_plot_ctx_t` — off-screen XRGB8888 buffer with scissor rect; `aether_plot_ctx_init()` | ✅ |
| 7.7.3 | `gui_window_stub.c` — real `gui_window_table`: create (alloc pixel buffer), destroy, invalidate (sets `nsaether_dirty`), get/set_scroll, get_dimensions, event (throbber), set_title, set_url, set_status | ✅ |
| 7.7.4 | `gui_bitmap_aether.c` — `gui_bitmap_table`: create/destroy/get_buffer/get_rowstride/get_width/get_height/set_opaque/modified | ✅ |
| 7.7.5 | `gui_layout_aether.c` — `gui_layout_table`: width/position/split via FreeType `aether_font_measure_width`; monospace fallback | ✅ |
| 7.7.6 | `apps/browser_test/main.c` — integration test: `netsurf_init` → `browser_window_create` → scheduler drain → `browser_window_redraw` (aether_plotter_table) → `gfx_raw_blit` | ✅ |

**Globals exposed to browser app:** `nsaether_dirty`, `nsaether_loading`, `nsaether_bw`, `nsaether_pixels`, `nsaether_win_w/h`, `nsaether_status[256]`, `nsaether_url[512]`

**Design notes:**
- Off-screen 1024×615 XRGB8888 buffer; `browser_window_redraw()` fills it; `gfx_raw_blit()` copies to framebuffer
- Bézier paths implemented via De Casteljau subdivision (depth 6) — no polygon-approximation shortcut needed
- `bitmap` callback handles both single-copy and tiled (`BITMAPF_REPEAT_X/Y`) in one pass
- `set_url` / `set_status` store into globals; browser app reads them each event-loop tick

---

## Phase 7.8 — aether_browser App (MVP)

**Duration:** 3–4 weeks  
**Output:** `apps/aether_browser/main.c`, registered at `/apps/aether_browser.app`

**MVP feature set:**
- [x] Lumina-styled browser window (dark glassmorphism, consistent with AetherOS)
- [x] Address bar (inline text input: cursor, backspace, arrow keys, Home/End)
- [x] Back / Forward / Reload buttons
- [x] Scroll via keyboard (↑↓ PgUp PgDn arrows → `browser_window_scroll_at_point`)
- [x] Page render (HTML + CSS, no JS at this stage)
- [x] Status bar ("Loading...", "Done", NetSurf hover URL)
- [x] Image display (PNG, JPEG, GIF, BMP — via NetSurf bitmap callbacks)
- [x] Link click navigation (PRESS_1 + CLICK_1 mouse events)
- [x] `file://` URL support (via `fetch_file_aether.c`)
- [x] Keyboard shortcut: Ctrl+L → focus address bar, Ctrl+R → reload, Ctrl+[/] → back/forward
- [x] App manifest: `aether_browser.app` in initrd + FAT32 disk

| Task | Description | Status |
|---|---|---|
| 7.8.1 | App skeleton: Lumina window, WM registration, chrome draw | ✅ |
| 7.8.2 | Viewport: `browser_window_redraw` + `gfx_raw_blit` into content area | ✅ |
| 7.8.3 | Event loop: WM poll → mouse/key → NetSurf + toolbar hit-test | ✅ |
| 7.8.4 | Scroll: `browser_window_scroll_at_point` for arrow/pgup/pgdn keys | ✅ |
| 7.8.5 | Status bar: `nsaether_status` + loading indicator | ✅ |
| 7.8.6 | Register app: `aether_browser.app` manifest + CMake target | ✅ |
| 7.8.7 | QEMU integration test: navigate to a local HTML file served via AetherOS `http` module | ⬜ |
| 7.8.8 | Pi 5 hardware test: navigate to a plain HTTP page on local network | ⬜ |

**Build notes:**
- `apps/aether_browser/main.c` — custom event loop (no `widget_run`); address bar drawn via `gfx_*` primitives
- Includes `desktop/browser_history.h` for `browser_window_history_back/forward()`
- Linked with `vendor_netsurf`, `libaether_font`, `libaether_posix`, `libaether` (same flags as `browser_test`)
- `gui_window_stub.c` upgraded: `set_title/set_url/set_status` + `GW_EVENT_START/STOP_THROBBER` in `event`

**Setup order:**
```
ninja -C build
scripts/make_disk.sh
```
Run inside AetherOS: `aether_browser` → renders default test page; or `aether_browser http://example.com/`

---

## MVP Success Criteria

The MVP milestone is complete when:
1. `aether_browser` launches from the Lumina desktop
2. User can type a URL in the address bar and press Enter
3. A plain HTTP HTML page loads and renders (text + images)
4. Scrolling works with mouse wheel and keyboard
5. Links navigate to new pages
6. Basic HTML forms submit over HTTP GET/POST
7. App integrates visually with the Lumina glassmorphism theme

> **Realistic sites for MVP testing:** `http://example.com`, `http://info.cern.ch` (first website), local AetherFS HTML files, any HTTP-only documentation mirror.

---

## Iteration 2 — JavaScript Engine Integration

**Duration:** 4–5 weeks  
**Prerequisites:** MVP complete, QuickJS compiled (Phase 7.4)

| Task | Description |
|---|---|
| I2.1 | Wire QuickJS into NetSurf DOM — `script` tag evaluation |
| I2.2 | Implement minimal Web APIs: `document.getElementById`, `querySelector`, `addEventListener`, `fetch` (HTTP only) |
| I2.3 | `console.log` → status bar / UART |
| I2.4 | `XMLHttpRequest` stub over AetherOS HTTP fetch |
| I2.5 | Timer APIs: `setTimeout`, `setInterval` via AetherOS tick scheduler |
| I2.6 | Test: HackerNews (minimal JS), simple todo apps, Wikipedia (sidebar JS) |

---

## Iteration 3 — HTTPS / TLS

**Duration:** 3–4 weeks

| Task | Description |
|---|---|
| I3.1 | Port **mbedTLS 3.x** (~300KB, pure C, designed for embedded) |
| I3.2 | Implement TLS record layer over AetherOS TCP socket |
| I3.3 | Bundle root CA certificates (Mozilla CA bundle, baked into initrd) |
| I3.4 | Wire into HTTP fetch bridge: detect `https://` scheme, wrap socket in TLS |
| I3.5 | Test: Wikipedia, GitHub, HTTPS documentation sites |

---

## Iteration 4 — Tabs & Browser Chrome

**Duration:** 3 weeks

| Task | Description |
|---|---|
| I4.1 | Tab bar widget (WIDGET_TABBAR): create/close/switch tabs |
| I4.2 | Multi-page NetSurf browser_window instances |
| I4.3 | Ctrl+T new tab, Ctrl+W close tab, Ctrl+1..9 switch |
| I4.4 | Tab favicons |
| I4.5 | Memory limit per tab (evict inactive tab renders) |

---

## Iteration 5 — Persistence & User Features

**Duration:** 2–3 weeks

| Task | Description |
|---|---|
| I5.1 | Bookmarks: save/load from AetherFS (`/user/bookmarks.json`) |
| I5.2 | Browser history: visited URLs + timestamps in AetherFS |
| I5.3 | Cookies: in-memory store first, then AetherFS persistence |
| I5.4 | Download manager: save HTTP responses to AetherFS |
| I5.5 | Zoom: Ctrl+/- scales plot coordinates |
| I5.6 | Reader mode: strip navigation, ads, render article text only |

---

## Iteration 6 — Modern Web Compatibility

**Duration:** ongoing

| Task | Description |
|---|---|
| I6.1 | CSS Grid + Flexbox layout improvements in NetSurf |
| I6.2 | HarfBuzz text shaping for complex scripts (Arabic, Devanagari) |
| I6.3 | CSS animations (NetSurf already has partial support) |
| I6.4 | `<video>` tag stub (show poster image, play button placeholder) |
| I6.5 | Web fonts (`@font-face` — download + FreeType render) |
| I6.6 | LocalStorage / IndexedDB over AetherFS |
| I6.7 | Service Workers (advanced — requires multi-task fetch model) |

---

## Risk Register

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| NetSurf POSIX assumptions not covered by shim | High | High | Map each NetSurf source file's OS calls before porting |
| QuickJS GC pauses visible at 60fps | Medium | Medium | Tune GC interval; run JS ticks between frames |
| Memory pressure (NetSurf + QuickJS + images) | Medium | Medium | Pi 5 has 4-8GB — set 256MB browser heap; QEMU: `-m 1G` |
| Fonts: no system font directory | High | Medium | Bundle Noto as static blob; embed in initrd |
| pthreads: NetSurf fetch uses threads | High | High | Disable async fetch; use cooperative single-threaded polling |
| CSS3 compliance gaps in NetSurf | High | Low | Accept limitations; document which sites render correctly |
| HTTP-only locks out most modern sites | High | Low (MVP) | HTTPS in Iteration 3; local test server for MVP |

---

## Dependency Build Order

```
Phase 7.0: libaether_posix (POSIX shim)
    └─► Phase 7.1: zlib ─► libpng, libjpeg-turbo
    └─► Phase 7.2: FreeType 2 + font bundle
    └─► Phase 7.3: libparserutils ─► libwapcaplet ─► libhubbub ─► libdom ─► libcss
                   libnsbmp, libnsgif, libnsutils
    └─► Phase 7.4: QuickJS
         └─► Phase 7.5: NetSurf core (links all above)
              └─► Phase 7.6: HTTP fetch bridge
              └─► Phase 7.7: AetherOS plot frontend
                   └─► Phase 7.8: aether_browser app (MVP)
                        └─► Iteration 2: JS
                        └─► Iteration 3: HTTPS
                        └─► Iteration 4: Tabs
                        └─► Iteration 5: Persistence
                        └─► Iteration 6: Modern web
```

---

## Estimated Timeline

| Phase | Description | Duration |
|---|---|---|
| 7.0 | POSIX shim | 3–4 weeks |
| 7.1 | Image libs (zlib, libpng, libjpeg) | 2–3 weeks |
| 7.2 | FreeType 2 + fonts | 2–3 weeks |
| 7.3 | NetSurf support libs | 3–4 weeks |
| 7.4 | QuickJS port | 2–3 weeks |
| 7.5 | NetSurf core | 4–5 weeks |
| 7.6 | HTTP fetch bridge | 1–2 weeks |
| 7.7 | AetherOS plot frontend | 4–5 weeks |
| 7.8 | aether_browser MVP app | 3–4 weeks |
| **MVP total** | | **~24–33 weeks** |
| Iteration 2 | JavaScript | +4–5 weeks |
| Iteration 3 | HTTPS | +3–4 weeks |
| Iteration 4 | Tabs | +3 weeks |
| Iteration 5 | Persistence | +2–3 weeks |

---

## Recommended Test Harness

Since most development happens in QEMU, set up a simple HTTP test server on the Mac:

```bash
# Serve local HTML test pages over HTTP on LAN
python3 -m http.server 8080 --directory tests/browser/
```

Test QEMU networking by launching with:
```
-netdev user,id=net0,hostfwd=tcp::8080-:8080 -device virtio-net-pci,netdev=net0
```

Then navigate to `http://10.0.2.2:8080/test.html` from within AetherOS.

**Recommended test pages (build in `tests/browser/`):**
- `basic.html` — headings, paragraphs, lists, links
- `images.html` — PNG, JPEG, GIF inline images
- `forms.html` — text input, submit button, GET form
- `css_box.html` — box model, colors, fonts, borders
- `css_flex.html` — Flexbox layout test (Iteration 6)
- `js_basic.html` — alert(), DOM manipulation, setTimeout (Iteration 2)

---

## Reference Links

- NetSurf source: https://git.netsurf-browser.org/netsurf.git
- NetSurf framebuffer frontend (reference): `frontends/framebuffer/` in NetSurf repo
- QuickJS: https://bellard.org/quickjs/
- NetSurf Haiku port (closest to AetherOS): `frontends/beos/`
- FreeType docs: https://freetype.org/freetype2/docs/
- mbedTLS (Iteration 3): https://github.com/Mbed-TLS/mbedtls
