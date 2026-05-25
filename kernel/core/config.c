/*
 * AetherOS — Kernel configuration file reader/writer
 * File: kernel/core/config.c
 *
 * Persistent key=value config on FAT32 at /config/*.cfg.
 * Operates directly on FAT32 (not VFS), so it can be called early in boot
 * (after fat32_mount) and from syscall handlers.
 */

#include "aether/config.h"
#include "aether/fat32.h"
#include "aether/printk.h"
#include "aether/types.h"

#define KCONFIG_MAX_FILE  2048
#define KCONFIG_MAX_KEY     32
#define KCONFIG_MAX_VAL    128

/* ── helpers ──────────────────────────────────────────────────────────────── */

int katoi(const char *s)
{
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

int kitoa(int v, char *buf, u32 buf_len)
{
    if (buf_len < 2) return 0;
    if (v < 0) { *buf++ = '-'; buf_len--; v = -v; }

    /* Write digits in reverse into a temp buffer */
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }

    if ((u32)n >= buf_len) return 0;
    for (int i = n - 1; i >= 0; i--) *buf++ = tmp[i];
    *buf = '\0';
    return n;
}

static int kstrlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int kstrcmp_n(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

static void kstrcpy(char *dst, const char *src, u32 max)
{
    u32 i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ── kconfig_read ─────────────────────────────────────────────────────────── */

int kconfig_read(const char *path, const char *key, char *out, u32 out_len)
{
    if (!fat32_ready()) return -1;

    int fh = fat32_open(path);
    if (fh < 0) return -1;

    static u8 buf[KCONFIG_MAX_FILE];
    int total = 0, n;
    while ((n = fat32_read(fh, buf + total,
                           (u32)(KCONFIG_MAX_FILE - 1 - total))) > 0)
        total += n;
    fat32_close(fh);
    buf[total] = '\0';

    int klen = kstrlen(key);
    const char *p = (const char *)buf;
    while (*p) {
        /* Skip comments and blank lines */
        if (*p == '#' || *p == '\n' || *p == '\r') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        /* Check if this line starts with key= */
        if (kstrcmp_n(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            u32 vlen = 0;
            while (val[vlen] && val[vlen] != '\n' && val[vlen] != '\r')
                vlen++;
            if (vlen >= out_len) vlen = out_len - 1;
            for (u32 i = 0; i < vlen; i++) out[i] = val[i];
            out[vlen] = '\0';
            return 0;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return -1;
}

/* ── kconfig_write ────────────────────────────────────────────────────────── */

int kconfig_write(const char *path, const char *key, const char *value)
{
    if (!fat32_ready()) return -1;

    static u8 fbuf[KCONFIG_MAX_FILE];
    int total = 0;

    /* Read existing content (ignore error — file may not exist yet) */
    int fh = fat32_open(path);
    if (fh >= 0) {
        int n;
        while ((n = fat32_read(fh, fbuf + total,
                               (u32)(KCONFIG_MAX_FILE - 1 - total))) > 0)
            total += n;
        fat32_close(fh);
    }
    fbuf[total] = '\0';

    int klen  = kstrlen(key);
    int vlen  = kstrlen(value);

    /* Build a new file in a second static buffer */
    static u8 out[KCONFIG_MAX_FILE];
    int olen = 0;
    int replaced = 0;

    const char *p = (const char *)fbuf;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') { p++; line_len++; }

        /* Check if this line is our key */
        if (!replaced && line_len > klen + 1 &&
            kstrcmp_n(line_start, key, klen) == 0 &&
            line_start[klen] == '=') {
            /* Replace this line */
            if (olen + klen + 1 + vlen + 1 >= KCONFIG_MAX_FILE) return -1;
            for (int i = 0; i < klen; i++) out[olen++] = (u8)key[i];
            out[olen++] = '=';
            for (int i = 0; i < vlen; i++) out[olen++] = (u8)value[i];
            out[olen++] = '\n';
            replaced = 1;
        } else {
            /* Copy line verbatim */
            if (olen + line_len >= KCONFIG_MAX_FILE) return -1;
            for (int i = 0; i < line_len; i++)
                out[olen++] = (u8)line_start[i];
        }
    }

    /* Key was not found — append it */
    if (!replaced) {
        /* Ensure file ends with a newline before appending */
        if (olen > 0 && out[olen - 1] != '\n') out[olen++] = '\n';
        if (olen + klen + 1 + vlen + 1 >= KCONFIG_MAX_FILE) return -1;
        for (int i = 0; i < klen; i++) out[olen++] = (u8)key[i];
        out[olen++] = '=';
        for (int i = 0; i < vlen; i++) out[olen++] = (u8)value[i];
        out[olen++] = '\n';
    }

    /* Write back to FAT32 (fat32_create truncates existing content) */
    fh = fat32_create(path);
    if (fh < 0) {
        kwarn("[config] kconfig_write: cannot create %s\n", path);
        return -1;
    }
    int written = fat32_write(fh, out, (u32)olen);
    fat32_close(fh);
    (void)kstrcpy; /* suppress unused warning */
    return (written == olen) ? 0 : -1;
}
