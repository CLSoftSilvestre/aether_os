#ifndef _GPU_H
#define _GPU_H
/*
 * AetherOS — Userspace GPU API (Phase 6.1)
 * File: userspace/lib/include/gpu.h
 *
 * Thin wrappers around SYS_GPU_* syscalls, plus a pure-C software path
 * that is always available regardless of hardware.
 *
 * Typical usage for a frosted-glass panel:
 *
 *   gpu_info_t info;
 *   gpu_init(&info);                       // once at app startup
 *
 *   // Blur the desktop region behind the window (captured into src_bo)
 *   gpu_bo_t src = gpu_alloc(w * h * 4);
 *   gpu_bo_t dst = gpu_alloc(w * h * 4);
 *   void *src_px = gpu_map(src);
 *   // ... copy fb region into src_px ...
 *   gpu_blur_bo(src, dst, w, h, 10);       // radius = 10 px
 *
 *   // Composite tinted frosted glass over the blurred background
 *   void *dst_px = gpu_map(dst);
 *   gpu_tint(dst_px, w * h, C_ACCENT, 40); // 40/255 purple tint
 *   // ... then blit dst_px onto the framebuffer ...
 *
 *   gpu_free(dst);
 *   gpu_free(src);
 */

#include <stdint.h>

/* ── GPU buffer object handle ─────────────────────────────────────────── */
typedef int gpu_bo_t;
#define GPU_BO_INVALID  (-1)

/* ── GPU capability flags ─────────────────────────────────────────────── */
#define GPU_CAP_PRESENT  (1u << 0)   /* V3D hardware active */
#define GPU_CAP_TFU      (1u << 1)   /* TFU (fast blit/filter) present */
#define GPU_CAP_QPU      (1u << 2)   /* QPU compute present */

typedef struct {
    uint32_t ident0;
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t num_qpus;
    uint32_t flags;          /* GPU_CAP_* */
} gpu_info_t;

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

/*
 * gpu_init — query GPU capabilities from the kernel.
 * Safe to call before any other gpu_* function.
 * info may be NULL.  Returns 1 if hardware GPU is present, 0 otherwise.
 */
int gpu_init(gpu_info_t *info);

/* ── Buffer Objects ────────────────────────────────────────────────────── */

/* Allocate a physically-contiguous DMA-accessible buffer.
 * Returns handle ≥ 0 on success, GPU_BO_INVALID on failure. */
gpu_bo_t gpu_alloc(unsigned size_bytes);

/* Free a GPU buffer previously returned by gpu_alloc(). */
void gpu_free(gpu_bo_t bo);

/*
 * Map the GPU buffer into the calling process's virtual address space.
 * Returns the base virtual address (usable like a normal pointer), or NULL.
 *
 * Note: in AetherOS the GPU BO pages live in kernel RAM which is AP=BOTH_RW
 * in the current identity-mapped page tables, so the returned address is
 * just the physical address of the buffer.
 */
void *gpu_map(gpu_bo_t bo);

/* ── 2-D effects ───────────────────────────────────────────────────────── */

/*
 * gpu_blur_bo — blur a GPU buffer region in-place using a separate dst BO.
 *   src / dst: GPU BO handles (each ≥ w*h*4 bytes).
 *   w, h: dimensions in pixels.
 *   radius: blur radius in pixels (1–16); 0 = identity copy.
 * Uses V3D TFU on Pi 4 hardware; falls back to software separable box-blur.
 * Returns 0 on success, -1 on error.
 */
int gpu_blur_bo(gpu_bo_t src, gpu_bo_t dst,
                unsigned w, unsigned h, unsigned radius);

/*
 * gpu_blur — software blur on plain userspace buffers (no GPU BO needed).
 * src / dst must each be at least w*h*4 bytes.
 * Always uses the software path (for small regions or when BOs are
 * not convenient to allocate).
 */
int gpu_blur(const uint32_t *src, uint32_t *dst,
             unsigned w, unsigned h, unsigned radius);

/*
 * gpu_tint — alpha-blend a flat color over every pixel in buf (in-place).
 *   buf:   XRGB8888 pixel array of count pixels.
 *   color: XRGB8888 tint color.
 *   alpha: 0 = no change, 255 = solid color.
 *
 * Formula: out = lerp(buf, color, alpha/255)
 */
void gpu_tint(uint32_t *buf, unsigned count,
              uint32_t color, uint8_t alpha);

/*
 * gpu_alpha_blend — alpha-blend fg over bg pixel-by-pixel.
 *   out, bg, fg: XRGB8888 arrays of count pixels.
 *   alpha: per-layer opacity 0–255 (0 = fully transparent fg).
 */
void gpu_alpha_blend(uint32_t *out,
                     const uint32_t *bg, const uint32_t *fg,
                     unsigned count, uint8_t alpha);

/*
 * gpu_blit — bilinear-scale a GPU BO and alpha-blend it onto the framebuffer
 * (Phase 6.1.7a — window animation compositing).
 *
 *   bo       GPU BO handle containing XRGB8888 pixels at natural (src_w×src_h)
 *   src_w/h  natural dimensions of the BO content
 *   dst_x/y  destination top-left on the framebuffer
 *   dst_w/h  destination size (may differ from src to scale the content)
 *   alpha    0 = no-op, 255 = fully opaque; blends against existing FB pixels
 *
 * Returns 0 on success, -1 on error.
 */
int gpu_blit(gpu_bo_t bo,
             unsigned src_w, unsigned src_h,
             unsigned dst_x, unsigned dst_y,
             unsigned dst_w, unsigned dst_h,
             unsigned char alpha);

/*
 * gpu_glass_panel — composite a frosted-glass rectangle.
 *
 *   bg        source pixels from the desktop / background at (bx, by).
 *   bg_pitch  stride of bg in pixels.
 *   out       destination XRGB8888 buffer (w × h pixels).
 *   w, h      panel dimensions.
 *   tint      XRGB8888 color to tint the blurred glass.
 *   tint_a    tint opacity 0–255 (e.g. 45 for light glassmorphism).
 *   blur_r    blur radius in pixels (8–14 looks good for frosted glass).
 *
 * Steps:
 *   1. Copy w×h pixels from bg into a temporary buffer.
 *   2. Apply Gaussian blur with radius blur_r.
 *   3. Tint the blurred result with (tint, tint_a).
 *   4. Write the result into out.
 *
 * Uses GPU BOs when available; pure-software otherwise.
 * Returns 0 on success.
 */
int gpu_glass_panel(const uint32_t *bg, unsigned bg_pitch,
                    uint32_t *out,
                    unsigned w, unsigned h,
                    uint32_t tint, uint8_t tint_a,
                    unsigned blur_r);

#endif /* _GPU_H */
