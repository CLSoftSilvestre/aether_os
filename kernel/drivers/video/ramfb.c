/*
 * QEMU ramfb driver — AetherOS
 * File: kernel/drivers/video/ramfb.c
 *
 * ramfb is a QEMU device that exposes a framebuffer backed by guest RAM.
 * The guest configures it by writing a RamFBCfg struct to the "etc/ramfb"
 * fw_cfg file.  QEMU then renders whatever is at the configured physical
 * address into the display window.
 *
 * Requires QEMU flags: -device ramfb -vga none
 */

#include "drivers/video/ramfb.h"
#include "drivers/video/fw_cfg.h"
#include "drivers/video/fb.h"
#include "aether/mm.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── ramfb configuration struct (all fields big-endian) ─────────────── */

#define RAMFB_WIDTH   1024U
#define RAMFB_HEIGHT   768U

/*
 * DRM_FORMAT_XRGB8888 = fourcc('X','R','2','4') = 0x34325258
 * Stored big-endian: 0x58523234
 */
#define DRM_FORMAT_XRGB8888_LE  0x34325258U

struct ramfb_cfg {
    u64 addr;     /* big-endian: guest physical address of framebuffer */
    u32 fourcc;   /* big-endian: pixel format */
    u32 flags;    /* big-endian: 0 */
    u32 width;    /* big-endian */
    u32 height;   /* big-endian */
    u32 stride;   /* big-endian: bytes per row */
} __attribute__((packed));

/* ── Byte-swap helpers ───────────────────────────────────────────────── */

static inline u32 bswap32(u32 v)
{
    return ((v & 0x000000FFu) << 24)
         | ((v & 0x0000FF00u) <<  8)
         | ((v & 0x00FF0000u) >>  8)
         | ((v & 0xFF000000u) >> 24);
}

static inline u64 bswap64(u64 v)
{
    return ((u64)bswap32((u32)(v & 0xFFFFFFFFu)) << 32)
         |  (u64)bswap32((u32)(v >> 32));
}

/* ── Framebuffer global state (declared in fb.c) ─────────────────────── */

volatile u32 *fb_base;
u32           fb_width;
u32           fb_height;
u32           fb_stride;

/* ── ramfb_init ──────────────────────────────────────────────────────── */

void ramfb_init(void)
{
    /* 1. Find "etc/ramfb" in the fw_cfg file directory */
    u32 cfg_size = 0;
    u16 selector = fwcfg_find_file("etc/ramfb", &cfg_size);
    if (selector == 0) {
        kwarn("ramfb: 'etc/ramfb' not found in fw_cfg — no display device?\n");
        kwarn("ramfb: ensure QEMU is launched with:  -device ramfb -vga none\n");
        return;
    }
    kinfo("ramfb: found 'etc/ramfb' selector=0x%x size=%lu\n",
          (unsigned)selector, (unsigned long)cfg_size);

    /* 2. Allocate contiguous physical pages for the framebuffer.
     *    1024×768×4 = 3,145,728 bytes = 768 pages at 4KB each. */
    u32 fb_pages = (RAMFB_WIDTH * RAMFB_HEIGHT * 4 + 4095u) / 4096u;
    uintptr_t fb_phys = pmm_alloc_pages(fb_pages);
    if (fb_phys == 0) {
        kerror("ramfb: PMM cannot allocate %lu contiguous pages\n",
               (unsigned long)fb_pages);
        return;
    }
    kinfo("ramfb: framebuffer at 0x%lx (%lu pages, %lu KB)\n",
          (unsigned long)fb_phys,
          (unsigned long)fb_pages,
          (unsigned long)(fb_pages * 4));

    /* 3. Build the RamFBCfg struct (big-endian fields) */
    struct ramfb_cfg cfg;
    cfg.addr   = bswap64((u64)fb_phys);
    cfg.fourcc = bswap32(DRM_FORMAT_XRGB8888_LE);
    cfg.flags  = 0;
    cfg.width  = bswap32(RAMFB_WIDTH);
    cfg.height = bswap32(RAMFB_HEIGHT);
    cfg.stride = bswap32(RAMFB_WIDTH * 4u);

    /* 4. Write the config to fw_cfg (triggers QEMU to register the display) */
    fwcfg_write_file(selector, &cfg, sizeof(cfg));

    /* 5. Set up the fb_* globals for fb.c / fb_console.c */
    fb_base   = (volatile u32 *)fb_phys;
    fb_width  = RAMFB_WIDTH;
    fb_height = RAMFB_HEIGHT;
    fb_stride = RAMFB_WIDTH * 4u;

    /* 6. Clear the framebuffer to a dark background */
    u32 total = RAMFB_WIDTH * RAMFB_HEIGHT;
    for (u32 i = 0; i < total; i++)
        fb_base[i] = FB_RGB(18, 18, 24);   /* near-black #121218 */

    __asm__ volatile("dsb sy\nisb" ::: "memory");

    kinfo("ramfb: %ux%u XRGB8888 framebuffer ready\n",
          (unsigned)RAMFB_WIDTH, (unsigned)RAMFB_HEIGHT);
}
