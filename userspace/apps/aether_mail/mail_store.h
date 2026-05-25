/*
 * AetherOS — AetherMail storage types and config API
 * File: userspace/apps/aether_mail/mail_store.h
 *
 * Defines all shared data structures (account, folder, message) and the
 * FAT32-backed config load/save API.  Config lives at /email/config on the
 * primary FAT32 volume; message cache lives at /email/cache/.
 */

#ifndef AETHER_MAIL_STORE_H
#define AETHER_MAIL_STORE_H

/* ── Field size limits ──────────────────────────────────────────────────── */

#define MAIL_HOST_MAX     128
#define MAIL_USER_MAX     128
#define MAIL_PASS_MAX     128
#define MAIL_NAME_MAX      64
#define MAIL_EMAIL_MAX    128
#define MAIL_SUBJECT_MAX  128
#define MAIL_FROM_MAX     100
#define MAIL_TO_MAX       100
#define MAIL_DATE_MAX      48
#define MAIL_BODY_MAX    8192
#define MAIL_UID_MAX       24

/* Pool sizes */
#define MAIL_MSG_MAX      64    /* messages cached in RAM per folder */
#define MAIL_FOLDER_MAX    8    /* folder descriptors */
#define MAIL_FOLDER_NAME  32

/* Storage paths */
#define MAIL_CONFIG_PATH  "/email/config"
#define MAIL_CACHE_DIR    "/email/cache"

/* ── Account configuration ──────────────────────────────────────────────── */

typedef struct {
    char  display_name[MAIL_NAME_MAX];
    char  email[MAIL_EMAIL_MAX];

    char           imap_host[MAIL_HOST_MAX];
    unsigned short imap_port;     /* 993 = IMAPS, 143 = plain */
    char           imap_user[MAIL_USER_MAX];
    char           imap_pass[MAIL_PASS_MAX];
    int            imap_tls;      /* 1 = TLS from start (port 993) */

    char           smtp_host[MAIL_HOST_MAX];
    unsigned short smtp_port;     /* 465 = SMTPS, 587 = STARTTLS (not impl) */
    char           smtp_user[MAIL_USER_MAX];
    char           smtp_pass[MAIL_PASS_MAX];
    int            smtp_tls;      /* 1 = TLS from start (port 465) */
} mail_account_t;

/* ── Folder descriptor ───────────────────────────────────────────────────── */

typedef struct {
    char name[MAIL_FOLDER_NAME];   /* IMAP mailbox name, e.g. "INBOX" */
    int  total;
    int  unseen;
} mail_folder_t;

/* ── Message ─────────────────────────────────────────────────────────────── */

typedef struct {
    int  seq_num;                      /* IMAP sequence number (1-based) */
    char uid[MAIL_UID_MAX];
    char from[MAIL_FROM_MAX];
    char to[MAIL_TO_MAX];
    char subject[MAIL_SUBJECT_MAX];
    char date[MAIL_DATE_MAX];
    char body[MAIL_BODY_MAX];
    unsigned char seen;
    unsigned char deleted;
} mail_msg_t;

/* ── Config API ─────────────────────────────────────────────────────────── */

/*
 * Load account config from /email/config.
 * Returns 1 if the file exists and was parsed, 0 otherwise.
 */
int mail_config_load(mail_account_t *acc);

/*
 * Save account config to /email/config (creates /email/ dir if needed).
 * Returns 0 on success, -1 on error.
 */
int mail_config_save(const mail_account_t *acc);

/* Returns 1 if /email/config exists (account has been set up). */
int mail_config_exists(void);

#endif /* AETHER_MAIL_STORE_H */
