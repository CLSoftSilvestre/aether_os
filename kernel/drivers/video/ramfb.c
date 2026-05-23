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
 *
 * Dynamic resolution (System Preferences):
 *   ramfb_reconfigure(w, h) re-allocates the FB and re-writes fw_cfg.
 *   Call it after fat32_mount() and before process_spawn() so init sees
 *   the correct dimensions.  The old FB memory is orphaned (no pmm_free yet).
 */

#include "drivers/video/ramfb.h"
#include "drivers/video/fw_cfg.h"
#include "drivers/video/fb.h"
#include "aether/mm.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── ramfb configuration struct (all fields big-endian) ─────────────── */

#define RAMFB_DEFAULT_W  1280U
#define RAMFB_DEFAULT_H   720U

/*
 * Supported resolution presets — ramfb_reconfigure validates against this list.
 */
#define RAMFB_NUM_PRESETS  5
static const u32 k_preset_w[RAMFB_NUM_PRESETS] = { 640, 800, 1024, 1280, 1920 };
static const u32 k_preset_h[RAMFB_NUM_PRESETS] = { 480, 600,  768,  720, 1080 };

/*
 * DRM_FORMAT_XRGB8888 = fourcc('X','R','2','4') = 0x34325258
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

/* ── Framebuffer global state (declared in fb.h, used by fb.c et al.) ── */

volatile u32 *fb_base;
u32           fb_width;
u32           fb_height;
u32           fb_stride;

/* fw_cfg selector saved by ramfb_init() for later reconfiguration */
static u16 g_ramfb_sel = 0;

/* ── ramfb_configure_hw — common setup helper ───────────────────────── */

static void ramfb_configure_hw(u32 w, u32 h)
{
    u32 fb_pages = (w * h * 4u + 4095u) / 4096u;
    uintptr_t fb_phys = pmm_alloc_pages(fb_pages);
    if (fb_phys == 0) {
        kerror("ramfb: PMM cannot allocate %lu pages for %ux%u\n",
               (unsigned long)fb_pages, (unsigned)w, (unsigned)h);
        return;
    }

    struct ramfb_cfg cfg;
    cfg.addr   = bswap64((u64)fb_phys);
    cfg.fourcc = bswap32(DRM_FORMAT_XRGB8888_LE);
    cfg.flags  = 0;
    cfg.width  = bswap32(w);
    cfg.height = bswap32(h);
    cfg.stride = bswap32(w * 4u);

    fwcfg_write_file(g_ramfb_sel, &cfg, sizeof(cfg));

    fb_base   = (volatile u32 *)fb_phys;
    fb_width  = w;
    fb_height = h;
    fb_stride = w * 4u;

    u32 total = w * h;
    for (u32 i = 0; i < total; i++)
        fb_base[i] = FB_RGB(18, 18, 24);   /* near-black #121218 */

    __asm__ volatile("dsb sy\nisb" ::: "memory");

    kinfo("ramfb: %ux%u XRGB8888 framebuffer at 0x%lx (%lu KB)\n",
          (unsigned)w, (unsigned)h,
          (unsigned long)fb_phys,
          (unsigned long)(fb_pages * 4));
}

/* ── ramfb_init ──────────────────────────────────────────────────────── */

void ramfb_init(void)
{
    u32 cfg_size = 0;
    g_ramfb_sel = fwcfg_find_file("etc/ramfb", &cfg_size);
    if (g_ramfb_sel == 0) {
        kwarn("ramfb: 'etc/ramfb' not found — no display device?\n");
        kwarn("ramfb: ensure QEMU is launched with: -device ramfb -vga none\n");
        return;
    }
    kinfo("ramfb: found 'etc/ramfb' selector=0x%x\n", (unsigned)g_ramfb_sel);
    ramfb_configure_hw(RAMFB_DEFAULT_W, RAMFB_DEFAULT_H);
}

/* ── ramfb_reconfigure ───────────────────────────────────────────────── */

/*
 * Switch to a new resolution.  Only call this before spawning init (no apps
 * are running, so no compositor buffers need to be invalidated).
 * The old framebuffer memory is orphaned — acceptable while pmm_free is absent.
 * Returns 0 on success, -1 if w/h is not in the supported preset list.
 */
int ramfb_reconfigure(u32 w, u32 h)
{
    if (g_ramfb_sel == 0) return -1;
    if (w == fb_width && h == fb_height) return 0;   /* already correct */

    /* Validate against supported presets */
    int ok = 0;
    for (int i = 0; i < RAMFB_NUM_PRESETS; i++) {
        if (k_preset_w[i] == w && k_preset_h[i] == h) { ok = 1; break; }
    }
    if (!ok) {
        kwarn("ramfb: unsupported resolution %ux%u — keeping %ux%u\n",
              (unsigned)w, (unsigned)h,
              (unsigned)fb_width, (unsigned)fb_height);
        return -1;
    }

    kinfo("ramfb: reconfiguring %ux%u → %ux%u\n",
          (unsigned)fb_width, (unsigned)fb_height, (unsigned)w, (unsigned)h);
    ramfb_configure_hw(w, h);
    return 0;
}
