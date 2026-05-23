/*
 * AetherOS — AetherMail SMTP client header
 * File: userspace/apps/aether_mail/smtp.h
 *
 * Minimal SMTP client (RFC 5321) with AUTH LOGIN for sending email.
 * Supports plain TCP (port 587) and direct TLS/SMTPS (port 465) when
 * HAVE_MBEDTLS is defined.
 */

#ifndef AETHER_MAIL_SMTP_H
#define AETHER_MAIL_SMTP_H

#include "mail_store.h"

/*
 * smtp_send — Compose and send one email message.
 *
 * @acc:     Account config (provides SMTP host/port/user/pass/tls).
 * @to:      Recipient email address (single address).
 * @subject: Message subject line.
 * @body:    Plain-text message body.
 *
 * Returns 0 on success, -1 on error.
 * On error a brief description is written to err_out (if non-NULL).
 */
int smtp_send(const mail_account_t *acc,
              const char *to,
              const char *subject,
              const char *body,
              char *err_out, int err_max);

#endif /* AETHER_MAIL_SMTP_H */
