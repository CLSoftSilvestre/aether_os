/*
 * AetherOS — Userspace configuration file helper
 * File: userspace/lib/include/config.h
 *
 * Read/write simple key=value config files on the FAT32 disk at /config/.
 * Uses the VFS syscalls (SYS_FS_*) so this is safe to call from any app.
 *
 * Max file size: 2048 bytes.  Max key length: 32 chars.  Max value: 128 chars.
 */
#ifndef AETHER_USERSPACE_CONFIG_H
#define AETHER_USERSPACE_CONFIG_H

/*
 * cfg_read — find `key` in a key=value file at `path`.
 * Copies the value (without trailing newline) into `out` (null-terminated).
 * Returns 0 on success, -1 if file not found or key not present.
 */
int cfg_read(const char *path, const char *key, char *out, int out_len);

/*
 * cfg_write — set `key=value` in the file at `path`.
 * Creates the file if it does not exist; updates in place otherwise.
 * Returns 0 on success, -1 on error.
 */
int cfg_write(const char *path, const char *key, const char *value);

#endif /* AETHER_USERSPACE_CONFIG_H */
