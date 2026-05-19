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
#include "drivers/video/fb.h"
#include "drivers/video/cursor.h"
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

/* Double-buffer back buffer (allocated by v3d_dbl_init when compositor claims FB) */
static volatile u32 *g_back_buf;
static u32           g_back_pages;

/* WM4: Dual Kawase blur scratch — two quarter-resolution ping-pong buffers */
#ifdef AETHER_KAWASE_BLUR
static volatile u32 *g_blur_ping;   /* scratch A: (fb_width/4) × (fb_height/4) */
static volatile u32 *g_blur_pong;   /* scratch B: same size, for ping-pong passes */
static u32           g_blur_pages;  /* pages per scratch buffer */
#endif

/* Lumina dark background — fallback when no wallpaper is registered */
#define LUMINA_BG_COLOR  0x0d1117u

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

/* ── FB ↔ BO helpers (Phase 6.1.7) ───────────────────────────────────────── */

/*
 * v3d_fb_capture — snapshot a rectangular region of the live framebuffer
 * into a GPU BO for use as animation source material.
 */
int v3d_fb_capture(u32 bo_handle, u32 src_x, u32 src_y, u32 w, u32 h)
{
    if (!fb_base || !w || !h) return -1;

    uintptr_t dst_phys = v3d_bo_phys(bo_handle);
    if (!dst_phys) return -1;

    /* Clamp to FB bounds */
    if (src_x >= fb_width || src_y >= fb_height) return -1;
    if (src_x + w > fb_width)  w = fb_width  - src_x;
    if (src_y + h > fb_height) h = fb_height - src_y;

    u32 fb_stride_px = fb_stride / 4u;
    u32 *dst = (u32 *)dst_phys;

    for (u32 row = 0; row < h; row++) {
        const volatile u32 *src_row = fb_base + (src_y + row) * fb_stride_px + src_x;
        u32 *dst_row = dst + row * w;
        for (u32 col = 0; col < w; col++)
            dst_row[col] = src_row[col];
    }
    return 0;
}

/*
 * v3d_blit_to_fb — bilinear-scale a GPU BO and alpha-blend it onto the
 * framebuffer at the specified destination position and size.
 *
 * alpha = 0   → fully transparent, framebuffer unchanged (early-out)
 * alpha = 255 → fully opaque, destination pixels replaced
 *
 * Used by the window animation subsystem (6.1.7) to composite scaled+faded
 * window snapshots during open/close spring animations.
 */
int v3d_blit_to_fb(u32 bo_handle,
                   u32 src_w, u32 src_h,
                   u32 dst_x, u32 dst_y,
                   u32 dst_w, u32 dst_h,
                   u8 alpha)
{
    if (!alpha) return 0;
    if (!fb_base || !src_w || !src_h || !dst_w || !dst_h) return -1;

    uintptr_t src_phys = v3d_bo_phys(bo_handle);
    if (!src_phys) return -1;

    if (dst_x >= fb_width || dst_y >= fb_height) return 0;

    /* Clip destination to framebuffer extents */
    u32 clip_w = dst_w;
    u32 clip_h = dst_h;
    if (dst_x + clip_w > fb_width)  clip_w = fb_width  - dst_x;
    if (dst_y + clip_h > fb_height) clip_h = fb_height - dst_y;

    const volatile u32 *src = (const volatile u32 *)src_phys;
    u32 fb_stride_px = fb_stride / 4u;
    u32 inv_a = 255u - (u32)alpha;

    for (u32 dy = 0; dy < clip_h; dy++) {
        u32 fb_y = dst_y + dy;

        /* Fixed-point bilinear: map dy in [0, dst_h) → [0, src_h-1] */
        u32 sy_fxp = (dst_h > 1u) ?
            (dy * (src_h - 1u) * 256u) / (dst_h - 1u) : 0u;
        u32 sy0 = sy_fxp >> 8;
        u32 fy  = sy_fxp & 0xFFu;
        u32 sy1 = (sy0 + 1u < src_h) ? sy0 + 1u : sy0;

        for (u32 dx = 0; dx < clip_w; dx++) {
            u32 fb_x = dst_x + dx;

            u32 sx_fxp = (dst_w > 1u) ?
                (dx * (src_w - 1u) * 256u) / (dst_w - 1u) : 0u;
            u32 sx0 = sx_fxp >> 8;
            u32 fx  = sx_fxp & 0xFFu;
            u32 sx1 = (sx0 + 1u < src_w) ? sx0 + 1u : sx0;

            u32 p00 = src[sy0 * src_w + sx0];
            u32 p10 = src[sy0 * src_w + sx1];
            u32 p01 = src[sy1 * src_w + sx0];
            u32 p11 = src[sy1 * src_w + sx1];

#define BL(p00, p10, p01, p11, sh, mask)                          \
    ((((((p00)>>(sh))&(mask)) * (256u-fx) * (256u-fy) +           \
       (((p10)>>(sh))&(mask)) * fx        * (256u-fy) +           \
       (((p01)>>(sh))&(mask)) * (256u-fx) * fy        +           \
       (((p11)>>(sh))&(mask)) * fx        * fy) + (1u << 15)) >> 16)

            u32 sr = BL(p00, p10, p01, p11, 16u, 0xFFu);
            u32 sg = BL(p00, p10, p01, p11,  8u, 0xFFu);
            u32 sb = BL(p00, p10, p01, p11,  0u, 0xFFu);
#undef BL

            if (alpha == 255u) {
                fb_base[fb_y * fb_stride_px + fb_x] =
                    (sr << 16) | (sg << 8) | sb;
            } else {
                u32 fp = fb_base[fb_y * fb_stride_px + fb_x];
                u32 fr = (fp >> 16) & 0xFFu;
                u32 fg = (fp >>  8) & 0xFFu;
                u32 fbl = fp & 0xFFu;
                fb_base[fb_y * fb_stride_px + fb_x] =
                    (((sr * (u32)alpha + fr * inv_a) / 255u) << 16) |
                    (((sg * (u32)alpha + fg * inv_a) / 255u) <<  8) |
                     ((sb * (u32)alpha + fbl* inv_a) / 255u);
            }
        }
    }
    return 0;
}

/*
 * v3d_composite_anim — single-pass window animation composite (Phase 6.2).
 *
 * Eliminates the two-step (restore_bg + blit) pattern that causes a visible
 * blank-window flash between frames.  For every pixel in the natural window
 * rect [nat_x, nat_y, nat_w, nat_h]:
 *   - Reads the background from the kernel wallpaper buffer (or 0x1A1A2E).
 *   - If the pixel is inside the scaled rect [anim_x, anim_y, anim_w, anim_h],
 *     bilinear-samples the BO (which holds the nat_w × nat_h window snapshot)
 *     and alpha-blends it over the background.
 *   - Writes the result directly to the framebuffer — one write per pixel.
 *
 * wp_ptr/wp_bmpw/wp_bmph: kernel wallpaper state forwarded from syscall.c.
 */
int v3d_composite_anim(u32 bo_handle,
                        u32 nat_x, u32 nat_y, u32 nat_w, u32 nat_h,
                        u32 anim_x, u32 anim_y, u32 anim_w, u32 anim_h,
                        u8 alpha,
                        uintptr_t wp_ptr, u32 wp_bmpw, u32 wp_bmph)
{
    if (!fb_base || !nat_w || !nat_h) return -1;

    uintptr_t bo_phys = v3d_bo_phys(bo_handle);
    if (!bo_phys) return -1;
    const u32 *bo_mem = (const u32 *)bo_phys;

    /* Clamp destination to framebuffer */
    u32 x0 = nat_x, y0 = nat_y;
    u32 x1 = nat_x + nat_w, y1 = nat_y + nat_h;
    if (x0 >= fb_width || y0 >= fb_height) return 0;
    if (x1 > fb_width)  x1 = fb_width;
    if (y1 > fb_height) y1 = fb_height;

    u32 stride_px = fb_stride / 4u;

    /* Wallpaper centering crop (BMP may be wider/taller than screen) */
    u32 crop_x = (wp_ptr && wp_bmpw > fb_width)  ? (wp_bmpw - fb_width)  / 2u : 0u;
    u32 crop_y = (wp_ptr && wp_bmph > fb_height) ? (wp_bmph - fb_height) / 2u : 0u;

    for (u32 dy = y0; dy < y1; dy++) {
        volatile u32 *fb_row = fb_base + dy * stride_px;

        /* Pre-fetch wallpaper row for this scanline */
        const u32 *wp_row = (void *)0;
        if (wp_ptr) {
            u32 wy = dy + crop_y;
            if (wy < wp_bmph)
                wp_row = (const u32 *)wp_ptr + wy * wp_bmpw;
        }

        for (u32 dx = x0; dx < x1; dx++) {
            /* Background pixel from wallpaper or fallback colour */
            u32 bg;
            if (wp_row) {
                u32 wx = dx + crop_x;
                bg = (wx < wp_bmpw) ? wp_row[wx] : 0x1A1A2Eu;
            } else {
                bg = 0x1A1A2Eu;
            }

            /* Skip BO compositing outside the scaled rect, or when transparent */
            if (alpha == 0u || dx < anim_x || dx >= anim_x + anim_w ||
                dy < anim_y || dy >= anim_y + anim_h) {
                fb_row[dx] = bg;
                continue;
            }

            /* Bilinear sample from BO (nat_w × nat_h source) */
            u32 sx_fxp = (anim_w > 1u) ?
                ((dx - anim_x) * (nat_w - 1u) * 256u) / (anim_w - 1u) : 0u;
            u32 sy_fxp = (anim_h > 1u) ?
                ((dy - anim_y) * (nat_h - 1u) * 256u) / (anim_h - 1u) : 0u;

            u32 sx0 = sx_fxp >> 8; u32 fx = sx_fxp & 0xFFu;
            u32 sy0 = sy_fxp >> 8; u32 fy = sy_fxp & 0xFFu;
            u32 sx1 = (sx0 + 1u < nat_w) ? sx0 + 1u : sx0;
            u32 sy1 = (sy0 + 1u < nat_h) ? sy0 + 1u : sy0;

            u32 p00 = bo_mem[sy0 * nat_w + sx0];
            u32 p10 = bo_mem[sy0 * nat_w + sx1];
            u32 p01 = bo_mem[sy1 * nat_w + sx0];
            u32 p11 = bo_mem[sy1 * nat_w + sx1];

#define BL(p00, p10, p01, p11, sh, mask)                          \
    ((((((p00)>>(sh))&(mask)) * (256u-fx) * (256u-fy) +           \
       (((p10)>>(sh))&(mask)) * fx        * (256u-fy) +           \
       (((p01)>>(sh))&(mask)) * (256u-fx) * fy        +           \
       (((p11)>>(sh))&(mask)) * fx        * fy) + (1u << 15)) >> 16)

            u32 sr = BL(p00, p10, p01, p11, 16u, 0xFFu);
            u32 sg = BL(p00, p10, p01, p11,  8u, 0xFFu);
            u32 sb = BL(p00, p10, p01, p11,  0u, 0xFFu);
#undef BL

            if (alpha == 255u) {
                fb_row[dx] = (sr << 16) | (sg << 8) | sb;
            } else {
                u32 br = (bg >> 16) & 0xFFu;
                u32 bg_ = (bg >>  8) & 0xFFu;
                u32 bb  =  bg        & 0xFFu;
                u32 ia  = 255u - (u32)alpha;
                fb_row[dx] =
                    (((sr * (u32)alpha + br  * ia) / 255u) << 16) |
                    (((sg * (u32)alpha + bg_ * ia) / 255u) <<  8) |
                     ((sb * (u32)alpha + bb  * ia) / 255u);
            }
        }
    }
    return 0;
}

/* ── WM3: double-buffer ───────────────────────────────────────────────── */

void v3d_dbl_init(void)
{
    if (g_back_buf) return;   /* idempotent */
    if (!fb_base || !fb_width || !fb_height) return;

    u32 bytes  = fb_width * fb_height * 4u;
    u32 npages = pages_for(bytes);
    uintptr_t phys = pmm_alloc_pages(npages);
    if (!phys) {
        kwarn("[V3D] dbl_init: PMM exhausted — compositor uses direct FB\n");
        return;
    }
    g_back_buf   = (volatile u32 *)phys;
    g_back_pages = npages;
    kinfo("[V3D] double buffer: %u KB at 0x%lx\n",
          (bytes + 1023u) / 1024u, (unsigned long)phys);
}

void v3d_dbl_flip(void)
{
    if (!g_back_buf || !fb_base) return;
    u32 n = fb_width * fb_height;
    __asm__ volatile("dsb sy" ::: "memory");   /* ensure back-buf writes visible */
    const volatile u32 *s = g_back_buf;
    volatile u32       *d = fb_base;
    for (u32 i = 0; i < n; i++) d[i] = s[i];
    /* The back buffer never contains the cursor sprite, so redraw it on top of
     * the freshly copied front buffer.  cursor_redraw() saves the new background
     * pixels first so the next cursor_move() can restore them cleanly. */
    cursor_redraw();
}

/* ── WM4: Dual Kawase blur ────────────────────────────────────────────── */

#ifdef AETHER_KAWASE_BLUR

void v3d_blur_init(void)
{
    if (g_blur_ping) return;   /* idempotent */
    if (!fb_width || !fb_height) return;

    u32 sw    = (fb_width  + 3u) / 4u;   /* quarter-res width  */
    u32 sh    = (fb_height + 3u) / 4u;   /* quarter-res height */
    u32 bytes = sw * sh * 4u;
    u32 np    = pages_for(bytes);

    uintptr_t pa = pmm_alloc_pages(np);
    uintptr_t pb = pmm_alloc_pages(np);
    if (!pa || !pb) {
        if (pa) { for (u32 i = 0; i < np; i++) pmm_free_page(pa + i * PMM_PAGE_SIZE); }
        if (pb) { for (u32 i = 0; i < np; i++) pmm_free_page(pb + i * PMM_PAGE_SIZE); }
        kwarn("[V3D] blur_init: PMM exhausted — glassmorphism blur disabled\n");
        return;
    }
    g_blur_ping  = (volatile u32 *)pa;
    g_blur_pong  = (volatile u32 *)pb;
    g_blur_pages = np;
    kinfo("[V3D] Kawase blur scratch: %u KB × 2 at 0x%lx / 0x%lx\n",
          (bytes + 1023u) / 1024u, (unsigned long)pa, (unsigned long)pb);
}

/*
 * kawase_pass — one Dual Kawase blur pass over a w×h pixel buffer.
 *
 * For each output pixel (x, y), samples the four diagonal neighbours at
 * distance d in the source: (x±d, y±d).  Coordinates are clamped to the
 * buffer boundary (mirror-less edge extension).  src and dst must not alias.
 *
 * Offset sequence per frame: d = 1, 2, 3, … (one increment per pass).
 * Running 4 passes with d=1–4 on a ÷4 downsampled buffer approximates a
 * ~40-pixel Gaussian on the full-resolution image.
 */
static void kawase_pass(const volatile u32 *src, volatile u32 *dst,
                        u32 w, u32 h, u32 d)
{
    for (u32 y = 0; y < h; y++) {
        u32 y0 = (y >= d)        ? y - d : 0u;
        u32 y1 = (y + d < h)     ? y + d : h - 1u;
        for (u32 x = 0; x < w; x++) {
            u32 x0  = (x >= d)       ? x - d : 0u;
            u32 x1  = (x + d < w)    ? x + d : w - 1u;
            u32 p00 = src[y0 * w + x0];
            u32 p10 = src[y0 * w + x1];
            u32 p01 = src[y1 * w + x0];
            u32 p11 = src[y1 * w + x1];
            u32 r   = ((p00 >> 16 & 0xFFu) + (p10 >> 16 & 0xFFu) +
                       (p01 >> 16 & 0xFFu) + (p11 >> 16 & 0xFFu) + 2u) >> 2;
            u32 g   = ((p00 >>  8 & 0xFFu) + (p10 >>  8 & 0xFFu) +
                       (p01 >>  8 & 0xFFu) + (p11 >>  8 & 0xFFu) + 2u) >> 2;
            u32 b   = ((p00       & 0xFFu) + (p10       & 0xFFu) +
                       (p01       & 0xFFu) + (p11       & 0xFFu) + 2u) >> 2;
            dst[y * w + x] = (r << 16) | (g << 8) | b;
        }
    }
}

/*
 * apply_kawase_blur — capture, blur, and write back a rectangular region.
 *
 * Pipeline:
 *   1. Box-downsample 4× from tgt[dst_rect] → g_blur_ping (sw × sh pixels).
 *   2. N Kawase passes (ping↔pong) with d = 1, 2, …, N.
 *      N = blur_radius/10 + 1, clamped 1–6.
 *   3. Bilinear-upsample the result back to tgt[dst_rect].
 *
 * After return, the background under the layer is replaced with a blurred
 * version.  v3d_composite_layers() then alpha-blends the layer's BO on top,
 * producing the Lumina glassmorphism frosted-glass effect.
 *
 * Graceful degradation: if g_blur_ping/pong were not allocated (PMM
 * exhausted during v3d_blur_init), the function is a no-op and the window
 * composites without blur.
 */
static void apply_kawase_blur(volatile u32 *tgt, u32 tgt_stride_px,
                               u32 dst_x, u32 dst_y, u32 dst_w, u32 dst_h,
                               u8 blur_radius)
{
    if (!g_blur_ping || !g_blur_pong) return;
    if (!dst_w || !dst_h) return;

    /* Small-buffer dimensions (quarter resolution, rounded up) */
    u32 sw = (dst_w + 3u) / 4u;
    u32 sh = (dst_h + 3u) / 4u;

    /* ── 1. Box downsample 4× ─────────────────────────────────────────── */
    for (u32 sy = 0; sy < sh; sy++) {
        for (u32 sx = 0; sx < sw; sx++) {
            u32 r = 0, g = 0, b = 0, cnt = 0;
            for (u32 ky = 0; ky < 4u; ky++) {
                u32 fy = dst_y + sy * 4u + ky;
                if (fy >= dst_y + dst_h) break;
                for (u32 kx = 0; kx < 4u; kx++) {
                    u32 fx = dst_x + sx * 4u + kx;
                    if (fx >= dst_x + dst_w) break;
                    u32 px = tgt[fy * tgt_stride_px + fx];
                    r += (px >> 16) & 0xFFu;
                    g += (px >>  8) & 0xFFu;
                    b +=  px        & 0xFFu;
                    cnt++;
                }
            }
            if (cnt) {
                g_blur_ping[sy * sw + sx] =
                    ((r / cnt) << 16) | ((g / cnt) << 8) | (b / cnt);
            }
        }
    }

    /* ── 2. Kawase passes ─────────────────────────────────────────────── */
    /* n_passes scaled to blur_radius: radius 10→2 passes, 20→3, 30→4, etc. */
    u32 n_passes = (u32)(blur_radius / 10u) + 1u;
    if (n_passes > 6u) n_passes = 6u;
    if (n_passes < 1u) n_passes = 1u;

    volatile u32 *ping = g_blur_ping;
    volatile u32 *pong = g_blur_pong;
    for (u32 p = 0; p < n_passes; p++) {
        kawase_pass(ping, pong, sw, sh, p + 1u);
        /* swap ping↔pong so 'ping' always holds the latest result */
        volatile u32 *tmp = ping; ping = pong; pong = tmp;
    }

    /* ── 3. Bilinear upsample 4× → back to tgt ───────────────────────── */
#define _BU(c00, c10, c01, c11, sh_, mask_)                                 \
    ((((((c00) >> (sh_)) & (mask_)) * (256u - bfx) * (256u - bfy) +        \
       (((c10) >> (sh_)) & (mask_)) * bfx           * (256u - bfy) +        \
       (((c01) >> (sh_)) & (mask_)) * (256u - bfx)  * bfy          +        \
       (((c11) >> (sh_)) & (mask_)) * bfx           * bfy) + (1u << 15)) >> 16)

    for (u32 dy = 0; dy < dst_h; dy++) {
        u32 sy_fxp = (dst_h > 1u && sh > 1u) ?
            (dy * (sh - 1u) * 256u) / (dst_h - 1u) : 0u;
        u32 sy0 = sy_fxp >> 8;
        u32 bfy = sy_fxp & 0xFFu;
        u32 sy1 = (sy0 + 1u < sh) ? sy0 + 1u : sy0;

        for (u32 dx = 0; dx < dst_w; dx++) {
            u32 sx_fxp = (dst_w > 1u && sw > 1u) ?
                (dx * (sw - 1u) * 256u) / (dst_w - 1u) : 0u;
            u32 sx0 = sx_fxp >> 8;
            u32 bfx = sx_fxp & 0xFFu;
            u32 sx1 = (sx0 + 1u < sw) ? sx0 + 1u : sx0;

            u32 q00 = ping[sy0 * sw + sx0];
            u32 q10 = ping[sy0 * sw + sx1];
            u32 q01 = ping[sy1 * sw + sx0];
            u32 q11 = ping[sy1 * sw + sx1];

            u32 ur = _BU(q00, q10, q01, q11, 16u, 0xFFu);
            u32 ug = _BU(q00, q10, q01, q11,  8u, 0xFFu);
            u32 ub = _BU(q00, q10, q01, q11,  0u, 0xFFu);

            tgt[(dst_y + dy) * tgt_stride_px + (dst_x + dx)] =
                (ur << 16) | (ug << 8) | ub;
        }
    }
#undef _BU
}

#else  /* AETHER_KAWASE_BLUR not defined */

void v3d_blur_init(void) {}   /* no-op — scratch buffers not needed */

#endif /* AETHER_KAWASE_BLUR */

/* ── WM3: batch compositor ────────────────────────────────────────────── */

/*
 * sw_fill_background — paint the back (or fb) target with the wallpaper.
 * Center-crop if wallpaper is larger than the screen; fill remaining pixels
 * (or the whole target if no wallpaper) with LUMINA_BG_COLOR.
 */
static void sw_fill_background(volatile u32 *target, u32 stride_px,
                                u32 w, u32 h,
                                uintptr_t wp_ptr, u32 wp_bmpw, u32 wp_bmph)
{
    u32 crop_x = (wp_ptr && wp_bmpw > w) ? (wp_bmpw - w) / 2u : 0u;
    u32 crop_y = (wp_ptr && wp_bmph > h) ? (wp_bmph - h) / 2u : 0u;

    for (u32 y = 0; y < h; y++) {
        const u32 *wp_row = NULL;
        if (wp_ptr) {
            u32 wy = y + crop_y;
            if (wy < wp_bmph)
                wp_row = (const u32 *)wp_ptr + wy * wp_bmpw;
        }
        for (u32 x = 0; x < w; x++) {
            u32 wx = x + crop_x;
            target[y * stride_px + x] =
                (wp_row && wx < wp_bmpw) ? wp_row[wx] : LUMINA_BG_COLOR;
        }
    }
}

int v3d_composite_layers(const v3d_layer_t *layers, int n,
                          uintptr_t wp_ptr, u32 wp_bmpw, u32 wp_bmph)
{
    if (!fb_base || !fb_width || !fb_height) return -1;
    if (n > 0 && !layers) return -1;

    /* Choose render target: back buffer (double-buffered) or direct FB */
    volatile u32 *tgt;
    u32 tgt_stride_px;
    if (g_back_buf) {
        tgt           = g_back_buf;
        tgt_stride_px = fb_width;            /* back buffer is packed */
    } else {
        tgt           = fb_base;
        tgt_stride_px = fb_stride / 4u;
    }

    /* Step 1: fill background (wallpaper or Lumina dark colour) */
    sw_fill_background(tgt, tgt_stride_px, fb_width, fb_height,
                       wp_ptr, wp_bmpw, wp_bmph);

    if (n <= 0) return 0;

/* Step 2: composite each layer — bilinear scale + alpha blend.
 * _CL: fixed-point bilinear sample from four neighbouring pixels.
 * Uses local variables fx/fy (fractional coordinates, 0-255 range). */
#define _CL(p00, p10, p01, p11, sh, mask)                                  \
    ((((((p00) >> (sh)) & (mask)) * (256u - fx) * (256u - fy) +            \
       (((p10) >> (sh)) & (mask)) * fx           * (256u - fy) +            \
       (((p01) >> (sh)) & (mask)) * (256u - fx)  * fy          +            \
       (((p11) >> (sh)) & (mask)) * fx           * fy) + (1u << 15)) >> 16)

    for (int i = 0; i < n; i++) {
        const v3d_layer_t *l = &layers[i];
        if (!l->bo_handle || !l->opacity) continue;
        if (l->dst_w <= 0 || l->dst_h <= 0) continue;
        if (l->src_w <= 0 || l->src_h <= 0) continue;

        uintptr_t src_phys = v3d_bo_phys(l->bo_handle);
        if (!src_phys) continue;

        u32 dst_x = (u32)l->dst_x, dst_y = (u32)l->dst_y;
        u32 dst_w = (u32)l->dst_w, dst_h = (u32)l->dst_h;
        u32 src_w = (u32)l->src_w, src_h = (u32)l->src_h;
        u8  alpha = l->opacity;

        if (dst_x >= fb_width || dst_y >= fb_height) continue;

        /* Clip to target bounds */
        u32 clip_w = dst_w, clip_h = dst_h;
        if (dst_x + clip_w > fb_width)  clip_w = fb_width  - dst_x;
        if (dst_y + clip_h > fb_height) clip_h = fb_height - dst_y;

        /* WM4: blur the background beneath this layer before blending */
#ifdef AETHER_KAWASE_BLUR
        if (l->blur_radius > 0)
            apply_kawase_blur(tgt, tgt_stride_px,
                              dst_x, dst_y, clip_w, clip_h, l->blur_radius);
#endif

        const volatile u32 *src = (const volatile u32 *)src_phys;
        u32 inv_a = 255u - (u32)alpha;

        for (u32 dy = 0; dy < clip_h; dy++) {
            u32 ty = dst_y + dy;
            u32 sy_fxp = (dst_h > 1u) ?
                (dy * (src_h - 1u) * 256u) / (dst_h - 1u) : 0u;
            u32 sy0 = sy_fxp >> 8;
            u32 fy  = sy_fxp & 0xFFu;
            u32 sy1 = (sy0 + 1u < src_h) ? sy0 + 1u : sy0;

            for (u32 dx = 0; dx < clip_w; dx++) {
                u32 tx = dst_x + dx;
                u32 sx_fxp = (dst_w > 1u) ?
                    (dx * (src_w - 1u) * 256u) / (dst_w - 1u) : 0u;
                u32 sx0 = sx_fxp >> 8;
                u32 fx  = sx_fxp & 0xFFu;
                u32 sx1 = (sx0 + 1u < src_w) ? sx0 + 1u : sx0;

                u32 p00 = src[sy0 * src_w + sx0];
                u32 p10 = src[sy0 * src_w + sx1];
                u32 p01 = src[sy1 * src_w + sx0];
                u32 p11 = src[sy1 * src_w + sx1];

                u32 sr = _CL(p00, p10, p01, p11, 16u, 0xFFu);
                u32 sg = _CL(p00, p10, p01, p11,  8u, 0xFFu);
                u32 sb = _CL(p00, p10, p01, p11,  0u, 0xFFu);

                if (alpha == 255u) {
                    tgt[ty * tgt_stride_px + tx] = (sr << 16) | (sg << 8) | sb;
                } else {
                    u32 fp   = tgt[ty * tgt_stride_px + tx];
                    u32 fr   = (fp >> 16) & 0xFFu;
                    u32 fg_  = (fp >>  8) & 0xFFu;
                    u32 fb_  =  fp        & 0xFFu;
                    tgt[ty * tgt_stride_px + tx] =
                        (((sr * (u32)alpha + fr  * inv_a) / 255u) << 16) |
                        (((sg * (u32)alpha + fg_ * inv_a) / 255u) <<  8) |
                         ((sb * (u32)alpha + fb_ * inv_a) / 255u);
                }
            }
        }
    }

#undef _CL
    return 0;
}
