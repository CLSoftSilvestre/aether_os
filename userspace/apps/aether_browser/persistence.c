/*
 * persistence.c — Bookmark and history persistence for aether_browser
 *
 * Storage: FAT32 /config/ (pre-created by make_disk.sh)
 *   /config/bookmarks.txt  — "url|title\n" per line
 *   /config/history.txt    — "url\n" per line, oldest first
 *
 * fopen("w") in libaether_posix calls sys_fs_create (truncate + rewrite),
 * so both files are always overwritten in full on every save.
 */

#include "persistence.h"
#include <stdio.h>
#include <string.h>

/* ── Bookmark store ──────────────────────────────────────────────────────── */

static bookmark_t g_bmarks[BMARKS_MAX];
static int        g_bmark_count = 0;

static void bmarks_load(void)
{
    FILE *f = fopen(BMARKS_PATH, "r");
    if (!f) return;
    char line[580];
    while (fgets(line, sizeof(line), f) && g_bmark_count < BMARKS_MAX) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (!len) continue;

        bookmark_t *bm = &g_bmarks[g_bmark_count];
        char *pipe = strchr(line, '|');
        if (pipe) {
            int ulen = (int)(pipe - line);
            if (ulen >= (int)sizeof(bm->url)) ulen = (int)sizeof(bm->url) - 1;
            memcpy(bm->url, line, (size_t)ulen);
            bm->url[ulen] = '\0';
            const char *tp = pipe + 1;
            int tlen = len - ulen - 1;
            if (tlen >= (int)sizeof(bm->title)) tlen = (int)sizeof(bm->title) - 1;
            if (tlen > 0) memcpy(bm->title, tp, (size_t)tlen);
            bm->title[tlen > 0 ? tlen : 0] = '\0';
        } else {
            int ulen = len;
            if (ulen >= (int)sizeof(bm->url)) ulen = (int)sizeof(bm->url) - 1;
            memcpy(bm->url, line, (size_t)ulen);
            bm->url[ulen] = '\0';
            bm->title[0] = '\0';
        }
        g_bmark_count++;
    }
    fclose(f);
}

static void bmarks_save(void)
{
    FILE *f = fopen(BMARKS_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_bmark_count; i++) {
        fputs(g_bmarks[i].url, f);
        fputc('|', f);
        fputs(g_bmarks[i].title, f);
        fputc('\n', f);
    }
    fclose(f);
}

/* ── History store ───────────────────────────────────────────────────────── */

static char g_hist[HISTORY_MAX][512];
static int  g_hist_count = 0;

static void history_load(void)
{
    FILE *f = fopen(HISTORY_PATH, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) && g_hist_count < HISTORY_MAX) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (!len) continue;
        if (len >= 512) len = 511;
        memcpy(g_hist[g_hist_count], line, (size_t)len);
        g_hist[g_hist_count][len] = '\0';
        g_hist_count++;
    }
    fclose(f);
}

static void history_save(void)
{
    FILE *f = fopen(HISTORY_PATH, "w");
    if (!f) return;
    for (int i = 0; i < g_hist_count; i++) {
        fputs(g_hist[i], f);
        fputc('\n', f);
    }
    fclose(f);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void persist_init(void)
{
    bmarks_load();
    history_load();
}

void bmarks_add(const char *url, const char *title)
{
    if (!url || g_bmark_count >= BMARKS_MAX) return;
    if (bmarks_find(url) >= 0) return;

    bookmark_t *bm = &g_bmarks[g_bmark_count++];

    int ulen = (int)strlen(url);
    if (ulen >= (int)sizeof(bm->url)) ulen = (int)sizeof(bm->url) - 1;
    memcpy(bm->url, url, (size_t)ulen);
    bm->url[ulen] = '\0';

    if (title && title[0]) {
        int tlen = (int)strlen(title);
        if (tlen >= (int)sizeof(bm->title)) tlen = (int)sizeof(bm->title) - 1;
        memcpy(bm->title, title, (size_t)tlen);
        bm->title[tlen] = '\0';
    } else {
        bm->title[0] = '\0';
    }

    bmarks_save();
}

void bmarks_remove(int idx)
{
    if (idx < 0 || idx >= g_bmark_count) return;
    for (int i = idx; i < g_bmark_count - 1; i++)
        g_bmarks[i] = g_bmarks[i + 1];
    g_bmark_count--;
    bmarks_save();
}

int bmarks_find(const char *url)
{
    if (!url) return -1;
    for (int i = 0; i < g_bmark_count; i++)
        if (strcmp(g_bmarks[i].url, url) == 0) return i;
    return -1;
}

int bmarks_count(void) { return g_bmark_count; }

const bookmark_t *bmarks_get(int idx)
{
    if (idx < 0 || idx >= g_bmark_count) return (void *)0;
    return &g_bmarks[idx];
}

void history_append(const char *url)
{
    if (!url || !url[0]) return;
    if (g_hist_count >= HISTORY_MAX) {
        for (int i = 0; i < HISTORY_MAX - 1; i++)
            memcpy(g_hist[i], g_hist[i + 1], 512);
        g_hist_count = HISTORY_MAX - 1;
    }
    int len = (int)strlen(url);
    if (len >= 512) len = 511;
    memcpy(g_hist[g_hist_count], url, (size_t)len);
    g_hist[g_hist_count][len] = '\0';
    g_hist_count++;
    history_save();
}

int history_count(void) { return g_hist_count; }

const char *history_get_url(int idx)
{
    if (idx < 0 || idx >= g_hist_count) return (void *)0;
    return g_hist[idx];
}
