/*
 * AetherOS — Kernel configuration file reader/writer
 * File: kernel/include/aether/config.h
 *
 * Reads and writes simple key=value config files on FAT32 at /config/*.conf.
 * Lines starting with '#' are treated as comments and preserved on write.
 * Max file size: 2048 bytes.  Max key length: 32 chars.  Max value: 128 chars.
 *
 * These functions are kernel-internal only; userspace uses the VFS syscalls
 * via userspace/lib/config.c.
 */
#ifndef AETHER_CONFIG_H
#define AETHER_CONFIG_H

#include "aether/types.h"

/*
 * kconfig_read — find `key` in a key=value file at `path` on FAT32.
 * Copies the value (without trailing newline) into `out` (null-terminated).
 * Returns 0 on success, -1 if file not found or key not present.
 */
int kconfig_read(const char *path, const char *key, char *out, u32 out_len);

/*
 * kconfig_write — set `key=value` in `path` on FAT32.
 * If the key already exists, its value is updated in place.
 * If it does not exist, a new `key=value\n` line is appended.
 * If the file does not exist, it is created.
 * Returns 0 on success, -1 on error.
 */
int kconfig_write(const char *path, const char *key, const char *value);

/*
 * katoi — kernel-internal ASCII-to-integer (no libc).
 */
int katoi(const char *s);

/*
 * kitoa — kernel-internal integer-to-ASCII, decimal, null-terminated.
 * Returns number of characters written (excluding NUL).
 */
int kitoa(int v, char *buf, u32 buf_len);

#endif /* AETHER_CONFIG_H */
