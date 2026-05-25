/*
 * AetherOS — User account management
 * File: kernel/include/aether/users.h
 *
 * Simple user database stored in /config/users.cfg on FAT32.
 * Passwords are stored as djb2 hashes (adequate for a hobby OS).
 *
 * Config file format (one user per line):
 *   name:hash:role
 * where role: 0 = user, 1 = admin.
 *
 * Default on first boot (no users.conf): admin with empty password (hash=5381).
 */
#ifndef AETHER_USERS_H
#define AETHER_USERS_H

#include "aether/types.h"

#define AETHER_MAX_USERS   16
#define AETHER_ROLE_USER    0
#define AETHER_ROLE_ADMIN   1
#define AETHER_NAME_MAX    32

/* Kernel-internal user record */
typedef struct {
    char name[AETHER_NAME_MAX];
    u32  pw_hash;   /* djb2 hash of plain-text password */
    u8   role;      /* AETHER_ROLE_USER / AETHER_ROLE_ADMIN */
    u8   active;    /* 1 = slot occupied */
} user_t;

/* Global user table — accessible from syscall handlers */
extern user_t g_users[AETHER_MAX_USERS];
extern u32    g_user_count;
extern int    g_current_uid;   /* index into g_users, or -1 if not logged in */

/* Initialise: load from /config/users.cfg (or create default admin). */
void users_init(void);

/* Persist the current user table to /config/users.cfg. */
int  users_save(void);

/* djb2 hash of s (bare-metal, no libc). */
u32  users_djb2(const char *s);

#endif /* AETHER_USERS_H */
