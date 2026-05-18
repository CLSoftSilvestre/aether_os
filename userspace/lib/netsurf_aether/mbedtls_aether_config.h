/*
 * mbedtls_aether_config.h — Minimal mbedTLS 3.6 config for AetherOS
 *
 * Selected via:  MBEDTLS_CONFIG_FILE="mbedtls_aether_config.h"
 *
 * Goals:
 *   - TLS 1.2 client-only (no TLS 1.3, no DTLS, no server)
 *   - No PSA crypto (avoids mbedTLS 3.x dependency on psa_crypto_init)
 *   - No platform entropy (/dev/urandom not available on AetherOS)
 *   - Custom entropy via mbedtls_hardware_poll() in tls_aether.c
 *   - Cipher suites: ECDHE-RSA/ECDSA-AES128-GCM-SHA256 (modern servers)
 *                    RSA-AES128-CBC-SHA256 (fallback)
 *   - SNI: enabled (required for virtual hosting on shared IPs)
 *   - Certificate verification: OPTIONAL (QEMU RTC is often epoch=0)
 *
 * Excluded:
 *   - MBEDTLS_PSA_CRYPTO_C     — no PSA
 *   - MBEDTLS_SSL_PROTO_TLS1_3 — TLS 1.3 requires PSA in mbedTLS 3.x
 *   - MBEDTLS_NET_C            — we use custom I/O callbacks
 *   - MBEDTLS_TIMING_C         — uses setitimer (not in libaether_posix)
 *   - MBEDTLS_THREADING_C      — single-threaded
 *   - MBEDTLS_DEBUG_C          — reduces binary size
 */

#ifndef MBEDTLS_AETHER_CONFIG_H
#define MBEDTLS_AETHER_CONFIG_H

/* ── System headers available ────────────────────────────────────────────── */

/* MBEDTLS_HAVE_ASM intentionally disabled.
 *
 * The AArch64 MULADDC inline assembly in bn_mul.h uses "+r" constraints for
 * uintptr_t locals (muladdc_d / muladdc_s) and the "%x" register modifier.
 * At -O0 the compiler keeps these locals on the stack rather than in registers,
 * which can cause the asm to operate on stale values and produce wrong bignum
 * results → ECDSA ServerKeyExchange verification fails with ECP_VERIFY_FAILED.
 *
 * Without MBEDTLS_HAVE_ASM, mbedTLS falls back to the __uint128_t pure-C path
 * (MBEDTLS_HAVE_UDBL): r = s[i] * (uint128_t)b; r0 = low; r1 = high.
 * GCC/AArch64 compiles this to a pair of mul/umulh instructions — identical
 * output to the asm, but register-allocated correctly at all optimisation levels.
 */

#define MBEDTLS_HAVE_TIME          /* time() via time_posix.c  */
/* No MBEDTLS_HAVE_TIME_DATE: avoids cert expiry checks that fail when
 * RTC returns 0 (epoch) in QEMU — connection still works without it. */

/* ── Platform / memory ───────────────────────────────────────────────────── */

#define MBEDTLS_PLATFORM_C
/* Use libaether_posix malloc/free (8 MB free-list heap) */
#define MBEDTLS_PLATFORM_MEMORY

/* platform_util.c needs mbedtls_ms_time().  It probes for POSIX or Windows
 * and errors out if neither is detected.  Our freestanding env has neither,
 * so we provide our own via sys_get_ticks() in tls_aether.c. */
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* ── Entropy / RNG ───────────────────────────────────────────────────────── */

/* No /dev/urandom.  We implement mbedtls_hardware_poll() in tls_aether.c
 * using sys_rtc_get() XOR'd with sys_get_ticks() via splitmix64.
 * This is not cryptographically strong entropy but sufficient for a
 * hobby OS where we're prioritising connectivity over NSA-grade security. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* ── TLS layer ───────────────────────────────────────────────────────────── */

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C              /* client-side handshake */
#define MBEDTLS_SSL_PROTO_TLS1_2       /* TLS 1.2 only */
/* NO MBEDTLS_SSL_PROTO_TLS1_3 — would require PSA crypto init */

/* TLS features */
#define MBEDTLS_SSL_SERVER_NAME_INDICATION  /* SNI — required for most HTTPS */
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC        /* RFC 7366 security improvement */
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET  /* RFC 7627 security improvement */
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH     /* RFC 6066 */
#define MBEDTLS_SSL_ALPN                    /* ALPN (HTTP/1.1 negotiation) */

/* ── Key exchange ────────────────────────────────────────────────────────── */

#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED    /* most common modern servers */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED  /* e.g. Let's Encrypt ECDSA certs */
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED           /* RSA key exchange fallback */

/* ── Symmetric ciphers ───────────────────────────────────────────────────── */

#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C                  /* AES-GCM (preferred TLS 1.2 AEAD) */
#define MBEDTLS_CIPHER_MODE_CBC        /* AES-CBC fallback */
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_PADDING_PKCS7

/* ── Hash / MAC ──────────────────────────────────────────────────────────── */

#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C               /* SHA-256 (required for TLS 1.2) */
#define MBEDTLS_SHA512_C               /* SHA-384/512 cipher suites */
#define MBEDTLS_SHA1_C                 /* needed for some RSA cert signatures */

/* ── Asymmetric crypto ───────────────────────────────────────────────────── */

/* RSA */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15              /* RSA PKCS#1 v1.5 signatures */
#define MBEDTLS_PKCS1_V21              /* RSA-PSS/OAEP */

/* Elliptic Curve */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED   /* P-256 — most common */
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED   /* P-384 — used by some CAs */
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED   /* P-521 */
/* MBEDTLS_ECP_NIST_OPTIM intentionally disabled.
 *
 * The fast modular reduction for NIST primes (ecp_mod_p256/p384) manipulates
 * MPI limbs as 32-bit words via aliased pointer casts.  At -O0 with AArch64
 * the compiler may not always honour the aliasing assumptions in the carry
 * chain, producing wrong intermediate values and a final R.X ≠ r result.
 * Using the generic Montgomery reduction is slower but correct at all levels.
 */

/* Public key abstraction */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* ── X.509 / certificate parsing ─────────────────────────────────────────── */

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

#define MBEDTLS_PEM_PARSE_C            /* PEM (base64) decoding for CA bundle */
#define MBEDTLS_BASE64_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C           /* needed by pk_write / ecdsa_write */
#define MBEDTLS_OID_C

/* ── Misc ─────────────────────────────────────────────────────────────────── */

#define MBEDTLS_ERROR_C                /* human-readable error strings */
#define MBEDTLS_VERSION_C
#define MBEDTLS_DEBUG_C                /* handshake state + error logging via UART */

/* Constant-time comparison helpers (security) */
/* MBEDTLS_CONSTANT_TIME_C is defined unconditionally in mbedTLS 3.x internals */

/* NOTE: Do NOT include "mbedtls/check_config.h" here.
 * build_info.h includes it AFTER config_adjust_legacy_crypto.h runs, which
 * is what derives MBEDTLS_MD_CAN_SHA256 from MBEDTLS_SHA256_C etc.
 * Including check_config.h from inside the config file causes it to run
 * before those derived macros exist, producing spurious prerequisite errors. */

#endif /* MBEDTLS_AETHER_CONFIG_H */
