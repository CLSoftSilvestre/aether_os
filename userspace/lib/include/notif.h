#ifndef AETHER_NOTIF_H
#define AETHER_NOTIF_H

/*
 * AetherOS — Notification IPC
 * File: userspace/lib/include/notif.h
 *
 * Filesystem-based notification store at NOTIF_PATH (FAT32 / AFS).
 * Apps post notifications by opening the file, appending an entry, and
 * updating the header.  The topbar reads the header to show the badge
 * count.  The notification center reads the full store to display entries
 * and can clear them.
 *
 * File layout (binary, fixed-size):
 *   [notif_header_t]                        16 bytes
 *   [notif_entry_t × NOTIF_MAX]             NOTIF_MAX × 104 bytes
 *   Total worst-case: 16 + 32×104 = 3344 bytes
 *
 * Thread safety: single-writer only (no locking).  Race conditions are
 * tolerated for now; a future mutex syscall will fix this.
 */

#define NOTIF_PATH      "/notif"
#define NOTIF_MAGIC     0x4E544600u   /* "NTF\0" */
#define NOTIF_VERSION   1u
#define NOTIF_MAX       32

typedef struct {
    unsigned int magic;      /* NOTIF_MAGIC                        */
    unsigned int version;    /* NOTIF_VERSION                      */
    unsigned int count;      /* total entries stored (0..NOTIF_MAX)*/
    unsigned int unread;     /* entries not yet seen by the user   */
} notif_header_t;            /* 16 bytes                           */

#define NOTIF_APP_MAX   16
#define NOTIF_TITLE_MAX 32
#define NOTIF_MSG_MAX   48

typedef struct {
    unsigned int  id;                       /* monotonic counter              */
    unsigned int  timestamp;                /* Unix epoch seconds             */
    unsigned char read;                     /* 0 = unread, 1 = read           */
    unsigned char flags;                    /* reserved, set 0                */
    unsigned char _pad[2];
    char          app[NOTIF_APP_MAX];       /* source app name (NUL-term)     */
    char          title[NOTIF_TITLE_MAX];   /* short headline                 */
    char          msg[NOTIF_MSG_MAX];       /* body text                      */
} notif_entry_t;             /* 104 bytes                          */

#endif /* AETHER_NOTIF_H */
