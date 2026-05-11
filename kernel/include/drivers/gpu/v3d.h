#ifndef DRIVERS_GPU_V3D_H
#define DRIVERS_GPU_V3D_H
/*
 * AetherOS — V3D GPU Driver (Phase 6.1.1)
 * File: kernel/include/drivers/gpu/v3d.h
 *
 * Supports BCM2711 (Pi 4) V3D 4.2.
 * Falls back gracefully to software on QEMU -M virt (no V3D present).
 *
 * V3D on BCM2711 memory map:
 *   Hub registers:   0xFEC00000 – 0xFEC04000 (16 KB)
 *   Core 0 regs:     0xFEC04000 – 0xFEC08000 (16 KB)
 *
 * Architecture overview:
 *   - Buffer Objects (BOs): physically contiguous pages from PMM.
 *     Both kernel and GPU reference them by physical address.
 *   - Blur / compositing: driven by the TFU (Texture Formatting Unit)
 *     when hardware is present; pure-C separable box-blur otherwise.
 *   - QPU compute (shaders): reserved for Phase 6.1.3+ (Mesa port).
 */

#include "aether/types.h"

/* ── MMIO base addresses ──────────────────────────────────────────────── */
#define V3D_HUB_BASE_PI4    0xFEC00000UL
#define V3D_CORE0_BASE_PI4  0xFEC04000UL

/* ── Hub register offsets ─────────────────────────────────────────────── */
#define V3D_HUB_IDENT0    0x000   /* identity word 0: "V3D\x02" magic */
#define V3D_HUB_IDENT1    0x004   /* version / QPU count */
#define V3D_HUB_IDENT2    0x008
#define V3D_HUB_IDENT3    0x00C
#define V3D_HUB_AXICFG    0x010
#define V3D_HUB_UIFCFG    0x014
#define V3D_HUB_INTSTS    0x050   /* interrupt status  */
#define V3D_HUB_INTCLR    0x058   /* interrupt clear   */
#define V3D_HUB_INTENA    0x05C   /* interrupt enable  */

/*
 * Texture Formatting Unit (TFU) — hub register offsets.
 * TFU performs hardware-accelerated texture format conversion and bilinear
 * downsampling (mipmap generation).  Used for GPU-accelerated blur:
 *   downsample 2× via TFU → software bilinear upsample back to full size.
 *
 * Register offsets from Linux kernel drivers/gpu/drm/v3d/v3d_regs.h
 * (raspberrypi/linux, BCM2711 V3D 4.2).
 *
 * Writing TFU_ICFG last starts the job.  Completion is signalled by
 * the V3D_HUB_INT_TFUC bit in V3D_HUB_INTSTS (poll or use interrupt).
 */
#define V3D_TFU_CS    0xe00   /* control / status (r=1 when ready) */
#define V3D_TFU_IIA   0xe04   /* input image address (physical) */
#define V3D_TFU_ICA   0xe08   /* input config A: linear stride in bytes */
#define V3D_TFU_IIS   0xe0c   /* input image size: (width<<16)|height */
#define V3D_TFU_IUA   0xe10   /* input UV address (YUV formats, else 0) */
#define V3D_TFU_IOA   0xe14   /* output image address (physical) */
#define V3D_TFU_IOS   0xe18   /* output image size: (width<<16)|height */
#define V3D_TFU_ICFG  0xe1c   /* format/tiling config — write LAST to kick job */
#define V3D_TFU_COEF0 0xe20   /* filter coefficient 0 (0 = default box filter) */
#define V3D_TFU_COEF1 0xe24
#define V3D_TFU_COEF2 0xe28
#define V3D_TFU_COEF3 0xe2c

/* TFU_ICFG bit fields (V3D 4.2 / BCM2711) */
#define TFU_ICFG_TTYPE_SHIFT    0         /* [2:0] tiling type */
#define TFU_ICFG_TTYPE_RASTER   0u        /*   0 = plain row-major (raster) */
#define TFU_ICFG_FORMAT_SHIFT   4         /* [8:4] pixel format */
#define TFU_ICFG_FORMAT_RGBA8   12u       /*   12 = XRGB8888 / RGBA8 (V3D 4.x) */
#define TFU_ICFG_NLEVELS_SHIFT  12        /* [15:12] mip levels to generate - 1 */
/*   0 in field = generate 1 level (half size from input); matches Mesa usage */

/* Hub interrupt bits (in V3D_HUB_INTSTS / V3D_HUB_INTCLR) */
#define V3D_HUB_INT_TFUC  (1u << 1)       /* TFU job complete */

/* V3D IDENT0 magic — lower 24 bits spell "V3D" */
#define V3D_IDENT0_MAGIC  0x02443356u   /* 'V','3','D', rev=2 */

/* ── Buffer Object pool ───────────────────────────────────────────────── */
#define GPU_MAX_BOS        32
#define GPU_MAX_BO_PAGES   2048   /* 8 MB per BO maximum */

typedef struct {
    uintptr_t  phys;     /* physical address (GPU DMA base) */
    u32        pages;    /* allocation size in 4KB pages */
    u32        handle;   /* 1-based slot index; 0 = free */
} gpu_bo_t;

/* ── GPU capabilities ─────────────────────────────────────────────────── */
typedef struct {
    u32  ident0;           /* raw V3D_HUB_IDENT0 value */
    u32  version_major;
    u32  version_minor;
    u32  num_qpus;
    u32  flags;            /* GPU_CAP_* bitfield */
} gpu_caps_t;

#define GPU_CAP_PRESENT  (1u << 0)   /* V3D hardware detected and powered on */
#define GPU_CAP_TFU      (1u << 1)   /* Texture Formatting Unit present */
#define GPU_CAP_QPU      (1u << 2)   /* QPU general-purpose compute */

/* ── Public kernel API ────────────────────────────────────────────────── */

/* Initialise — powers on V3D via mailbox, reads IDENT registers. */
void  v3d_init(void);

/* True if V3D hardware was detected during v3d_init(). */
bool  v3d_present(void);

/* Fill *caps with hardware capabilities (safe to call before v3d_init). */
void  v3d_get_caps(gpu_caps_t *caps);

/*
 * Buffer Object management.
 * v3d_bo_alloc  — allocate physically-contiguous pages; returns handle ≥1 or -1.
 * v3d_bo_free   — release pages back to PMM.
 * v3d_bo_phys   — return physical address of BO (0 if invalid handle).
 * v3d_bo_size   — return size in bytes of BO (0 if invalid).
 */
int       v3d_bo_alloc(u32 size_bytes);
int       v3d_bo_free(u32 handle);
uintptr_t v3d_bo_phys(u32 handle);
u32       v3d_bo_size(u32 handle);

/*
 * v3d_blur — apply a separable Gaussian blur to an XRGB8888 framebuffer
 * region of size width × height pixels.
 *
 *   src_phys / dst_phys  physical addresses of source and destination buffers
 *                        (must each be ≥ width * height * 4 bytes)
 *   radius               blur radius in pixels (1–16); 0 = identity copy
 *
 * Hardware path (Pi 4, TODO Phase 6.1.5): TFU multi-pass filter.
 * Software path (always available): separable box-blur with PMM temp buffer.
 * Returns 0 on success, -1 on invalid arguments or PMM exhaustion.
 */
int v3d_blur(uintptr_t src_phys, uintptr_t dst_phys,
             u32 width, u32 height, u32 radius);

/*
 * v3d_fb_capture — copy a framebuffer region into a GPU BO.
 *   bo_handle    existing BO with capacity ≥ w*h*4 bytes
 *   src_x, src_y top-left corner in the framebuffer
 *   w, h         dimensions in pixels
 * Returns 0 on success, -1 on error (invalid BO, FB absent, OOB).
 */
int v3d_fb_capture(u32 bo_handle, u32 src_x, u32 src_y, u32 w, u32 h);

/*
 * v3d_blit_to_fb — bilinear-scale a GPU BO and alpha-blend it onto
 * the framebuffer (Phase 6.1.7 window animation compositing).
 *
 *   bo_handle        source BO with XRGB8888 pixels (src_w × src_h)
 *   src_w, src_h     natural size of the BO content
 *   dst_x, dst_y     framebuffer destination top-left
 *   dst_w, dst_h     destination size (scale implicit from src/dst ratio)
 *   alpha            0 = no-op, 255 = fully opaque
 *
 * Returns 0 on success, -1 on error.
 */
int v3d_blit_to_fb(u32 bo_handle,
                   u32 src_w, u32 src_h,
                   u32 dst_x, u32 dst_y,
                   u32 dst_w, u32 dst_h,
                   u8 alpha);

/*
 * v3d_composite_anim — single-pass animation composite (Phase 6.2).
 *
 * For every pixel in [nat_x, nat_y, nat_w × nat_h]: reads the wallpaper
 * background (via wp_ptr/wp_bmpw/wp_bmph forwarded from syscall.c), and if
 * the pixel is inside the scaled rect [anim_x, anim_y, anim_w × anim_h],
 * bilinear-samples the BO (captured at nat_w × nat_h) and alpha-blends it
 * over the background.  Each framebuffer pixel is written exactly once,
 * eliminating the two-step restore+blit flicker.
 */
int v3d_composite_anim(u32 bo_handle,
                        u32 nat_x, u32 nat_y, u32 nat_w, u32 nat_h,
                        u32 anim_x, u32 anim_y, u32 anim_w, u32 anim_h,
                        u8 alpha,
                        uintptr_t wp_ptr, u32 wp_bmpw, u32 wp_bmph);

#endif /* DRIVERS_GPU_V3D_H */
