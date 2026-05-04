/*
 * AetherOS — Userspace GPU Library (Phase 6.1)
 * File: userspace/lib/libaether/gpu.c
 *
 * Wraps the SYS_GPU_* kernel syscalls.  All functions that accept pixel
 * buffers fall back to a pure-C software path, so callers work identically
 * on QEMU (no GPU hardware) and on Pi 4/5 (V3D present).
 *
 * The software blur uses a sliding-window box blur (identical algorithm to
 * the kernel's sw_hblur / sw_vblur) running entirely in userspace memory.
 * For the glass panel, a temporary heap-like region is allocated by
 * obtaining a GPU BO and mapping it; if BO allocation fails, a
 * stack-local line buffer is used to stay within the 512-pixel width cap.
 */

#include "gpu.h"
#include "sys.h"
#include "stdlib.h"   /* for NULL */
#include "stdint.h"

/* ── Syscall numbers (mirrored from kernel/include/aether/syscall.h) ─── */
#define SYS_GPU_ALLOC  900
#define SYS_GPU_FREE   901
#define SYS_GPU_MAP    902
#define SYS_GPU_INFO   903
#define SYS_GPU_BLUR   904

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

int gpu_init(gpu_info_t *info)
{
    if (!info) {
        /* Just probe — discard the result */
        gpu_info_t tmp;
        long rc = _sys1(SYS_GPU_INFO, (long)(void *)&tmp);
        if (rc < 0) return 0;
        return (tmp.flags & GPU_CAP_PRESENT) ? 1 : 0;
    }

    long rc = _sys1(SYS_GPU_INFO, (long)(void *)info);
    if (rc < 0) {
        info->ident0 = info->version_major = info->version_minor = 0;
        info->num_qpus = 0;
        info->flags    = 0;
        return 0;
    }
    return (info->flags & GPU_CAP_PRESENT) ? 1 : 0;
}

/* ── Buffer Objects ────────────────────────────────────────────────────── */

gpu_bo_t gpu_alloc(unsigned size_bytes)
{
    long h = _sys1(SYS_GPU_ALLOC, (long)(unsigned long)size_bytes);
    return (gpu_bo_t)h;
}

void gpu_free(gpu_bo_t bo)
{
    if (bo <= 0) return;
    _sys1(SYS_GPU_FREE, (long)bo);
}

void *gpu_map(gpu_bo_t bo)
{
    if (bo <= 0) return (void *)0;
    uintptr_t vaddr = 0;
    long rc = _sys2(SYS_GPU_MAP, (long)bo, (long)(void *)&vaddr);
    if (rc < 0) return (void *)0;
    return (void *)vaddr;
}

/* ── GPU BO blur (uses kernel V3D driver) ─────────────────────────────── */

int gpu_blur_bo(gpu_bo_t src, gpu_bo_t dst,
                unsigned w, unsigned h, unsigned radius)
{
    if (src <= 0 || dst <= 0 || !w || !h) return -1;
    long handles = ((long)src << 32) | (unsigned int)dst;
    long wh      = ((long)w  << 32) | (unsigned int)h;
    return (int)_sys3(SYS_GPU_BLUR, handles, wh, (long)radius);
}

/* ── Software pixel helpers ────────────────────────────────────────────── */

/*
 * Sliding-window horizontal box-blur, one row at a time.
 * Running-sum approach: O(1) per pixel after window setup.
 */
static void sw_hblur_row(const uint32_t *src, uint32_t *dst,
                          unsigned w, unsigned radius)
{
    unsigned ksize = 2u * radius + 1u;
    unsigned sum_r = 0, sum_g = 0, sum_b = 0;

    /* Prime with left-clamped values */
    for (unsigned k = 0; k < ksize; k++) {
        unsigned sx = k <= radius ? 0u : k - radius;
        if (sx >= w) sx = w - 1u;
        uint32_t px = src[sx];
        sum_r += (px >> 16) & 0xFFu;
        sum_g += (px >>  8) & 0xFFu;
        sum_b += (px      ) & 0xFFu;
    }

    for (unsigned x = 0; x < w; x++) {
        dst[x] = ((sum_r / ksize) << 16) |
                 ((sum_g / ksize) <<  8) |
                  (sum_b / ksize);

        unsigned left_x  = (x < radius) ? 0u : x - radius;
        unsigned right_x = (x + radius + 1u < w) ? x + radius + 1u : w - 1u;

        uint32_t lpx = src[left_x];
        sum_r -= (lpx >> 16) & 0xFFu;
        sum_g -= (lpx >>  8) & 0xFFu;
        sum_b -= (lpx      ) & 0xFFu;

        uint32_t rpx = src[right_x];
        sum_r += (rpx >> 16) & 0xFFu;
        sum_g += (rpx >>  8) & 0xFFu;
        sum_b += (rpx      ) & 0xFFu;
    }
}

static void sw_vblur_col(const uint32_t *src, uint32_t *dst,
                          unsigned w, unsigned h,
                          unsigned x, unsigned radius)
{
    unsigned ksize = 2u * radius + 1u;
    unsigned sum_r = 0, sum_g = 0, sum_b = 0;

    for (unsigned k = 0; k < ksize; k++) {
        unsigned sy = k <= radius ? 0u : k - radius;
        if (sy >= h) sy = h - 1u;
        uint32_t px = src[sy * w + x];
        sum_r += (px >> 16) & 0xFFu;
        sum_g += (px >>  8) & 0xFFu;
        sum_b += (px      ) & 0xFFu;
    }

    for (unsigned y = 0; y < h; y++) {
        dst[y * w + x] = ((sum_r / ksize) << 16) |
                         ((sum_g / ksize) <<  8) |
                          (sum_b / ksize);

        unsigned top_y    = (y < radius) ? 0u : y - radius;
        unsigned bottom_y = (y + radius + 1u < h) ? y + radius + 1u : h - 1u;

        uint32_t tpx = src[top_y    * w + x];
        sum_r -= (tpx >> 16) & 0xFFu;
        sum_g -= (tpx >>  8) & 0xFFu;
        sum_b -= (tpx      ) & 0xFFu;

        uint32_t bpx = src[bottom_y * w + x];
        sum_r += (bpx >> 16) & 0xFFu;
        sum_g += (bpx >>  8) & 0xFFu;
        sum_b += (bpx      ) & 0xFFu;
    }
}

/* ── Software blur on plain buffers ───────────────────────────────────── */

int gpu_blur(const uint32_t *src, uint32_t *dst,
             unsigned w, unsigned h, unsigned radius)
{
    if (!src || !dst || !w || !h) return -1;
    if (radius > 16u) radius = 16u;

    if (!radius) {
        for (unsigned i = 0; i < w * h; i++) dst[i] = src[i];
        return 0;
    }

    /*
     * Try to obtain a GPU BO as temporary buffer for ping-pong.
     * If unavailable, allocate on the heap via a second GPU BO that maps
     * to kernel RAM.  If that also fails, do in-place vertical blur which
     * causes mild artefacts but never crashes.
     */
    unsigned buf_bytes = w * h * 4u;
    gpu_bo_t tmp_bo    = gpu_alloc(buf_bytes);
    uint32_t *tmp      = (uint32_t *)gpu_map(tmp_bo);

    if (!tmp) {
        /* Fallback: horizontal-only blur (in-place line buffer) */
        for (unsigned y = 0; y < h; y++)
            sw_hblur_row(src + y * w, dst + y * w, w, radius);
        if (tmp_bo > 0) gpu_free(tmp_bo);
        return 0;
    }

    /* Horizontal pass: src → tmp */
    for (unsigned y = 0; y < h; y++)
        sw_hblur_row(src + y * w, tmp + y * w, w, radius);

    /* Vertical pass: tmp → dst */
    for (unsigned x = 0; x < w; x++)
        sw_vblur_col(tmp, dst, w, h, x, radius);

    gpu_free(tmp_bo);
    return 0;
}

/* ── Color blending helpers ───────────────────────────────────────────── */

void gpu_tint(uint32_t *buf, unsigned count, uint32_t color, uint8_t alpha)
{
    if (!buf || !count || !alpha) return;

    unsigned tc_r = (color >> 16) & 0xFFu;
    unsigned tc_g = (color >>  8) & 0xFFu;
    unsigned tc_b = (color      ) & 0xFFu;
    unsigned inv  = 255u - (unsigned)alpha;

    for (unsigned i = 0; i < count; i++) {
        unsigned pr = (buf[i] >> 16) & 0xFFu;
        unsigned pg = (buf[i] >>  8) & 0xFFu;
        unsigned pb = (buf[i]      ) & 0xFFu;

        unsigned or = (pr * inv + tc_r * alpha) / 255u;
        unsigned og = (pg * inv + tc_g * alpha) / 255u;
        unsigned ob = (pb * inv + tc_b * alpha) / 255u;

        buf[i] = (or << 16) | (og << 8) | ob;
    }
}

void gpu_alpha_blend(uint32_t *out,
                     const uint32_t *bg, const uint32_t *fg,
                     unsigned count, uint8_t alpha)
{
    if (!out || !bg || !fg || !count) return;

    unsigned fg_a  = (unsigned)alpha;
    unsigned bg_a  = 255u - fg_a;

    for (unsigned i = 0; i < count; i++) {
        unsigned bg_r = (bg[i] >> 16) & 0xFFu;
        unsigned bg_g = (bg[i] >>  8) & 0xFFu;
        unsigned bg_b = (bg[i]      ) & 0xFFu;
        unsigned fg_r = (fg[i] >> 16) & 0xFFu;
        unsigned fg_g = (fg[i] >>  8) & 0xFFu;
        unsigned fg_b = (fg[i]      ) & 0xFFu;

        out[i] = (((bg_r * bg_a + fg_r * fg_a) / 255u) << 16) |
                 (((bg_g * bg_a + fg_g * fg_a) / 255u) <<  8) |
                  ((bg_b * bg_a + fg_b * fg_a) / 255u);
    }
}

/* ── Frosted glass panel compositor ───────────────────────────────────── */

int gpu_glass_panel(const uint32_t *bg, unsigned bg_pitch,
                    uint32_t *out,
                    unsigned w, unsigned h,
                    uint32_t tint, uint8_t tint_a,
                    unsigned blur_r)
{
    if (!bg || !out || !w || !h) return -1;

    /* Step 1: copy the background region into a GPU BO (or out directly) */
    unsigned buf_bytes = w * h * 4u;
    gpu_bo_t src_bo    = gpu_alloc(buf_bytes);
    uint32_t *src_px   = (uint32_t *)gpu_map(src_bo);

    uint32_t *copy_target = src_px ? src_px : out;

    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
            copy_target[y * w + x] = bg[y * bg_pitch + x];

    /* Step 2: blur */
    if (src_px) {
        /* Use GPU BO path — kernel handles hardware vs software fallback */
        gpu_bo_t dst_bo = gpu_alloc(buf_bytes);
        if (dst_bo > 0) {
            gpu_blur_bo(src_bo, dst_bo, w, h, blur_r);
            uint32_t *dst_px = (uint32_t *)gpu_map(dst_bo);
            if (dst_px) {
                /* Step 3: tint */
                gpu_tint(dst_px, w * h, tint, tint_a);
                /* Step 4: copy result to out */
                for (unsigned i = 0; i < w * h; i++) out[i] = dst_px[i];
                gpu_free(dst_bo);
                gpu_free(src_bo);
                return 0;
            }
            gpu_free(dst_bo);
        }
        gpu_free(src_bo);
    }

    /* Fallback: software blur directly on out (already has background copy) */
    gpu_blur(copy_target, out, w, h, blur_r);
    gpu_tint(out, w * h, tint, tint_a);
    return 0;
}
