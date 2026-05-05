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

/* ── Software bilinear upsample ───────────────────────────────────────── */
/*
 * Bilinear upsample from src (src_w × src_h) to dst (dst_w × dst_h).
 * Used after TFU downsampling to reconstruct the blur result at full size.
 * Fixed-point 8-bit fractional coordinates (256 = 1.0).
 */
static void __attribute__((unused))
sw_bilinear_upsample(const volatile u32 *src, volatile u32 *dst,
                                  u32 src_w, u32 src_h,
                                  u32 dst_w, u32 dst_h)
{
    if (dst_w < 2u || dst_h < 2u) return;

    for (u32 dy = 0; dy < dst_h; dy++) {
        u32 sy_fxp = (dy * (src_h - 1u) * 256u) / (dst_h - 1u);
        u32 sy0 = sy_fxp >> 8;
        u32 fy  = sy_fxp & 0xFFu;
        u32 sy1 = (sy0 + 1u < src_h) ? sy0 + 1u : sy0;

        for (u32 dx = 0; dx < dst_w; dx++) {
            u32 sx_fxp = (dx * (src_w - 1u) * 256u) / (dst_w - 1u);
            u32 sx0 = sx_fxp >> 8;
            u32 fx  = sx_fxp & 0xFFu;
            u32 sx1 = (sx0 + 1u < src_w) ? sx0 + 1u : sx0;

            u32 p00 = src[sy0 * src_w + sx0];
            u32 p10 = src[sy0 * src_w + sx1];
            u32 p01 = src[sy1 * src_w + sx0];
            u32 p11 = src[sy1 * src_w + sx1];

#define BLERP(p00, p10, p01, p11, sh, mask) \
    ((((((p00)>>(sh))&(mask)) * (256u-fx) * (256u-fy) + \
       (((p10)>>(sh))&(mask)) * fx        * (256u-fy) + \
       (((p01)>>(sh))&(mask)) * (256u-fx) * fy        + \
       (((p11)>>(sh))&(mask)) * fx        * fy) + (1u << 15)) >> 16)

            dst[dy * dst_w + dx] =
                (BLERP(p00, p10, p01, p11, 16u, 0xFFu) << 16) |
                (BLERP(p00, p10, p01, p11,  8u, 0xFFu) <<  8) |
                 BLERP(p00, p10, p01, p11,  0u, 0xFFu);
#undef BLERP
        }
    }
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

/* ── TFU hardware downsample (Pi 4 only) ──────────────────────────────── */
/*
 * tfu_downsample_2x — use the V3D Texture Formatting Unit to generate
 * a 2× downsampled copy of src_phys into dst_phys using bilinear filtering.
 *
 * Register sequence (from raspberrypi/linux v3d_sched.c):
 *   IIA, ICA, IIS, IUA, IOA, IOS, COEF0-3, then ICFG (kicks the job).
 * Completion: poll V3D_HUB_INTSTS for V3D_HUB_INT_TFUC (bit 1).
 *
 * Returns 0 on success, -1 on timeout or invalid dimensions.
 */
#ifdef AETHER_TARGET_PI4
static int tfu_downsample_2x(uintptr_t src_phys, uintptr_t dst_phys,
                              u32 src_w, u32 src_h)
{
    u32 dst_w = src_w / 2u;
    u32 dst_h = src_h / 2u;
    if (!dst_w || !dst_h) return -1;

    /* Clear any stale TFU-complete flag */
    HUB_WR(V3D_HUB_INTCLR, V3D_HUB_INT_TFUC);

    /* Input: row-major XRGB8888, stride = src_w * 4 bytes */
    HUB_WR(V3D_TFU_IIA,   (u32)(src_phys & 0xFFFFFFFFu));
    HUB_WR(V3D_TFU_ICA,   src_w * 4u);
    HUB_WR(V3D_TFU_IIS,   (src_w << 16) | src_h);
    HUB_WR(V3D_TFU_IUA,   0u);              /* no YUV UV plane */

    /* Output: half-size buffer */
    HUB_WR(V3D_TFU_IOA,   (u32)(dst_phys & 0xFFFFFFFFu));
    HUB_WR(V3D_TFU_IOS,   (dst_w << 16) | dst_h);

    /* Default box filter (0 = use hardware default) */
    HUB_WR(V3D_TFU_COEF0, 0u);
    HUB_WR(V3D_TFU_COEF1, 0u);
    HUB_WR(V3D_TFU_COEF2, 0u);
    HUB_WR(V3D_TFU_COEF3, 0u);

    /* ICFG: raster linear (TTYPE=0), RGBA8 format, 0 levels field = 1 output level.
     * Writing this register kicks the job. */
    u32 icfg = (TFU_ICFG_TTYPE_RASTER  << TFU_ICFG_TTYPE_SHIFT)  |
               (TFU_ICFG_FORMAT_RGBA8  << TFU_ICFG_FORMAT_SHIFT)  |
               (0u                     << TFU_ICFG_NLEVELS_SHIFT);
    __asm__ volatile("dsb sy" ::: "memory");  /* ensure all writes visible before kick */
    HUB_WR(V3D_TFU_ICFG, icfg);

    /* Poll for completion: TFUC bit set when done */
    u32 timeout = 0x200000u;
    while (!(HUB_RD(V3D_HUB_INTSTS) & V3D_HUB_INT_TFUC) && --timeout)
        __asm__ volatile("nop" ::: "memory");

    HUB_WR(V3D_HUB_INTCLR, V3D_HUB_INT_TFUC);

    if (!timeout) {
        kwarn("[V3D] TFU timeout — falling back to software blur\n");
        return -1;
    }
    return 0;
}

/*
 * v3d_tfu_blur — GPU-accelerated blur via iterative TFU downsampling.
 *
 * Strategy:
 *   Blur radius determines how many 2× downsample passes to chain.
 *   Each pass halves both dimensions; the final bilinear upsample back
 *   to full size creates a progressively stronger blur effect.
 *
 *   radius  1–3  → 1 pass  (approx. Gaussian σ ≈ 1–2 px)
 *   radius  4–7  → 2 passes (approx. σ ≈ 3–5 px)
 *   radius  8+   → 3 passes (approx. σ ≈ 6–10 px)
 *
 * Falls back gracefully if PMM allocation fails.
 */
static int v3d_tfu_blur(uintptr_t src_phys, uintptr_t dst_phys,
                         u32 width, u32 height, u32 radius)
{
    u32 passes = (radius <= 3u) ? 1u : (radius <= 7u) ? 2u : 3u;

    /* Don't let the image shrink below 8×8 */
    u32 min_dim = (width < height) ? width : height;
    while (passes > 1u && (min_dim >> passes) < 8u)
        passes--;

    /* Allocate intermediate level buffers: level[i] is (w>>i+1) × (h>>i+1) */
    uintptr_t level[3]       = {0, 0, 0};
    u32       level_pages[3] = {0, 0, 0};

    for (u32 i = 0; i < passes; i++) {
        u32 lw      = width  >> (i + 1u);
        u32 lh      = height >> (i + 1u);
        u32 nbytes  = lw * lh * 4u;
        level_pages[i] = pages_for(nbytes);
        level[i]       = pmm_alloc_pages(level_pages[i]);
        if (!level[i]) {
            for (u32 j = 0; j < i; j++)
                for (u32 k = 0; k < level_pages[j]; k++)
                    pmm_free_page(level[j] + k * (u32)PMM_PAGE_SIZE);
            return -1;
        }
    }

    /* Downsample chain: src → level[0] → level[1] → ... */
    uintptr_t cur_src = src_phys;
    u32 cur_w = width, cur_h = height;

    for (u32 i = 0; i < passes; i++) {
        if (tfu_downsample_2x(cur_src, level[i], cur_w, cur_h) != 0) {
            for (u32 j = 0; j < passes; j++)
                if (level[j])
                    for (u32 k = 0; k < level_pages[j]; k++)
                        pmm_free_page(level[j] + k * (u32)PMM_PAGE_SIZE);
            return -1;
        }
        cur_src = level[i];
        cur_w >>= 1u;
        cur_h >>= 1u;
    }

    /* Bilinear upsample from smallest level directly back to full size */
    sw_bilinear_upsample(
        (const volatile u32 *)level[passes - 1u],
        (volatile u32 *)dst_phys,
        cur_w, cur_h,
        width, height);

    for (u32 i = 0; i < passes; i++)
        for (u32 k = 0; k < level_pages[i]; k++)
            pmm_free_page(level[i] + k * (u32)PMM_PAGE_SIZE);

    return 0;
}
#endif /* AETHER_TARGET_PI4 */

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
        /* Hardware path (Phase 6.1.5): TFU iterative downsample + bilinear upsample. */
        if (v3d_tfu_blur(src_phys, dst_phys, width, height, radius) == 0)
            return 0;
        /* Fall through to software on TFU failure */
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
