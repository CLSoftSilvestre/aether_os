/*
 * AetherOS — AetherMail IMAP4rev1 client header
 * File: userspace/apps/aether_mail/imap.h
 *
 * Minimal IMAP4rev1 (RFC 3501) client supporting:
 *   CAPABILITY, LOGIN, LIST, SELECT, SEARCH, FETCH, STORE, EXPUNGE, LOGOUT.
 *
 * TLS support: when compiled with HAVE_MBEDTLS, connects over IMAPS (port 993).
 * Without TLS, connects plain on port 143.
 */

#ifndef AETHER_MAIL_IMAP_H
#define AETHER_MAIL_IMAP_H

#include "mail_store.h"

/* ── Protocol constants ─────────────────────────────────────────────────── */

#define IMAP_LINE_MAX   2048   /* max length of one protocol line */
#define IMAP_RECV_CAP   8192   /* TCP receive ring buffer capacity */
#define IMAP_LIT_CAP    (MAIL_BODY_MAX + 256)

/* ── Connection handle ───────────────────────────────────────────────────── */

typedef struct {
    long  fd;               /* TCP socket fd */
    void *tls;              /* tls_conn_t* if TLS; NULL for plain */
    int   tag_seq;          /* monotonic command tag counter */
    /* Receive ring buffer */
    char  rbuf[IMAP_RECV_CAP];
    int   rstart;
    int   rlen;
    /* Parsed state */
    int   selected_exists;  /* EXISTS count from last SELECT */
    int   selected_unseen;  /* UNSEEN from last SELECT */
} imap_conn_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/*
 * Connect TCP socket and optionally upgrade to TLS.
 * Returns 0 on success, -1 on failure.
 * Reads the server greeting.
 */
int  imap_connect(imap_conn_t *c, const char *host,
                  unsigned short port, int use_tls);

/* LOGIN user pass.  Returns 1 = OK, 0 = NO/BAD, -1 = error. */
int  imap_login(imap_conn_t *c, const char *user, const char *pass);

/*
 * LIST "" "*"  — enumerate mailboxes.
 * Fills folders[] (up to max), sets *count_out.
 * Returns 0 on success, -1 on error.
 */
int  imap_list(imap_conn_t *c, mail_folder_t *folders, int max, int *count_out);

/*
 * SELECT mailbox.
 * Sets c->selected_exists and c->selected_unseen.
 * Returns 1 = OK, 0 = NO/BAD, -1 = error.
 */
int  imap_select(imap_conn_t *c, const char *mailbox);

/*
 * SEARCH ALL — returns sequence numbers of all messages.
 * seqnums[] receives up to max entries; *count_out is set.
 * Returns 0 on success, -1 on error.
 */
int  imap_search_all(imap_conn_t *c, int *seqnums, int max, int *count_out);

/*
 * FETCH first:last (BODY.PEEK[HEADER.FIELDS (FROM SUBJECT DATE)] FLAGS)
 * Fills msgs[] (up to max); sets *count_out.
 * Returns 0 on success, -1 on error.
 */
int  imap_fetch_headers(imap_conn_t *c, int first, int last,
                        mail_msg_t *msgs, int max, int *count_out);

/*
 * FETCH seq_num BODY.PEEK[TEXT]
 * Fills body[] (at most body_max-1 bytes, NUL-terminated).
 * Returns 0 on success, -1 on error.
 */
int  imap_fetch_body(imap_conn_t *c, int seq_num, char *body, int body_max);

/*
 * STORE seq_num +FLAGS (\Deleted)  then  EXPUNGE.
 * Returns 0 on success, -1 on error.
 */
int  imap_delete_and_expunge(imap_conn_t *c, int seq_num);

/* LOGOUT and close the socket. */
void imap_logout(imap_conn_t *c);

#endif /* AETHER_MAIL_IMAP_H */
