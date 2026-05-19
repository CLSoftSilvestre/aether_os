# AetherOS — Compositing Window Manager: Development Plan

> **Branch:** `wmanager`  
> **Priority:** TOP — all other OS development paused until this is complete  
> **Created:** 2026-05-14  
> **Target:** macOS-quality compositing with transparency, Dual Kawase blur, smooth 60fps interactions  
> **Hardware:** Raspberry Pi 5 (BCM2712, VideoCore V GPU) + QEMU `-M virt`

---

## Why We Are Replacing the Current WM

The existing `kernel/core/wm.c` is a kernel-side window registry with static slots, a per-PID key event ring, and no concept of compositing. Apps render directly into the framebuffer at their registered coordinates. This means:

- Transparency is impossible — any window drawn over another erases it.
- Blur requires reading back what is behind a window, which no app can see.
- Screen redraw is uncoordinated — apps repaint independently, producing tearing.
- Moving a window forces a full-region repaint routed through PID 1 (init).

The only viable architecture for glassmorphism and smooth interactions is a **compositing model**: apps render into private off-screen buffers; a dedicated compositor process owns the final framebuffer and assembles the scene each frame.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────┐
│  Application (e.g. aether_term, files, calculator)  │
│  Renders into its own SHM texture buffer             │
│  Posts WM_EV_DAMAGE(rect) to compositor IPC channel  │
└────────────────────┬────────────────────────────────┘
                     │ shared memory (zero-copy)
┌────────────────────▼────────────────────────────────┐
│             Compositor (userspace daemon)            │
│  Owns display framebuffer                           │
│  Sorts WindowLayers by z-index                      │
│  Applies Dual Kawase blur via VideoCore V shaders   │
│  Alpha-blends layers → output FBO → page flip       │
└────────────────────┬────────────────────────────────┘
                     │ syscalls (SHM, WM registry, IPC)
┌────────────────────▼────────────────────────────────┐
│              Kernel (thin WM layer)                 │
│  wm.c: registry + z-index + damage events only     │
│  mm: shared memory regions (dma-buf style)          │
│  display driver: VSync / page-flip                  │
└─────────────────────────────────────────────────────┘
```

---

## Core Data Structures

These replace the current `wm_window_t` and `CompositorState` lives in the compositor process:

```c
/* kernel/include/aether/wm.h  — extended WindowLayer */
typedef struct {
    uint32_t  id;
    uint32_t  pid;
    int32_t   z_index;          /* higher = closer to user */
    int       x, y, w, h;
    float     opacity;          /* 0.0 fully transparent … 1.0 opaque */
    float     blur_radius;      /* 0 = no blur; typical frosted = 20.0 */
    uint32_t  shm_handle;       /* kernel SHM id for content buffer */
    bool      damaged;          /* needs re-composite this frame */
    bool      visible;
    char      title[32];
    int       active;
} wm_window_t;

/* compositor/compositor.h  — compositor-side scene */
typedef struct {
    wm_window_t **layers;       /* sorted ascending by z_index */
    uint32_t      layer_count;
    framebuffer_t *output;      /* final screen buffer (page-flip target) */
    region_t      damage;       /* union of all damaged rects this frame */
    texture_t    *blur_scratch; /* GPU scratch buffer for Kawase passes */
} CompositorState;
```

---

## Implementation Phases

---

### Phase WM1 — Kernel Infrastructure (Shared Memory + Extended WM Registry)

**Goal:** The kernel can allocate shared-memory regions that both an app and the compositor map into their address spaces. The WM registry gains z-index, opacity, blur_radius, and SHM handle.

**Files to create / modify:**

| File | Change |
|------|--------|
| `kernel/mm/shm.c` (new) | Allocate/free named SHM regions; reference-counted pages |
| `kernel/mm/shm.h` (new) | `shm_create`, `shm_map`, `shm_unmap`, `shm_handle_t` |
| `kernel/include/aether/wm.h` | Add `z_index`, `opacity`, `blur_radius`, `shm_handle`, `damaged`, `visible` to `wm_window_t`; new event types |
| `kernel/core/wm.c` | Extend `wm_register` to accept new fields; add `wm_set_zindex`, `wm_damage`, `wm_set_opacity` |
| `kernel/core/syscall.c` | Add `SYS_SHM_CREATE`, `SYS_SHM_MAP`, `SYS_SHM_UNMAP`, `SYS_WM_DAMAGE`, `SYS_WM_SET_ZINDEX`, `SYS_WM_SET_OPACITY` |

**New WM events:**

```c
#define WM_EV_DAMAGE         0xF0u  /* app signals dirty rect to compositor */
#define WM_EV_COMPOSITOR_REQ 0xF1u  /* compositor asks app to re-render */
#define WM_EV_FOCUS_GAINED   0xF2u
#define WM_EV_FOCUS_LOST     0xF3u
```

**Shared memory design:**

- `shm_create(size) → handle`: kernel allocates physically contiguous pages, returns an opaque handle.
- `shm_map(handle, vaddr_hint) → vaddr`: maps pages into calling process's address space.
- Both the app and compositor call `shm_map` with the same handle → they share the buffer, zero-copy.
- On QEMU, implement as simple page-range sharing. On Pi 5 target, align to VideoCore DMA constraints (64-byte cache lines, 4KB pages).

**Acceptance criteria:**
- Two processes can write/read the same SHM region.
- `wm_register` stores and returns a SHM handle.
- All new syscalls reachable from userspace.

---

### Phase WM2 — CPU Compositor (No GPU Yet)

**Goal:** A userspace compositor daemon replaces PID 1's window-painting loop. It composites on the CPU using painter's algorithm with alpha blending. No blur. Proves the architecture before adding GPU complexity.

**New directory:** `userspace/apps/compositor/`

**Files:**

| File | Purpose |
|------|---------|
| `compositor.c` | Main loop, event pump, frame scheduler |
| `scene.c` | Layer list management, z-sort, damage union |
| `blend.c` | CPU alpha blending: `dst = src*alpha + dst*(1-alpha)` |
| `damage.c` | Dirty-rect tracking and merging |
| `ipc.c` | Receives `WM_EV_DAMAGE` from apps; sends `WM_EV_COMPOSITOR_REQ` |

**Frame loop (60fps, 16.67ms budget):**

```
1. poll_damage_events()       — drain WM event ring, collect dirty rects
2. union_damage_rects()       — merge overlapping dirty regions
3. sort_layers_by_zindex()    — stable sort (only when z order changes)
4. for each damaged region:
     for each layer back→front (painter's):
         if layer.intersects(region):
             if layer.opacity == 1.0 and no blur:
                 blit_opaque(layer, region)
             else:
                 alpha_blend(layer, region, layer.opacity)
5. flush_to_framebuffer(damaged_region)
6. vsync_wait()               — block on display IRQ or timer
```

**Damage tracking is critical.** A single pixel repaint on a terminal cursor must not force a full 1080p composite. Use `region_t` as a simple list of non-overlapping rectangles, merging with a union operation.

**Acceptance criteria:**
- Multiple overlapping windows displayed correctly.
- Dragging a window leaves no ghost (vacated region repainted from layers below).
- CPU usage ≤ 30% at idle (only damaged regions processed).
- No tearing (frame submitted after vsync).

---

### Phase WM3 — GPU Compositing via VideoCore V

**Goal:** Move compositing to the VideoCore V GPU (`kernel/drivers/gpu/v3d.c` + mailbox). Each window's SHM buffer is uploaded as a GPU texture. Alpha blending runs in a fragment shader. CPU compositing code stays as fallback for QEMU.

**New files:**

| File | Purpose |
|------|---------|
| `compositor/gpu_compositor.c` | GPU path: upload textures, bind FBOs, draw quads, read back |
| `compositor/shaders/composite.vert` | Full-screen quad vertex shader |
| `compositor/shaders/composite.frag` | Alpha blend: `fragColor = tex * opacity + bg * (1-opacity)` |
| `kernel/drivers/video/display.c` (extend) | Add page-flip / double-buffer support for zero-tear scanout |

**Texture pipeline:**

1. App renders into SHM buffer (CPU or any method).
2. Compositor maps SHM buffer as a VideoCore texture (DMA address, no copy on Pi 5).
3. Each frame: bind output FBO → draw textured quads back-to-front → page flip.

**On QEMU:** VideoCore V is not emulated. The GPU path is #ifdef guarded (`AETHER_GPU_COMPOSITOR`). QEMU uses the CPU path from Phase WM2.

**On Pi 5:** The BCM2712's VideoCore VII (actual GPU in Pi 5, compatible with V3D API) is accessed via the mailbox and MMIO already wired in `v3d.c`. Extend it to support:
- `v3d_texture_from_dma(phys_addr, w, h, format)` — wraps a SHM buffer as a texture.
- `v3d_fbo_create(w, h)` / `v3d_fbo_destroy`.
- `v3d_draw_quad(texture, dst_rect, opacity)`.

**Acceptance criteria:**
- On Pi 5: GPU composite at 1080p60, CPU usage < 5% at idle.
- On QEMU: CPU path works correctly; GPU path compiles but is not entered.
- Page flip eliminates tearing on Pi 5.

---

### Phase WM4 — Dual Kawase Blur (Glassmorphism Core)

**Goal:** Any window with `blur_radius > 0` renders with a frosted-glass effect: the layers *behind* it are blurred and tinted, then the window's own content is alpha-composited on top. This is the visual centerpiece of the Lumina design language.

**Why Dual Kawase:** Gaussian blur costs O(n²) per pixel. Kawase is a multi-pass filter that achieves similar visual quality at O(1) per pixel per pass (4 samples per tap, 4-6 passes). Dual Kawase (downsample + upsample) produces excellent quality for large radii (16–30px) at minimal GPU cost.

**Blur pipeline per blurred window:**

```
1. Capture background:
     Sample all layers *below* this window in the damaged rect → temp FBO A

2. Downsample (÷4 or ÷8):
     Blit FBO A → FBO B (quarter resolution)

3. Kawase blur passes (4 passes recommended):
     Pass 1 (offset=0.5): FBO B → FBO C
     Pass 2 (offset=1.5): FBO C → FBO B
     Pass 3 (offset=2.5): FBO B → FBO C
     Pass 4 (offset=3.5): FBO C → FBO B

4. Upsample with bilinear:
     Blit FBO B → FBO A (back to original resolution, GPU linear filter)

5. Composite:
     fragColor = blur_bg * tint_color * (1 - window_opacity)
               + window_content * window_opacity
```

**Kawase fragment shader (GLSL-ES 3.0 for VideoCore):**

```glsl
precision mediump float;
uniform sampler2D u_source;
uniform vec2      u_texel;    /* 1.0/width, 1.0/height */
uniform float     u_offset;   /* 0.5, 1.5, 2.5, 3.5 */
in  vec2 v_uv;
out vec4 fragColor;

void main() {
    vec2 o = u_offset * u_texel;
    fragColor = (texture(u_source, v_uv + vec2(-o.x, -o.y)) +
                 texture(u_source, v_uv + vec2( o.x, -o.y)) +
                 texture(u_source, v_uv + vec2(-o.x,  o.y)) +
                 texture(u_source, v_uv + vec2( o.x,  o.y))) * 0.25;
}
```

**Default Lumina glassmorphism parameters:**

| Property | Value |
|----------|-------|
| `opacity` | 0.72 |
| `blur_radius` | 20px |
| Tint | `rgba(255,255,255,0.08)` (light) / `rgba(0,0,0,0.25)` (dark) |
| Window border | 1px, `rgba(255,255,255,0.3)` |
| Corner radius | 12px |
| Drop shadow | 0 8px 32px `rgba(0,0,0,0.4)` |

**Blur cache:** The blurred background changes only when layers *behind* the window are damaged. Cache the blurred FBO per window and invalidate only on relevant damage events. This avoids re-running the blur pipeline every frame for static windows.

**Acceptance criteria:**
- Frosted glass effect visible on aether_term overlapping the desktop wallpaper.
- Blur updates when a window moves behind the frosted window.
- Blur stays cached (no re-run) when only the front window's content changes.
- No visible banding or ring artifacts at default parameters.

---

### Phase WM5 — macOS-Quality Window Interactions

**Goal:** Smooth, physically-plausible window animations and interaction model. This is what makes the OS feel alive.

#### WM5.1 — Smooth Window Drag at 60fps

Current `wm_move` fires a `WM_EV_REDRAW` to the app which then repaints. In the compositing model, dragging never asks the app to repaint. The compositor simply moves the window's layer to the new `(x,y)` and re-composites. The app's texture stays valid.

- Mouse drag: compositor receives mouse-delta events, updates `layer.x / layer.y`, marks entire screen as damaged for that frame.
- No app involvement during drag — instant, always 60fps.

#### WM5.2 — Window Open / Close Animation (Spring Physics)

```c
typedef struct {
    float x, v;  /* position (0…1), velocity */
    float k;     /* spring constant (stiffness) */
    float d;     /* damping */
} SpringScalar;

/* Per-frame update: dt = 1/60 */
void spring_update(SpringScalar *s, float target, float dt) {
    float f = -s->k * (s->x - target) - s->d * s->v;
    s->v += f * dt;
    s->x += s->v * dt;
}
```

- Open: scale springs from 0.85→1.0, opacity from 0→1, duration ~180ms.
- Close: scale springs to 0.85, opacity to 0, window removed from scene after spring settles.
- Parameters: `k=280`, `d=28` (critically damped, no overshoot).

#### WM5.3 — Window Focus & Shadow

- Focused window: drop shadow radius = 32px, offset = (0, 8px), opacity = 0.5.
- Unfocused: shadow radius = 16px, opacity = 0.25, window tint slightly dimmed.
- Shadow rendered as blurred transparent quad *behind* the window layer (z-index = window.z - 0.5).

#### WM5.4 — Expose / Mission Control (Stretch Goal)

- Keyboard shortcut triggers all windows to animate to a tiled grid layout (spring physics).
- Click restores selected window with reverse animation.
- Implement after core compositing is solid.

#### WM5.5 — Window Resize

- Resize handle: 8px border all sides + corners.
- Live resize: compositor stretches the current texture (slightly blurry) while drag in progress; sends `WM_EV_RESIZE(new_w, new_h)` to app at end of gesture (or throttled at 15fps during drag).
- App allocates new SHM buffer, repaints, signals compositor.

**Acceptance criteria for WM5:**
- Window drag feels instantaneous (no frame drops during drag).
- Open/close animation looks physically natural, no pop.
- Focused/unfocused visual distinction immediately obvious.

---

### Phase WM6 — App Migration & Cleanup

**Goal:** All existing apps use the new compositor client API. Old direct-framebuffer rendering paths removed.

**New client library:** `userspace/lib/libaether/compositor_client.c`

```c
/* App-side API — wraps SHM allocation + WM registration */
wm_client_t *wmc_create(const char *title, int w, int h);
void          wmc_begin_frame(wm_client_t *c);   /* lock SHM buffer */
uint32_t     *wmc_pixels(wm_client_t *c);         /* ARGB pixel array */
void          wmc_end_frame(wm_client_t *c);      /* unlock + post damage */
void          wmc_set_opacity(wm_client_t *c, float opacity);
void          wmc_set_blur(wm_client_t *c, float radius);
void          wmc_destroy(wm_client_t *c);
```

**Apps to migrate (in order of complexity):**

| App | Notes |
|-----|-------|
| `widget_demo` | Simplest; good test bed for new API |
| `aether_term` | High repaint rate; validates damage tracking performance |
| `files` | Moderate complexity |
| `calculator` | Simple |
| `statusbar` | Needs z-index pinned to top layer |
| `init` | Remove all window-painting logic; becomes just process launcher |

**Cleanup:**
- Remove `wm_pack_window_closed` and the init-repaint-on-close path.
- Remove `WM_EV_REDRAW` as a concept (compositor handles all redraws).
- Keep `wm_deliver_key`, `wm_key_dequeue` — input routing still kernel-side.

**Acceptance criteria:**
- All apps display correctly through compositor.
- No app renders directly to `/dev/fb0` or the legacy framebuffer.
- QEMU and Pi 5 both boot to a working Lumina desktop.

---

### Phase WM7 — Window Minimize & Resize

**Goal:** Interactive minimize and resize gestures with compositor-driven animation.

#### 7a — Minimize

**Design** (macOS Dock-style genie effect is too complex; use spring scale-to-dock):
- New kernel flag `wm_window_t.minimized` (u8, alongside `visible`).
- New syscall `SYS_WM_MINIMIZE(win_id)` — kernel sets `minimized=1`, `visible=0`, sends `WM_EV_MINIMIZE` to compositor.
- Compositor ANIM_MINIMIZE: spring scale from natural size → dock icon position (≈ 48×48 at dock X), fading opacity 1→0. When done, clears the window BO on screen (wallpaper shows through).
- New syscall `SYS_WM_RESTORE(win_id)` — kernel sets `minimized=0`, `visible=1`, sends `WM_EV_RESTORE`; compositor plays reverse spring.
- Init: minimize button (yellow circle, macOS convention) at `wx+26, wy+8` in the title bar; hit-test same as close button offset.
- Dock: minimized windows appear as thumbnail icons in the right section of the dock (separated from app launchers by a divider). Click → `SYS_WM_RESTORE`.

**New event codes:**
```c
#define WM_EV_MINIMIZE  0xF5u   /* compositor: play minimize animation */
#define WM_EV_RESTORE   0xF6u   /* compositor: play restore animation  */
```

**New syscalls:**
```c
#define SYS_WM_MINIMIZE  45
#define SYS_WM_RESTORE   46
```

**Acceptance criteria:**
- Click yellow button → window animates toward dock, disappears.
- Click minimized icon in dock → window springs back with ANIM_OPEN.
- Minimized windows do not receive input (visible=0).

---

#### 7b — Live Resize

**Design:**
- Resize handle: 8×8 px grip at bottom-right corner of each window title bar (or just the corner of the window frame drawn by init).
- Init detects `hit_resize_handle(wx, wy, ww, wh, mx, my)` on mouse press → enters drag-resize mode.
- During drag: init calls `SYS_WM_RESIZE(win_id, new_w, new_h)` — kernel updates `wm_window_t.w/h`, sends `WM_EV_RESIZE` to the window's PID.
- App receives `WM_EV_RESIZE`: reallocates its GPU BO (if using compositor path) or redraws into the new rect.
- Compositor: during resize, bilinear-stretches the existing BO into the new rect (blurry but seamless); sends full resize event to app at drag-end.

**New event codes:**
```c
#define WM_EV_RESIZE  0xF7u  /* packed: bits[55:32]=new_w, bits[31:0]=new_h */
```

**New syscall:**
```c
#define SYS_WM_RESIZE  47    /* arg0=win_id, arg1=new_w<<32|new_h */
```

**Acceptance criteria:**
- Dragging resize handle smoothly changes window bounds.
- App redraws at new size within 1 frame of drag-end.
- Minimum window size enforced (160×120).

---

### Architectural Note: Dock as a Dedicated Process

**Current state:** The dock is embedded in `init`. This simplifies IPC (shared address space) but couples unrelated concerns and makes the dock hard to update independently.

**macOS precedent:** `Dock.app` is a completely separate process with a dedicated window registered at the maximum z-index. The window server (compositor) handles layering. `launchd` (our `init` equivalent) only manages process lifecycle.

**Recommended AetherOS architecture** (target for a future WM8 phase):

| Component | macOS analog | Role |
|-----------|--------------|------|
| `init` | `launchd` | Process spawner only; no drawing |
| `dock` | `Dock.app` | Separate app; GPU-BO window at z=32767; draws dock, manages app launching |
| `statusbar` | `SystemUIServer` | Separate app; GPU-BO window at z=32766; draws clock, battery, etc. |
| `compositor` | `WindowServer` | Composites all windows; owns framebuffer |

**Migration path (WM8):**
1. Move dock data (`g_dock[]`, springs, icon cache) into a new `userspace/apps/dock/main.c`.
2. Init spawns dock at boot; dock registers a full-width, `DOCK_H`-tall GPU-BO window at the bottom of the screen with a fixed z-index (e.g., `WM_Z_DOCK = 32767`).
3. Init retains window chrome (title bar drawing, click routing) until a future phase merges chrome into the compositor.
4. Remove dock drawing code from `init/main.c`.

This is **not implemented yet** — it requires GPU-BO support in the dock app, which depends on completing WM6's full GPU-BO migration for existing apps first.

---

## File Map (New & Changed)

```
kernel/
  core/
    wm.c                    MODIFY — add z_index, opacity, blur_radius, SHM handle
  include/aether/
    wm.h                    MODIFY — extended wm_window_t, new event codes
    shm.h                   NEW    — shared memory kernel API
  mm/
    shm.c                   NEW    — SHM allocator
  core/
    syscall.c               MODIFY — SYS_SHM_*, SYS_WM_DAMAGE, SYS_WM_SET_ZINDEX
  drivers/gpu/
    v3d.c                   MODIFY — add texture_from_dma, FBO, draw_quad

userspace/
  apps/compositor/
    compositor.c            NEW    — main loop, vsync, scene dispatch
    scene.c                 NEW    — layer list, z-sort, damage union
    blend.c                 NEW    — CPU alpha blend (QEMU fallback)
    gpu_compositor.c        NEW    — VideoCore V GPU path
    damage.c                NEW    — dirty-rect tracking
    ipc.c                   NEW    — WM event pump
    shaders/
      composite.vert        NEW    — quad vertex shader
      composite.frag        NEW    — alpha blend fragment shader
      kawase.frag           NEW    — Dual Kawase blur fragment shader
  lib/libaether/
    compositor_client.c     NEW    — client API (wmc_*)
    compositor_client.h     NEW
```

---

## Performance Targets

| Metric | Target |
|--------|--------|
| Frame rate | 60fps locked (1080p on Pi 5) |
| Frame budget | 16.67ms total |
| Blur pass (4-pass Kawase) | < 2ms GPU time |
| Full composite (8 windows) | < 6ms GPU time |
| CPU usage at idle | < 5% (compositor daemon) |
| Input latency (key → screen) | < 2 frames (33ms) |
| SHM buffer size per window | `w * h * 4` bytes (ARGB32) |

---

## Development Order and Dependencies

```
WM1 (kernel SHM + extended registry)
  └── WM2 (CPU compositor)
        ├── WM3 (GPU compositing)           ← depends on v3d.c GPU work
        │     └── WM4 (Kawase blur)
        └── WM5 (interactions/animations)   ← can start after WM2
              └── WM5.5 (close protocol)
                    └── WM6 (app migration)
                          ├── WM7a (minimize)
                          └── WM7b (resize)
                                └── WM8 (dock as separate process)
```

WM3 and WM5 can be developed in parallel once WM2 is working.
WM7a (minimize) can start once compositor is spawned and close animations work.
WM7b (resize) can start independently of WM7a.

---

## Testing Strategy

**QEMU (every phase):**
- Boot with `make run` — verify no regression in existing apps.
- CPU compositor path must always work on QEMU.
- Use framebuffer screenshots (`/dev/fb0` dump) to verify compositing output.

**Pi 5 (WM3 onward):**
- Deploy image via SD card; test GPU compositor path.
- Measure actual frame times with in-kernel timestamp logging.
- Stress test: 8 overlapping blurred windows, drag all simultaneously.

**Visual regression:**
- Capture `screenshots/` before and after each phase.
- Manual comparison for visual correctness (no headless renderer available).

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| VideoCore V shader support incomplete in v3d.c | WM2 CPU path as fallback; add GPU incrementally |
| SHM physical memory fragmentation at runtime | Allocate window buffers at boot from reserved DMA pool |
| Blur too slow on Pi 5 at 1080p | Downsample to ÷8 before blur; cap blur to front 4 windows only |
| App migration breaks existing functionality | Migrate one app at a time; keep legacy syscalls as stubs until WM6 complete |
| QEMU has no GPU → can't test blur | Build blur test harness that dumps PNG output on host |

---

## Definition of Done

This branch is mergeable to `master` when:

1. Compositor daemon starts at boot and renders the Lumina desktop.
2. All existing apps run through the compositor without regression.
3. Any window can have `opacity < 1.0` with correct transparency.
4. Any window can have `blur_radius > 0` with real-time Dual Kawase blur.
5. Window open/close uses spring animation (no pop).
6. Window drag runs at 60fps with no tearing on Pi 5.
7. QEMU boots correctly on the CPU compositing path.
8. CPU usage < 5% when no windows are being redrawn.
