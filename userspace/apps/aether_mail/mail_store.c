/*
 * AetherOS — AetherMail config storage
 * File: userspace/apps/aether_mail/mail_store.c
 *
 * Persists the account config in a key=value text file on the FAT32 volume.
 * Format is identical to the AetherOS sys_prefs convention:
 *   key=value\n
 * Lines beginning with '#' are comments; unknown keys are ignored on load.
 */

#include "mail_store.h"
#include <sys.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void trim_crlf(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == '\r' || s[n-1] == '\n' || s[n-1] == ' '))
        s[--n] = '\0';
}

static int parse_kv(const char *line, const char *key, char *val_out, int val_max)
{
    int klen = (int)strlen(key);
    if (strncmp(line, key, (unsigned)klen) != 0 || line[klen] != '=')
        return 0;
    strncpy(val_out, line + klen + 1, (unsigned)(val_max - 1));
    val_out[val_max - 1] = '\0';
    return 1;
}

/* ── mail_config_exists ──────────────────────────────────────────────────── */

int mail_config_exists(void)
{
    long fd = sys_fs_open(MAIL_CONFIG_PATH);
    if (fd < 0) return 0;
    sys_fs_close(fd);
    return 1;
}

/* ── mail_config_load ────────────────────────────────────────────────────── */

int mail_config_load(mail_account_t *acc)
{
    long fd = sys_fs_open(MAIL_CONFIG_PATH);
    if (fd < 0) return 0;

    static char file_buf[4096];
    long n = sys_fs_read(fd, file_buf, (long)sizeof(file_buf) - 1);
    sys_fs_close(fd);
    if (n <= 0) return 0;
    file_buf[n] = '\0';

    memset(acc, 0, sizeof(*acc));

    /* Sensible defaults */
    acc->imap_port = 993;
    acc->imap_tls  = 1;
    acc->smtp_port = 465;
    acc->smtp_tls  = 1;

    char line[256];
    char val[256];
    int  lstart = 0;

    for (int i = 0; i <= (int)n; i++) {
        char c = (i < (int)n) ? file_buf[i] : '\n';
        if (c != '\n') {
            if ((i - lstart) < (int)sizeof(line) - 1)
                line[i - lstart] = c;
            continue;
        }
        line[i - lstart] = '\0';
        trim_crlf(line);
        lstart = i + 1;

        if (line[0] == '#' || line[0] == '\0') continue;

        if (parse_kv(line, "display_name", val, sizeof(val)))
            strncpy(acc->display_name, val, MAIL_NAME_MAX - 1);
        else if (parse_kv(line, "email", val, sizeof(val)))
            strncpy(acc->email, val, MAIL_EMAIL_MAX - 1);
        else if (parse_kv(line, "imap_host", val, sizeof(val)))
            strncpy(acc->imap_host, val, MAIL_HOST_MAX - 1);
        else if (parse_kv(line, "imap_port", val, sizeof(val)))
            acc->imap_port = (unsigned short)atoi(val);
        else if (parse_kv(line, "imap_user", val, sizeof(val)))
            strncpy(acc->imap_user, val, MAIL_USER_MAX - 1);
        else if (parse_kv(line, "imap_pass", val, sizeof(val)))
            strncpy(acc->imap_pass, val, MAIL_PASS_MAX - 1);
        else if (parse_kv(line, "imap_tls", val, sizeof(val)))
            acc->imap_tls = atoi(val);
        else if (parse_kv(line, "smtp_host", val, sizeof(val)))
            strncpy(acc->smtp_host, val, MAIL_HOST_MAX - 1);
        else if (parse_kv(line, "smtp_port", val, sizeof(val)))
            acc->smtp_port = (unsigned short)atoi(val);
        else if (parse_kv(line, "smtp_user", val, sizeof(val)))
            strncpy(acc->smtp_user, val, MAIL_USER_MAX - 1);
        else if (parse_kv(line, "smtp_pass", val, sizeof(val)))
            strncpy(acc->smtp_pass, val, MAIL_PASS_MAX - 1);
        else if (parse_kv(line, "smtp_tls", val, sizeof(val)))
            acc->smtp_tls = atoi(val);
    }

    return 1;
}

/* ── mail_config_save ────────────────────────────────────────────────────── */

int mail_config_save(const mail_account_t *acc)
{
    /* Ensure /email/ directory exists */
    sys_fs_mkdir("/email");

    long fd = sys_fs_create(MAIL_CONFIG_PATH);
    if (fd < 0) return -1;

    static char buf[4096];
    int n = snprintf(buf, sizeof(buf),
        "display_name=%s\n"
        "email=%s\n"
        "imap_host=%s\n"
        "imap_port=%u\n"
        "imap_user=%s\n"
        "imap_pass=%s\n"
        "imap_tls=%d\n"
        "smtp_host=%s\n"
        "smtp_port=%u\n"
        "smtp_user=%s\n"
        "smtp_pass=%s\n"
        "smtp_tls=%d\n",
        acc->display_name,
        acc->email,
        acc->imap_host,
        (unsigned)acc->imap_port,
        acc->imap_user,
        acc->imap_pass,
        acc->imap_tls,
        acc->smtp_host,
        (unsigned)acc->smtp_port,
        acc->smtp_user,
        acc->smtp_pass,
        acc->smtp_tls);

    long written = sys_fs_write(fd, buf, (long)n);
    sys_fs_close(fd);
    return (written == (long)n) ? 0 : -1;
}
