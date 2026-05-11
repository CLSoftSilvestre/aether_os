#ifndef AETHER_ICON_CACHE_H
#define AETHER_ICON_CACHE_H

/*
 * AetherOS — Icon cache (Phase 6.2)
 *
 * Loads .bmp icons from /icons/ on the FAT32 volume on first access and keeps
 * them in a fixed static pool.  Both 24-bpp and 32-bpp BMPs are accepted.
 * Pixels matching GFX_ICON_TRANSPARENT (pure magenta) are treated as
 * transparent when blitted via gfx_icon_blit().
 *
 * Each process that uses icons maintains its own pool — no shared state across
 * the process boundary.
 *
 * Icon BMP naming convention (all files in /icons/):
 *   App icons:   icon_term.bmp  icon_files.bmp  icon_editor.bmp
 *                icon_calc.bmp  icon_tictactoe.bmp  icon_widget.bmp
 *                icon_text.bmp  icon_telnet.bmp
 *   File types:  file_folder.bmp  file_folder_open.bmp
 *                file_txt.bmp  file_as.bmp  file_exec.bmp  file_generic.bmp
 *   Drives:      drive_fat32.bmp  drive_initrd.bmp  drive_afs.bmp
 *
 * Recommended BMP spec for authors:
 *   Size: 48 × 48 pixels (scaled at draw time for smaller targets)
 *   Depth: 24-bpp or 32-bpp, uncompressed
 *   Background / transparent areas: fill with RGB(255, 0, 255) — pure magenta
 */

/* Maximum icon edge dimension the cache will accept. */
#define ICON_MAX_DIM    48

/* Maximum number of distinct icons held simultaneously per process. */
#define ICON_CACHE_MAX  20

typedef struct {
    char     name[32];                            /* lookup key (base name)   */
    unsigned pixels[ICON_MAX_DIM * ICON_MAX_DIM]; /* XRGB8888, top-to-bottom */
    unsigned width;
    unsigned height;
    int      valid;   /* 1 if file was found and loaded; 0 means not found   */
} icon_entry_t;

/*
 * Reset the cache.  Optional — BSS zero-initialisation is sufficient, but
 * calling this before first use is good practice.
 */
void icon_cache_init(void);

/*
 * Return the cache entry for the icon named 'name' (without path or extension).
 * Loads /icons/<name>.bmp from the VFS on the first call for each name.
 * Returns NULL if the file is missing, malformed, or the pool is full.
 * The returned pointer is valid for the lifetime of the process.
 */
const icon_entry_t *icon_cache_get(const char *name);

#endif /* AETHER_ICON_CACHE_H */
