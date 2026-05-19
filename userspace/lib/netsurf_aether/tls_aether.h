/*
 * tls_aether.h — TLS client API for AetherOS (Iteration 3)
 *
 * Thin wrapper around mbedTLS 3.6 providing a simple synchronous
 * connect/read/write/close API over an existing TCP file descriptor.
 *
 * Usage in fetch_http_aether.c:
 *   1. Call tls_global_init() once at browser startup (loads CA bundle).
 *   2. After a successful TCP connect(), call tls_connect(fd, hostname).
 *   3. Use tls_write() / tls_read_all() instead of send() / recv().
 *   4. Call tls_close() when done (frees context; does NOT close the fd).
 */

#ifndef TLS_AETHER_H
#define TLS_AETHER_H

#include <stddef.h>
#include <stdint.h>

/* Opaque TLS connection context */
typedef struct tls_conn tls_conn_t;

/*
 * tls_global_init() — Parse the embedded Mozilla CA bundle.
 * Call once before any tls_connect().  Idempotent.
 */
void tls_global_init(void);

/*
 * tls_connect() — Perform a TLS 1.2 handshake over an existing TCP fd.
 *
 * @fd:       Connected TCP socket (from connect())
 * @hostname: Server hostname for SNI and certificate CN/SAN matching
 *            (pass NULL to skip SNI and skip hostname verification)
 *
 * Returns a tls_conn_t* on success, NULL on handshake failure.
 * The caller retains ownership of fd; call close(fd) separately.
 */
tls_conn_t *tls_connect(int fd, const char *hostname);

/*
 * tls_write() — Send data over the TLS connection.
 * Returns bytes written (>= 0) or -1 on error.
 */
int tls_write(tls_conn_t *conn, const void *buf, size_t len);

/*
 * tls_read() — Receive up to len bytes from the TLS connection.
 * Returns bytes read (> 0), 0 on EOF, or -1 on error.
 */
int tls_read(tls_conn_t *conn, void *buf, size_t len);

/*
 * tls_read_all() — Receive the full TLS response into a malloc'd buffer.
 * Reads until the server closes the TLS session.
 * Sets *out_len to the number of bytes read.
 * Returns a malloc'd buffer (caller must free), or NULL on failure.
 */
uint8_t *tls_read_all(tls_conn_t *conn, size_t *out_len);

/*
 * tls_close() — Close the TLS session and free the context.
 * Does NOT close the underlying fd.
 */
void tls_close(tls_conn_t *conn);

#endif /* TLS_AETHER_H */
