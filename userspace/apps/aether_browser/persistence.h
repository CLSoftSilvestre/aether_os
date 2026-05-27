#pragma once

/* Limits */
#define BMARKS_MAX   64
#define HISTORY_MAX  100

/* Storage paths on FAT32 /config/ (directory pre-created by make_disk.sh) */
#define BMARKS_PATH  "/config/bookmarks.txt"
#define HISTORY_PATH "/config/history.txt"

/* Bookmark record: URL and display title, pipe-separated in the file */
typedef struct {
    char url[512];
    char title[64];
} bookmark_t;

/* Call once at startup — loads bookmarks and history from disk */
void persist_init(void);

/* Bookmarks ─────────────────────────────────────────────────────────────── */

/* Add a bookmark (no-op if URL already exists). Saves to disk immediately. */
void bmarks_add(const char *url, const char *title);

/* Remove bookmark by index. Saves to disk immediately. */
void bmarks_remove(int idx);

/* Return index of matching URL, or -1 if not found. */
int  bmarks_find(const char *url);

int               bmarks_count(void);
const bookmark_t *bmarks_get(int idx);

/* History ────────────────────────────────────────────────────────────────── */

/* Append a URL to the in-memory history and rewrite the history file.
 * Oldest entry is evicted when HISTORY_MAX is reached. */
void        history_append(const char *url);

int         history_count(void);
const char *history_get_url(int idx);
