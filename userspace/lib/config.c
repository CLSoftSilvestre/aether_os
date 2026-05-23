/*
 * AetherOS — Userspace configuration file helper
 * File: userspace/lib/config.c
 */

#include <config.h>
#include <sys.h>
#include <string.h>

#define CFG_MAX_FILE  2048

/* ── internal helpers ─────────────────────────────────────────────────────── */

static int cfg_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int cfg_strcmp_n(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 1;
        if (!a[i]) return 0;
    }
    return 0;
}

/* ── cfg_read ─────────────────────────────────────────────────────────────── */

int cfg_read(const char *path, const char *key, char *out, int out_len)
{
    static char buf[CFG_MAX_FILE];

    int fd = sys_fs_open(path);
    if (fd < 0) return -1;

    int total = 0, n;
    while ((n = (int)sys_fs_read(fd, (void *)(buf + total),
                                 (long)(CFG_MAX_FILE - 1 - total))) > 0)
        total += n;
    sys_fs_close(fd);
    buf[total] = '\0';

    int klen = cfg_strlen(key);
    const char *p = buf;
    while (*p) {
        if (*p == '#' || *p == '\n' || *p == '\r') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }
        if (cfg_strcmp_n(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            int vlen = 0;
            while (val[vlen] && val[vlen] != '\n' && val[vlen] != '\r')
                vlen++;
            if (vlen >= out_len) vlen = out_len - 1;
            for (int i = 0; i < vlen; i++) out[i] = val[i];
            out[vlen] = '\0';
            return 0;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return -1;
}

/* ── cfg_write ────────────────────────────────────────────────────────────── */

int cfg_write(const char *path, const char *key, const char *value)
{
    static char fbuf[CFG_MAX_FILE];
    static char obuf[CFG_MAX_FILE];
    int total = 0;

    int fd = sys_fs_open(path);
    if (fd >= 0) {
        int n;
        while ((n = (int)sys_fs_read(fd, (void *)(fbuf + total),
                                     (long)(CFG_MAX_FILE - 1 - total))) > 0)
            total += n;
        sys_fs_close(fd);
    }
    fbuf[total] = '\0';

    int klen  = cfg_strlen(key);
    int vlen  = cfg_strlen(value);
    int olen  = 0;
    int replaced = 0;

    const char *p = fbuf;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') { p++; line_len++; }

        if (!replaced && line_len > klen + 1 &&
            cfg_strcmp_n(line_start, key, klen) == 0 &&
            line_start[klen] == '=') {
            if (olen + klen + 1 + vlen + 1 >= CFG_MAX_FILE) return -1;
            for (int i = 0; i < klen; i++)  obuf[olen++] = key[i];
            obuf[olen++] = '=';
            for (int i = 0; i < vlen; i++)  obuf[olen++] = value[i];
            obuf[olen++] = '\n';
            replaced = 1;
        } else {
            if (olen + line_len >= CFG_MAX_FILE) return -1;
            for (int i = 0; i < line_len; i++) obuf[olen++] = line_start[i];
        }
    }

    if (!replaced) {
        if (olen > 0 && obuf[olen - 1] != '\n') obuf[olen++] = '\n';
        if (olen + klen + 1 + vlen + 1 >= CFG_MAX_FILE) return -1;
        for (int i = 0; i < klen; i++)  obuf[olen++] = key[i];
        obuf[olen++] = '=';
        for (int i = 0; i < vlen; i++)  obuf[olen++] = value[i];
        obuf[olen++] = '\n';
    }

    fd = sys_fs_create(path);
    if (fd < 0) return -1;
    long written = sys_fs_write(fd, obuf, (long)olen);
    sys_fs_close(fd);
    return (written == (long)olen) ? 0 : -1;
}
