#ifndef NETSURF_AETHER_H
#define NETSURF_AETHER_H

/*
 * AetherOS NetSurf bridge — compile-time configuration.
 *
 * This header is #included before any NetSurf source so we can override
 * feature flags before netsurf's own config.h takes effect.
 *
 * Build system defines NETSURF_AETHER=1 for all netsurf_aether bridge files
 * and for the vendor_netsurf library so these settings take effect.
 */

/* Platform identity */
#define NETSURF_FB_FRONTEND        1   /* treat ourselves as a framebuffer frontend */
#define NETSURF_UA_NAME            "NetSurf"
#define NETSURF_UA_VERSION         "3.11"

/* Memory budget: Pi 5 has 4-8 GB; QEMU gets -m 1G from run_qemu.sh */
#define NETSURF_LLCACHE_SIZE       (32 * 1024 * 1024)   /* 32 MB low-level cache */
#define NETSURF_MEMORY_CACHE_SIZE  (16 * 1024 * 1024)   /* 16 MB object cache   */

/* Disable optional features not needed for MVP */
#define NETSURF_USE_RSVG          0   /* no RSVG SVG renderer */
#define NETSURF_USE_WEBP          0   /* no WebP */
#define NETSURF_USE_HARU          0   /* no PDF export */
#define NETSURF_USE_EXPAT         0   /* no expat XML parser */
#define NETSURF_USE_CURL          0   /* we provide our own HTTP fetcher */
#define NETSURF_USE_DUKTAPE       0   /* replaced by QuickJS */

/* Paths on the AetherOS FAT32 disk */
#define NETSURF_RESOURCE_PATH     "/resources/"
#define NETSURF_CACHE_PATH        "/tmp/nscache/"

/* Schedule queue depth (cooperative scheduler, single-threaded) */
#define NETSURF_AETHER_SCHED_MAX  64

#endif /* NETSURF_AETHER_H */
