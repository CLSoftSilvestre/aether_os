/*
 * AetherOS — V3D GPU Driver (Phase 6.1.1)
 * File: kernel/drivers/gpu/v3d.c
 *
 * Supports BCM2711 (Pi 4) V3D 4.2.
 * Compiles and runs on QEMU -M virt in software-only mode.
 *
 * Boot sequence (Pi 4 hardware):
 *   1. Power on V3D IP block via VideoCore mailbox.
 *   2. Read V3D_HUB_IDENT0 to confirm hardware identity.
 *   3. Extract version and QPU count from IDENT1.
 *   4. Clear any stale hub interrupts.
 *
 * Buffer Objects:
 *   Physically contiguous pages from pmm_alloc_pages().  GPU accesses
 *   them by physical address; the kernel accesses them via identity map
 *   (VA == PA for kernel RAM on this platform).
 *
 * Blur implementation:
 *   - Hardware (Pi 4, Phase 6.1.5): TFU multi-pass filter — TODO.
 *   - Software (always): separable horizontal + vertical box-blur using
 *     a PMM-allocated ping-pong buffer to avoid read-after-write artefacts.
 */

#include "drivers/gpu/v3d.h"
#include "drivers/gpu/mailbox.h"
#include "aether/mm.h"
#include "aether/printk.h"
#include "aether/types.h"

/* ── MMIO accessor macros ─────────────────────────────────────────────── */

#ifdef AETHER_TARGET_PI4
static volatile u32 * const g_hub = (volatile u32 *)V3D_HUB_BASE_PI4;
#define HUB_RD(off)       (g_hub[(off) / 4])
#define HUB_WR(off, val)  (g_hub[(off) / 4] = (u32)(val))
#else
/* Never called when g_present == false */
#define HUB_RD(off)       (0u)
#define HUB_WR(off, val)  ((void)(val))
#endif

/* ── Driver state ─────────────────────────────────────────────────────── */

static bool       g_present;
static gpu_caps_t g_caps;
static gpu_bo_t   g_bos[GPU_MAX_BOS];

/* ── Internal helpers ─────────────────────────────────────────────────── */

static inline u32 pages_for(u32 bytes)
{
    return (bytes + (u32)PMM_PAGE_SIZE - 1u) / (u32)PMM_PAGE_SIZE;
}

/* ── Software blur (horizontal pass) ─────────────────────────────────── */
/*
 * Box-blur each row of src into dst.  Using a sliding-window running sum
 * avoids the O(radius) inner loop — O(1) per pixel after the first.
 */
static void sw_hblur(const volatile u32 *src, volatile u32 *dst,
                     u32 w, u32 h, u32 radius)
{
    for (u32 y = 0; y < h; y++) {
        const volatile u32 *row = src + y * w;
        volatile u32       *out = dst + y * w;

        u32 sum_r = 0, sum_g = 0, sum_b = 0;
        u32 ksize = 2u * radius + 1u;

        /* Prime the window with the first (radius+1) pixels */
        for (u32 k = 0; k <= radius; k++) {
            u32 px = row[k < w ? k : w - 1u];
            sum_r += (px >> 16) & 0xFFu;
            sum_g += (px >>  8) & 0xFFu;
            sum_b += (px      ) & 0xFFu;
        }
        /* Also include the left-clamped pixels that didn't advance the window */
        for (u32 k = 0; k < radius; k++) {
            u32 px = row[0];
            sum_r += (px >> 16) & 0xFFu;
            sum_g += (px >>  8) & 0xFFu;
            sum_b += (px      ) & 0xFFu;
        }

        for (u32 x = 0; x < w; x++) {
            out[x] = ((sum_r / ksize) << 16) |
                     ((sum_g / ksize) <<  8) |
                      (sum_b / ksize);

            /* Slide window right: remove leftmost, add new right pixel */
            u32 left_x  = (x < radius)     ? 0u      : x - radius;
            u32 right_x = (x + radius + 1u < w) ? x + radius + 1u : w - 1u;

            u32 lpx = row[left_x];
            sum_r -= (lpx >> 16) & 0xFFu;
            sum_g -= (lpx >>  8) & 0xFFu;
            sum_b -= (lpx      ) & 0xFFu;

            u32 rpx = row[right_x];
            sum_r += (rpx >> 16) & 0xFFu;
            sum_g += (rpx >>  8) & 0xFFu;
            sum_b += (rpx      ) & 0xFFu;
        }
    }
}

/* ── Software blur (vertical pass) ───────────────────────────────────── */

static void sw_vblur(const volatile u32 *src, volatile u32 *dst,
                     u32 w, u32 h, u32 radius)
{
    for (u32 x = 0; x < w; x++) {
        u32 sum_r = 0, sum_g = 0, sum_b = 0;
        u32 ksize = 2u * radius + 1u;

        /* Prime window */
        for (u32 k = 0; k <= radius; k++) {
            u32 py = k < h ? k : h - 1u;
            u32 px = src[py * w + x];
            sum_r += (px >> 16) & 0xFFu;
            sum_g += (px >>  8) & 0xFFu;
            sum_b += (px      ) & 0xFFu;
        }
        for (u32 k = 0; k < radius; k++) {
            u32 px = src[x];   /* top-clamped */
            sum_r += (px >> 16) & 0xFFu;
            sum_g += (px >>  8) & 0xFFu;
            sum_b += (px      ) & 0xFFu;
        }

        for (u32 y = 0; y < h; y++) {
            dst[y * w + x] = ((sum_r / ksize) << 16) |
                             ((sum_g / ksize) <<  8) |
                              (sum_b / ksize);

            u32 top_y    = (y < radius)     ? 0u      : y - radius;
            u32 bottom_y = (y + radius + 1u < h) ? y + radius + 1u : h - 1u;

            u32 tpx = src[top_y    * w + x];
            sum_r -= (tpx >> 16) & 0xFFu;
            sum_g -= (tpx >>  8) & 0xFFu;
            sum_b -= (tpx      ) & 0xFFu;

            u32 bpx = src[bottom_y * w + x];
            sum_r += (bpx >> 16) & 0xFFu;
            sum_g += (bpx >>  8) & 0xFFu;
            sum_b += (bpx      ) & 0xFFu;
        }
    }
}

/* ── Initialisation ───────────────────────────────────────────────────── */

void v3d_init(void)
{
    g_present = false;
    for (int i = 0; i < GPU_MAX_BOS; i++)
        g_bos[i].handle = 0;

#ifndef AETHER_TARGET_PI4
    kinfo("[V3D] QEMU mode — GPU hardware absent, software fallback active\n");
    g_caps.flags = 0;
    return;
#else
    /* Step 1: power on V3D via mailbox */
    int rc = mailbox_set_power_state(MBOX_DEV_V3D,
                                     MBOX_POWER_ON | MBOX_POWER_WAIT);
    if (rc < 0) {
        kwarn("[V3D] mailbox power-on failed — software fallback\n");
        g_caps.flags = 0;
        return;
    }

    /* Step 2: identity check */
    u32 ident0 = HUB_RD(V3D_HUB_IDENT0);
    if ((ident0 & 0x00FFFFFFu) != (V3D_IDENT0_MAGIC & 0x00FFFFFFu)) {
        kwarn("[V3D] unexpected IDENT0=0x%08x — software fallback\n", ident0);
        g_caps.flags = 0;
        return;
    }

    /* Step 3: extract capabilities from IDENT1 */
    u32 ident1      = HUB_RD(V3D_HUB_IDENT1);
    u32 ver_major   = (ident1 >> 24) & 0xFFu;
    u32 ver_minor   = (ident1 >> 16) & 0xFFu;
    u32 num_qpus    = (ident1 >>  8) & 0xFFu;

    g_caps.ident0         = ident0;
    g_caps.version_major  = ver_major;
    g_caps.version_minor  = ver_minor;
    g_caps.num_qpus       = num_qpus;
    g_caps.flags          = GPU_CAP_PRESENT | GPU_CAP_TFU | GPU_CAP_QPU;
    g_present             = true;

    /* Step 4: clear stale hub interrupts */
    HUB_WR(V3D_HUB_INTCLR, 0xFFFFFFFFu);

    u32 clk_hz = mailbox_get_clock_rate(MBOX_CLK_V3D);
    kinfo("[V3D] V3D %u.%u — %u QPUs — %u MHz\n",
          ver_major, ver_minor, num_qpus, clk_hz / 1000000u);
#endif
}

bool v3d_present(void) { return g_present; }

void v3d_get_caps(gpu_caps_t *caps)
{
    if (caps) *caps = g_caps;
}

/* ── Buffer Object management ─────────────────────────────────────────── */

int v3d_bo_alloc(u32 size_bytes)
{
    if (!size_bytes) return -1;

    u32 npages = pages_for(size_bytes);
    if (npages > GPU_MAX_BO_PAGES) {
        kwarn("[V3D] bo_alloc: size %u exceeds max (%u pages)\n",
              size_bytes, GPU_MAX_BO_PAGES);
        return -1;
    }

    for (int i = 0; i < GPU_MAX_BOS; i++) {
        if (g_bos[i].handle) continue;

        uintptr_t phys = pmm_alloc_pages(npages);
        if (!phys) {
            kwarn("[V3D] bo_alloc: PMM out of pages (requested %u)\n", npages);
            return -1;
        }

        g_bos[i].phys   = phys;
        g_bos[i].pages  = npages;
        g_bos[i].handle = (u32)(i + 1);

        kinfo("[V3D] BO alloc: handle=%u phys=0x%lx pages=%u (%u KB)\n",
              g_bos[i].handle, (unsigned long)phys,
              npages, npages * 4u);
        return (int)(i + 1);
    }

    kwarn("[V3D] bo_alloc: BO pool exhausted (%d slots)\n", GPU_MAX_BOS);
    return -1;
}

int v3d_bo_free(u32 handle)
{
    if (!handle || handle > (u32)GPU_MAX_BOS) return -1;

    gpu_bo_t *bo = &g_bos[handle - 1u];
    if (!bo->handle) return -1;

    for (u32 i = 0; i < bo->pages; i++)
        pmm_free_page(bo->phys + i * (u32)PMM_PAGE_SIZE);

    kinfo("[V3D] BO free: handle=%u phys=0x%lx\n",
          handle, (unsigned long)bo->phys);

    bo->handle = 0;
    bo->phys   = 0;
    bo->pages  = 0;
    return 0;
}

uintptr_t v3d_bo_phys(u32 handle)
{
    if (!handle || handle > (u32)GPU_MAX_BOS) return 0;
    return g_bos[handle - 1u].phys;
}

u32 v3d_bo_size(u32 handle)
{
    if (!handle || handle > (u32)GPU_MAX_BOS) return 0;
    return g_bos[handle - 1u].pages * (u32)PMM_PAGE_SIZE;
}

/* ── Blur ─────────────────────────────────────────────────────────────── */

int v3d_blur(uintptr_t src_phys, uintptr_t dst_phys,
             u32 width, u32 height, u32 radius)
{
    if (!src_phys || !dst_phys || !width || !height) return -1;

    /* Clamp radius to a sane range */
    if (radius > 16u) radius = 16u;

    /* Identity copy for radius == 0 */
    if (!radius) {
        const volatile u32 *s = (const volatile u32 *)src_phys;
        volatile u32       *d = (volatile u32 *)dst_phys;
        u32 n = width * height;
        for (u32 i = 0; i < n; i++) d[i] = s[i];
        return 0;
    }

#ifdef AETHER_TARGET_PI4
    if (g_present) {
        /*
         * Hardware path (Phase 6.1.5 TODO):
         *   Drive TFU with bilinear-sampled passes to approximate Gaussian blur.
         *   Multi-pass approach: blit with fractional offsets, accumulate with
         *   additive blending into dst.  For now fall through to software.
         */
    }
#endif

    /*
     * Software path: separable box blur using a PMM ping-pong buffer.
     *   Pass 1: horizontal  src  → tmp
     *   Pass 2: vertical    tmp  → dst
     * Applying box blur 3× approximates a Gaussian to within ~3%.
     */
    u32 buf_bytes = width * height * 4u;
    u32 npages    = pages_for(buf_bytes);
    uintptr_t tmp_phys = pmm_alloc_pages(npages);

    if (!tmp_phys) {
        kwarn("[V3D] blur: no temp buffer — skipping\n");
        return -1;
    }

    const volatile u32 *s   = (const volatile u32 *)src_phys;
    volatile u32       *tmp = (volatile u32 *)tmp_phys;
    volatile u32       *d   = (volatile u32 *)dst_phys;

    sw_hblur(s,   tmp, width, height, radius);
    sw_vblur(tmp, d,   width, height, radius);

    for (u32 i = 0; i < npages; i++)
        pmm_free_page(tmp_phys + i * (u32)PMM_PAGE_SIZE);

    return 0;
}
