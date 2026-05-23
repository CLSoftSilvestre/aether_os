/*
 * AetherOS — User account management
 * File: kernel/core/users.c
 */

#include "aether/users.h"
#include "aether/config.h"
#include "aether/fat32.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── Global state ─────────────────────────────────────────────────────────── */

user_t g_users[AETHER_MAX_USERS];
u32    g_user_count = 0;
int    g_current_uid = 0;   /* log in as first user (admin) by default */

/* ── Internal helpers ─────────────────────────────────────────────────────── */

u32 users_djb2(const char *s)
{
    u32 h = 5381;
    while (*s) h = ((h << 5) + h) + (u8)*s++;
    return h;
}

static int kstrlen_u(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void kstrcpy_u(char *dst, const char *src, int max)
{
    int i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int kstrcmp_u(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}

/* Parse decimal string → u32 */
static u32 katu32(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (u32)(*s++ - '0');
    return v;
}

/* Write u32 as decimal into buf; returns chars written */
static int ku32toa(u32 v, char *buf, int len)
{
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    if (n >= len) return 0;
    for (int i = n - 1; i >= 0; i--) *buf++ = tmp[i];
    *buf = '\0';
    return n;
}

/* ── users_save ───────────────────────────────────────────────────────────── */

int users_save(void)
{
    /* Build file content: "name:hash:role\n" per user */
    static char buf[2048];
    int pos = 0;

    for (u32 i = 0; i < AETHER_MAX_USERS; i++) {
        if (!g_users[i].active) continue;
        int nlen = kstrlen_u(g_users[i].name);
        if (pos + nlen + 1 + 12 + 1 + 2 + 1 >= 2048) break;

        /* name */
        for (int j = 0; j < nlen; j++) buf[pos++] = g_users[i].name[j];
        buf[pos++] = ':';
        /* hash (decimal) */
        char hbuf[12];
        int hlen = ku32toa(g_users[i].pw_hash, hbuf, 12);
        for (int j = 0; j < hlen; j++) buf[pos++] = hbuf[j];
        buf[pos++] = ':';
        /* role */
        buf[pos++] = (char)('0' + g_users[i].role);
        buf[pos++] = '\n';
    }

    int fh = fat32_create("/config/users.conf");
    if (fh < 0) {
        kwarn("[users] cannot write /config/users.conf\n");
        return -1;
    }
    int written = fat32_write(fh, (const u8 *)buf, (u32)pos);
    fat32_close(fh);
    return (written == pos) ? 0 : -1;
}

/* ── users_init ───────────────────────────────────────────────────────────── */

void users_init(void)
{
    /* Zero the table */
    for (int i = 0; i < AETHER_MAX_USERS; i++) {
        g_users[i].active = 0;
        g_users[i].name[0] = '\0';
        g_users[i].pw_hash = 0;
        g_users[i].role = AETHER_ROLE_USER;
    }
    g_user_count = 0;
    g_current_uid = 0;

    if (!fat32_ready()) {
        kinfo("[users] FAT32 not ready — installing default admin\n");
        goto default_admin;
    }

    int fh = fat32_open("/config/users.conf");
    if (fh < 0) {
        kinfo("[users] no users.conf — installing default admin\n");
        goto default_admin;
    }

    /* Read file */
    static char fbuf[2048];
    int total = 0, n;
    while ((n = fat32_read(fh, (u8 *)(fbuf + total),
                           (u32)(2047 - total))) > 0)
        total += n;
    fat32_close(fh);
    fbuf[total] = '\0';

    /* Parse "name:hash:role\n" lines */
    const char *p = fbuf;
    while (*p && g_user_count < AETHER_MAX_USERS) {
        /* Skip blank lines and comments */
        if (*p == '#' || *p == '\n' || *p == '\r') {
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            continue;
        }

        /* name */
        char name[AETHER_NAME_MAX];
        int ni = 0;
        while (*p && *p != ':' && *p != '\n' && ni < AETHER_NAME_MAX - 1)
            name[ni++] = *p++;
        name[ni] = '\0';
        if (*p != ':') { while (*p && *p != '\n') p++; if (*p) p++; continue; }
        p++;

        /* hash */
        const char *hs = p;
        while (*p && *p != ':' && *p != '\n') p++;
        if (*p != ':') { while (*p && *p != '\n') p++; if (*p) p++; continue; }
        u32 hash = katu32(hs);
        p++;

        /* role */
        u8 role = (u8)(*p == '1' ? AETHER_ROLE_ADMIN : AETHER_ROLE_USER);
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        u32 slot = g_user_count++;
        kstrcpy_u(g_users[slot].name, name, AETHER_NAME_MAX);
        g_users[slot].pw_hash = hash;
        g_users[slot].role    = role;
        g_users[slot].active  = 1;
    }

    if (g_user_count > 0) {
        kinfo("[users] loaded %lu user(s) from users.conf\n",
              (unsigned long)g_user_count);

        /* Ensure at least one admin exists */
        int has_admin = 0;
        for (u32 i = 0; i < g_user_count; i++)
            if (g_users[i].role == AETHER_ROLE_ADMIN) { has_admin = 1; break; }

        if (!has_admin) {
            kwarn("[users] no admin found — promoting first user to admin\n");
            g_users[0].role = AETHER_ROLE_ADMIN;
            users_save();
        }
        return;
    }

default_admin:
    /* No users loaded — create default admin with empty password */
    kstrcpy_u(g_users[0].name, "admin", AETHER_NAME_MAX);
    g_users[0].pw_hash = 5381;   /* djb2("") */
    g_users[0].role    = AETHER_ROLE_ADMIN;
    g_users[0].active  = 1;
    g_user_count = 1;
    g_current_uid = 0;
    kinfo("[users] default admin created (empty password)\n");
    users_save();

    (void)kstrcmp_u;  /* suppress unused warning */
}
