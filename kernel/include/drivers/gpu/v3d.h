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
 * TFU performs texture format conversions and can blit + filter
 * between two buffers; used for hardware-accelerated blur.
 */
#define V3D_TFU_CS        0x400   /* control / status */
#define V3D_TFU_INBASE0   0x404   /* input base PA */
#define V3D_TFU_ICFG      0x414   /* input config: width/height/format */
#define V3D_TFU_OUTBASE   0x418   /* output base PA */
#define V3D_TFU_OUTNUM    0x41C   /* number of output levels */
#define V3D_TFU_OUTCFG    0x420   /* output config */
#define V3D_TFU_COEF0     0x424   /* filter coefficients (4 registers) */
#define V3D_TFU_COEF1     0x428
#define V3D_TFU_COEF2     0x42C
#define V3D_TFU_COEF3     0x430

/* TFU_CS bits */
#define TFU_CS_ACTIVE  (1u << 0)   /* job in progress */
#define TFU_CS_RESET   (1u << 15)  /* software reset */

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

#endif /* DRIVERS_GPU_V3D_H */
