#!/usr/bin/env python3
"""
AetherGuitar — Photorealistic asset generator (Phase 8.10 visual overhaul)

Generates 32bpp BMP files that look like real guitar hardware:
  pedalboard_bg.bmp  — dark wood + velcro pedalboard surface
  pedal_*.bmp        — 5 effect pedal housings (different colours)
  amp_head_bg.bmp    — Marshall/Vox-style amp head face plate
  knob_base.bmp      — Phong-lit Davies 1900H knob (no indicator)
  stomp_off.bmp      — rubber stomp switch (deactivated)
  stomp_on.bmp       — rubber stomp switch (activated)
  cable_h.bmp        — horizontal instrument cable segment
  jack_left.bmp      — 1/4-inch TS jack plug (left-facing)
  jack_right.bmp     — 1/4-inch TS jack plug (right-facing)

All outputs go to ./assets/ relative to this script.
Run once; commit the output; AetherGuitar loads them at /aeguitar/.
"""

import math, os, struct
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageEnhance

OUT = os.path.join(os.path.dirname(__file__), "assets")
os.makedirs(OUT, exist_ok=True)

# Transparency key that matches GFX_ICON_TRANSPARENT in gfx.h (255,0,255 = magenta)
TRANSPARENT_COL = (255, 0, 255)

# ── helpers ────────────────────────────────────────────────────────────

def save_bmp32(img: Image.Image, name: str):
    """Save as 32bpp BI_RGB BMP (BGRA row-major, top-to-bottom).
    AetherOS gfx_bmp_load expects uncompressed 32bpp or 24bpp BMP.
    We use 24bpp since gfx_bmp_load handles it."""
    path = os.path.join(OUT, name)
    img.convert("RGB").save(path, format="BMP")
    print(f"  {name}  ({img.width}×{img.height})")

def blend(a, b, t):
    """Linear interpolate between two np arrays."""
    return a * (1 - t) + b * t

def radial_gradient(w, h, cx, cy, r, c_inner, c_outer):
    """Return (h,w,3) float32 array with a radial gradient."""
    xs, ys = np.meshgrid(np.arange(w), np.arange(h))
    dist = np.sqrt((xs - cx)**2 + (ys - cy)**2)
    t = np.clip(dist / r, 0, 1)[..., None]
    ci = np.array(c_inner, dtype=np.float32)
    co = np.array(c_outer, dtype=np.float32)
    return blend(ci, co, t)

def noise2d(w, h, scale=1.0, seed=42):
    """Smooth tiling noise [0,1]."""
    rng = np.random.default_rng(seed)
    cell = max(1, int(scale * 8))
    n = rng.random((max(4, h // cell + 2),
                    max(4, w // cell + 2)), dtype=np.float32)
    img = Image.fromarray((n * 255).astype(np.uint8), mode='L')
    img = img.resize((w, h), Image.BILINEAR)
    return np.array(img, dtype=np.float32) / 255.0

def clamp_u8(arr):
    return np.clip(arr, 0, 255).astype(np.uint8)

# ── PEDALBOARD BACKGROUND (804×496) ───────────────────────────────────

def gen_pedalboard_bg():
    W, H = 804, 496
    img = np.zeros((H, W, 3), dtype=np.float32)

    # --- dark walnut wood planks (outer frame) ---
    wood_base = np.array([38, 26, 16], dtype=np.float32)
    wood_mid  = np.array([52, 36, 22], dtype=np.float32)
    xs = np.arange(W)
    plank_h = 24
    for row in range(H):
        # alternating plank brightness
        plank_idx = row // plank_h
        grain_t = (math.sin(plank_idx * 1.37 + 0.3) * 0.5 + 0.5) * 0.3
        img[row, :] = blend(wood_base, wood_mid, grain_t)

    # wood grain (horizontal streaks)
    g = noise2d(W, H, scale=0.6, seed=7)
    img += (g[..., None] - 0.5) * 12

    # subtle horizontal plank lines
    for row in range(0, H, plank_h):
        img[row:row+2, :] = np.array([18, 12, 7], dtype=np.float32)
        img[min(row+plank_h-1, H-1), :] = np.array([55, 40, 26], dtype=np.float32)

    # --- central velcro carpet area ---
    pad_x, pad_y = 28, 40
    vx0, vy0, vx1, vy1 = pad_x, pad_y, W - pad_x, H - pad_y
    VW, VH = vx1 - vx0, vy1 - vy0

    velcro_base = np.array([22, 20, 22], dtype=np.float32)
    # fine grid texture
    xs2, ys2 = np.meshgrid(np.arange(VW), np.arange(VH))
    grid = ((xs2 % 4 < 2) & (ys2 % 4 < 2)).astype(np.float32)
    velcro = velcro_base + grid[..., None] * 5

    # velcro noise
    vn = noise2d(VW, VH, scale=0.2, seed=99)
    velcro += (vn[..., None] - 0.5) * 8

    img[vy0:vy1, vx0:vx1] = velcro

    # inner shadow on velcro edges
    for t in range(12):
        alpha = (12 - t) / 12.0 * 0.4
        img[vy0 + t, vx0:vx1] = img[vy0 + t, vx0:vx1] * (1 - alpha)
        img[vy1 - 1 - t, vx0:vx1] = img[vy1 - 1 - t, vx0:vx1] * (1 - alpha)
        img[vy0:vy1, vx0 + t] = img[vy0:vy1, vx0 + t] * (1 - alpha)
        img[vy0:vy1, vx1 - 1 - t] = img[vy0:vy1, vx1 - 1 - t] * (1 - alpha)

    # metallic side rails (left + right)
    rail_w = 20
    for rx, flip in [(0, False), (W - rail_w, True)]:
        for x in range(rail_w):
            t = x / rail_w
            if flip:
                t = 1 - t
            c = blend(np.array([100, 100, 110], dtype=np.float32),
                      np.array([50, 50, 55], dtype=np.float32), t)
            img[:, rx + x] = c

    # bright specular on rail edge
    img[:, 1:3] = np.array([180, 180, 200], dtype=np.float32)
    img[:, W-3:W-1] = np.array([180, 180, 200], dtype=np.float32)

    # corner rubber bumpers
    for bx, by in [(2, 6), (W-22, 6), (2, H-22), (W-22, H-22)]:
        pil = Image.fromarray(clamp_u8(img))
        d = ImageDraw.Draw(pil)
        d.ellipse([bx, by, bx+16, by+16], fill=(30, 30, 35), outline=(80, 80, 90))
        d.ellipse([bx+4, by+4, bx+12, by+12], fill=(55, 55, 62))
        img = np.array(pil, dtype=np.float32)

    # "AETHER GUITAR" logo watermark on board
    pil = Image.fromarray(clamp_u8(img))
    d = ImageDraw.Draw(pil)
    d.text((W // 2 - 52, H - 28), "AETHERGUITAR", fill=(50, 45, 30))
    img = np.array(pil, dtype=np.float32)

    pil_out = Image.fromarray(clamp_u8(img))
    save_bmp32(pil_out, "pedalboard_bg.bmp")
    return pil_out

# ── SINGLE EFFECT PEDAL (144×248) ─────────────────────────────────────

PEDAL_COLORS = [
    ("blue",   (30, 80, 180),  (60, 140, 255),  "NOISE GATE",    "GATE"),
    ("cobalt", (20, 60, 160),  (40, 100, 220),  "OVERDRIVE",     "DRIVE"),
    ("teal",   (15, 120, 110), (30, 180, 160),  "CHORUS",        "CHO"),
    ("amber",  (160, 90, 10),  (220, 145, 30),  "DELAY",         "DLY"),
    ("violet", (90, 30, 160),  (155, 60, 255),  "PLATE REVERB",  "VERB"),
]

def metal_paint(W, H, base_col, highlight_col):
    """Metallic spray paint effect."""
    arr = np.zeros((H, W, 3), dtype=np.float32)
    bc = np.array(base_col, dtype=np.float32)
    hc = np.array(highlight_col, dtype=np.float32)

    # base coat
    arr[:] = bc

    # directional light from upper-left
    ys, xs = np.meshgrid(np.arange(W), np.arange(H))
    light_t = np.clip((xs / H) * 0.4 + (ys / W) * 0.3, 0, 1)
    arr += (hc - bc) * light_t[..., None] * 0.5

    # specular band across upper third
    spec_y = H * 0.22
    spec_w = H * 0.15
    for y in range(H):
        t = math.exp(-((y - spec_y) ** 2) / (2 * spec_w ** 2))
        arr[y, :] = blend(arr[y, :], np.array([255, 255, 255], dtype=np.float32), t * 0.3)

    # metallic noise (tiny flakes)
    flake = noise2d(W, H, scale=0.15, seed=13)
    arr += (flake[..., None] - 0.5) * 18

    # edge darkening
    for y in range(6):
        t = (6 - y) / 6.0 * 0.5
        arr[y, :] *= (1 - t)
        arr[H-1-y, :] *= (1 - t)
    for x in range(6):
        t = (6 - x) / 6.0 * 0.5
        arr[:, x] *= (1 - t)
        arr[:, W-1-x] *= (1 - t)

    return arr

def gen_pedal(color_name, base_col, hi_col, effect_name, short_name):
    W, H = 144, 248
    arr = np.zeros((H, W, 3), dtype=np.float32)

    # == BODY (full H) — metallic paint ==
    body = metal_paint(W, H, base_col, hi_col)
    arr[:] = body

    # == Brushed aluminium top panel (H: 10..140) ==
    panel_y0, panel_y1 = 10, 140
    PH = panel_y1 - panel_y0
    alu_base = np.array([180, 182, 188], dtype=np.float32)
    alu_dark = np.array([135, 137, 143], dtype=np.float32)
    alu = np.zeros((PH, W, 3), dtype=np.float32)

    # horizontal brushed lines
    for y in range(PH):
        t = (math.sin(y * 0.6) * 0.5 + 0.5) * 0.4
        alu[y, :] = blend(alu_dark, alu_base, t)

    # panel highlight band
    for y in range(PH):
        t = math.exp(-((y - PH * 0.15) ** 2) / (2 * (PH * 0.08) ** 2))
        alu[y, :] = blend(alu[y, :], np.array([235, 237, 245], dtype=np.float32), t * 0.55)

    # subtle noise
    an = noise2d(W, PH, scale=0.3, seed=77)
    alu += (an[..., None] - 0.5) * 8

    arr[panel_y0:panel_y1, :] = alu

    # == Panel border (1px each side) ==
    arr[panel_y0, :] = np.array([220, 222, 230], dtype=np.float32)
    arr[panel_y1-1, :] = np.array([100, 102, 108], dtype=np.float32)
    arr[panel_y0:panel_y1, 0:2] = np.array([200, 202, 208], dtype=np.float32)
    arr[panel_y0:panel_y1, W-2:W] = np.array([110, 112, 118], dtype=np.float32)

    # == Knob cut-outs (3 circles in top half of panel) ==
    knob_cy = panel_y0 + 35
    knob_r  = 20
    knob_xs = [W // 2 - 38, W // 2, W // 2 + 38]
    for kcx in knob_xs:
        # recessed well
        pil_tmp = Image.fromarray(clamp_u8(arr))
        d = ImageDraw.Draw(pil_tmp)
        d.ellipse([kcx - knob_r - 2, knob_cy - knob_r - 2,
                   kcx + knob_r + 2, knob_cy + knob_r + 2],
                  fill=(100, 102, 108), outline=(80, 82, 88))
        arr = np.array(pil_tmp, dtype=np.float32)

    # == Effect name label ==
    pil_lbl = Image.fromarray(clamp_u8(arr))
    d = ImageDraw.Draw(pil_lbl)
    # brand strip
    d.rectangle([6, panel_y0 + 3, W - 6, panel_y0 + 14],
                fill=tuple(int(c * 0.5) for c in base_col))
    # effect name text
    name_x = (W - len(effect_name) * 6) // 2
    d.text((max(4, name_x), panel_y0 + 4), effect_name, fill=(230, 230, 240))
    # control labels below knobs
    labels = ["DRIVE", "TONE", "LEVEL"]
    for i, (kx, lbl) in enumerate(zip(knob_xs, labels)):
        lw = len(lbl) * 6
        d.text((kx - lw // 2, knob_cy + knob_r + 3), lbl, fill=(80, 82, 90))
    arr = np.array(pil_lbl, dtype=np.float32)

    # == Input / Output jack markers on sides ==
    pil_j = Image.fromarray(clamp_u8(arr))
    d = ImageDraw.Draw(pil_j)
    # left side: INPUT
    d.ellipse([0, H - 80, 8, H - 68], fill=(30, 30, 35), outline=(120, 120, 130))
    d.ellipse([2, H - 78, 6, H - 70], fill=(60, 60, 68))
    d.text((4, H - 94), "IN", fill=(180, 180, 200))
    # right side: OUTPUT
    d.ellipse([W-8, H - 80, W, H - 68], fill=(30, 30, 35), outline=(120, 120, 130))
    d.ellipse([W-6, H - 78, W-2, H - 70], fill=(60, 60, 68))
    d.text((W - 16, H - 94), "OUT", fill=(180, 180, 200))
    arr = np.array(pil_j, dtype=np.float32)

    # == Stomp switch area (H: 146..220) ==
    stomp_y0, stomp_y1 = 148, 225
    SH = stomp_y1 - stomp_y0
    SW = W - 20

    pil_s = Image.fromarray(clamp_u8(arr))
    d = ImageDraw.Draw(pil_s)

    # raised metal collar
    d.rounded_rectangle([9, stomp_y0 - 2, W - 9, stomp_y1 + 2], radius=18,
                         fill=(40, 40, 45), outline=(80, 80, 88), width=2)

    # rubber switch body
    rubber_dark  = tuple(int(c * 0.3) for c in base_col)
    rubber_light = tuple(int(min(255, c * 0.5 + 30)) for c in base_col)
    d.rounded_rectangle([12, stomp_y0 + 2, W - 12, stomp_y1 - 2], radius=15,
                         fill=rubber_dark, outline=rubber_light, width=1)

    # embossed circle on stomp
    cx_s, cy_s = W // 2, (stomp_y0 + stomp_y1) // 2
    r_inner = min(SW, SH) // 2 - 10
    d.ellipse([cx_s - r_inner - 1, cy_s - r_inner - 1,
               cx_s + r_inner + 1, cy_s + r_inner + 1],
              outline=(60, 60, 70), width=1)
    d.ellipse([cx_s - r_inner + 2, cy_s - r_inner + 2,
               cx_s + r_inner - 2, cy_s + r_inner - 2],
              fill=tuple(int(c * 0.35 + 10) for c in base_col))

    arr = np.array(pil_s, dtype=np.float32)

    # == LED bezel (above stomp) ==
    pil_led = Image.fromarray(clamp_u8(arr))
    d = ImageDraw.Draw(pil_led)
    led_cx, led_cy = W // 2, 142
    d.ellipse([led_cx - 7, led_cy - 7, led_cx + 7, led_cy + 7],
              fill=(20, 20, 24), outline=(100, 100, 110), width=1)
    d.ellipse([led_cx - 5, led_cy - 5, led_cx + 5, led_cy + 5],
              fill=(10, 35, 10))  # off state: dark green
    arr = np.array(pil_led, dtype=np.float32)

    # == Corner screws ==
    pil_scr = Image.fromarray(clamp_u8(arr))
    d = ImageDraw.Draw(pil_scr)
    for sx, sy in [(8, 8), (W-8, 8), (8, H-8), (W-8, H-8)]:
        d.ellipse([sx-5, sy-5, sx+5, sy+5], fill=(50, 52, 58), outline=(120, 122, 130))
        d.ellipse([sx-3, sy-3, sx+3, sy+3], fill=(80, 82, 88))
        d.line([sx-2, sy, sx+2, sy], fill=(40, 42, 48), width=1)
        d.line([sx, sy-2, sx, sy+2], fill=(40, 42, 48), width=1)

    # == Bottom serial/brand text ==
    d.text((W // 2 - 20, H - 16), f"AOS-{short_name}", fill=(80, 60, 30))

    pil_out = pil_scr
    save_bmp32(pil_out, f"pedal_{color_name}.bmp")
    return pil_out

# ── AMP HEAD PANELS (804×200) — one per amp model ─────────────────────
#
# Knob well X positions (image-relative, match AMP_KNOB_IX in view_amp.c):
#   Fender   (ts=0): [260, 350, 440, 530, 650]  VOL  TREBLE  MID   BASS   MASTER
#   Marshall (ts=1): [260, 345, 430, 515, 670]  PRE  BASS    MID   TREBLE MASTER
#   Vox      (ts=2): [270, 365, 460, 555, 655]  VOL  TREBLE  BASS  CUT    MASTER
#
# knob_y = 100 (centre of 200px image) → 140 after AMP_BLIT_H scaling in C.

def _amp_cabinet_and_chrome(arr, W, H):
    """Black tolex cabinet + chrome bezel. Modifies arr in-place, returns arr."""
    arr[:] = np.array([12, 11, 12], dtype=np.float32)
    xs, ys = np.meshgrid(np.arange(W), np.arange(H))
    bump = (np.sin(xs * 0.3 + 0.5) * np.sin(ys * 0.4) * 0.5 + 0.5).astype(np.float32)
    arr += bump[..., None] * 5
    arr += (noise2d(W, H, scale=0.1, seed=55)[..., None] - 0.5) * 4

    bz_y0, bz_y1 = 20, 180
    BH = bz_y1 - bz_y0
    chrome = np.zeros((BH, W, 3), dtype=np.float32)
    for y in range(BH):
        t = y / BH
        if t < 0.12:
            chrome[y, :] = np.array([200, 205, 215], dtype=np.float32) * (t / 0.12)
        elif t < 0.22:
            chrome[y, :] = blend(np.array([200, 205, 215], dtype=np.float32),
                                  np.array([240, 245, 255], dtype=np.float32), (t-0.12)/0.1)
        elif t < 0.35:
            chrome[y, :] = blend(np.array([240, 245, 255], dtype=np.float32),
                                  np.array([165, 168, 178], dtype=np.float32), (t-0.22)/0.13)
        else:
            chrome[y, :] = blend(np.array([165, 168, 178], dtype=np.float32),
                                  np.array([130, 133, 143], dtype=np.float32), (t-0.35)/0.65)
    arr[bz_y0:bz_y1, :] = chrome
    arr[bz_y0:bz_y1, :] += (noise2d(W, BH, scale=0.8, seed=22)[..., None] - 0.5) * 8
    arr[bz_y0:bz_y0+2, :] = np.array([250, 255, 255], dtype=np.float32)
    arr[bz_y1-2:bz_y1, :] = np.array([90, 92, 100], dtype=np.float32)
    return arr

def _amp_grill(arr, grill_base_col, grill_seed=33):
    """Dark fabric grill section (x:28–200, y:28–172). Modifies arr in-place."""
    grill_x0, grill_x1, grill_y0, grill_y1 = 28, 200, 28, 172
    GW, GH = grill_x1 - grill_x0, grill_y1 - grill_y0
    grill = np.zeros((GH, GW, 3), dtype=np.float32) + np.array(grill_base_col, dtype=np.float32)
    xs_g, ys_g = np.meshgrid(np.arange(GW), np.arange(GH))
    grill += ((xs_g % 3 == 0) | (ys_g % 3 == 0)).astype(np.float32)[..., None] * 10
    grill += (noise2d(GW, GH, scale=0.25, seed=grill_seed)[..., None] - 0.5) * 5
    arr[grill_y0:grill_y1, grill_x0:grill_x1] = grill

    pil = Image.fromarray(clamp_u8(arr))
    d   = ImageDraw.Draw(pil)
    d.rectangle([grill_x0-2, grill_y0-2, grill_x1+2, grill_y1+2],
                outline=(100, 102, 110), width=2)
    return np.array(pil, dtype=np.float32)

def _amp_ctrl_panel(arr, ctrl_top_col, ctrl_bot_col):
    """Chrome sub-panel for the control section (x:210–795, y:30–170)."""
    ctrl_x0, ctrl_x1, ctrl_y0, ctrl_y1 = 210, 795, 30, 170
    sub = np.zeros((ctrl_y1 - ctrl_y0, ctrl_x1 - ctrl_x0, 3), dtype=np.float32)
    for y in range(ctrl_y1 - ctrl_y0):
        t = y / (ctrl_y1 - ctrl_y0)
        sub[y, :] = blend(np.array(ctrl_top_col, dtype=np.float32),
                          np.array(ctrl_bot_col, dtype=np.float32), t)
    arr[ctrl_y0:ctrl_y1, ctrl_x0:ctrl_x1] = sub
    arr[ctrl_y0:ctrl_y1, ctrl_x0:ctrl_x1] += \
        (noise2d(ctrl_x1-ctrl_x0, ctrl_y1-ctrl_y0, scale=0.9, seed=88)[..., None] - 0.5) * 5
    return arr

def _amp_knob_wells(d, knob_cxs, knob_labels, knob_y, label_col, well_fill, well_rim):
    """Draw recessed knob wells + labels onto a PIL ImageDraw."""
    for kcx, klbl in zip(knob_cxs, knob_labels):
        d.ellipse([kcx-23, knob_y-23, kcx+23, knob_y+23], fill=well_rim)
        d.ellipse([kcx-20, knob_y-20, kcx+20, knob_y+20], fill=well_fill)
        lx = kcx - len(klbl) * 3
        d.text((lx, knob_y + 26), klbl, fill=label_col)

def _amp_power_leds(d, x0=762):
    """Power + Standby indicator LEDs at the far right of the control section."""
    d.ellipse([x0, 38, x0+18, 56], fill=(150, 10, 10), outline=(200, 40, 40))
    d.text((x0-10, 60), "POWER", fill=(50, 52, 60))
    d.ellipse([x0, 76, x0+18, 94], fill=(10, 90, 10), outline=(40, 170, 40))
    d.text((x0-14, 98), "STANDBY", fill=(50, 52, 60))

# ── Marshall JCM800 ────────────────────────────────────────────────────

def gen_amp_marshall():
    W, H = 804, 200
    arr = np.zeros((H, W, 3), dtype=np.float32)
    arr = _amp_cabinet_and_chrome(arr, W, H)
    arr = _amp_grill(arr, [8, 8, 9], grill_seed=33)        # very dark grill cloth

    # Control panel: warm brushed chrome (Marshall signature look)
    arr = _amp_ctrl_panel(arr, [215, 218, 224], [168, 171, 180])

    knob_y   = 100
    knob_cxs = [260, 345, 430, 515, 670]
    labels   = ["PREAMP", "BASS", "MIDDLE", "TREBLE", "MASTER"]

    pil = Image.fromarray(clamp_u8(arr))
    d   = ImageDraw.Draw(pil)

    # Gold "Marshall" logo
    d.text((218, 32), "Marshall", fill=(210, 178, 55))
    d.text((222, 50), "JCM 800", fill=(180, 150, 42))
    # Channel label
    d.text((218, 74), "CHANNEL I",  fill=(55, 57, 68))
    d.text((218, 88), "HIGH GAIN",  fill=(55, 57, 68))

    # Knob wells (dark chrome with gold-tinted labels)
    _amp_knob_wells(d, knob_cxs, labels, knob_y,
                    label_col=(42, 38, 22),
                    well_fill=(130, 133, 142), well_rim=(95, 98, 108))

    # Channel-selector toggle between knobs 0 and 1
    toggle_x = (knob_cxs[0] + knob_cxs[1]) // 2
    d.rounded_rectangle([toggle_x-8, knob_y-18, toggle_x+8, knob_y+18],
                        radius=4, fill=(50, 52, 60), outline=(100, 103, 115), width=1)
    d.ellipse([toggle_x-4, knob_y-14, toggle_x+4, knob_y-6], fill=(200, 200, 210))

    _amp_power_leds(d)

    arr = np.array(d._image, dtype=np.float32)
    save_bmp32(Image.fromarray(clamp_u8(arr)), "marshall_amp.bmp")

# ── Fender Deluxe Reverb (Blackface) ──────────────────────────────────

def gen_amp_fender():
    W, H = 804, 200
    arr = np.zeros((H, W, 3), dtype=np.float32)
    arr = _amp_cabinet_and_chrome(arr, W, H)
    # Fender grill cloth is a lighter weave (lighter gray, slight blue tint)
    arr = _amp_grill(arr, [14, 14, 17], grill_seed=51)

    # Blackface silver control panel (brighter, cooler tone)
    arr = _amp_ctrl_panel(arr, [230, 233, 238], [185, 188, 198])

    knob_y   = 100
    knob_cxs = [260, 350, 440, 530, 650]
    labels   = ["VOLUME", "TREBLE", "MIDDLE", "BASS", "MASTER"]

    pil = Image.fromarray(clamp_u8(arr))
    d   = ImageDraw.Draw(pil)

    # Fender "block" logo (white/light on silver)
    d.text((218, 30), "FENDER",        fill=(20, 22, 32))
    d.text((218, 48), "DELUXE REVERB", fill=(40, 42, 55))
    # Channel inputs label
    d.text((218, 72), "NORMAL",  fill=(50, 52, 65))
    d.text((218, 85), "BRIGHT",  fill=(50, 52, 65))

    # Chicken-head style reference lines under/above the knob wells
    for kx in knob_cxs:
        for ang_deg in [-135, -90, -45, 0, 45, 90, 135]:
            ang = math.radians(ang_deg)
            ox = int(kx + math.cos(ang) * 23)
            oy = int(knob_y + math.sin(ang) * 23)
            ix = int(kx + math.cos(ang) * 18)
            iy = int(knob_y + math.sin(ang) * 18)
            d.line([ix, iy, ox, oy], fill=(160, 163, 172), width=1)

    # Knob wells (chrome on silver, dark Fender-blue label text)
    _amp_knob_wells(d, knob_cxs, labels, knob_y,
                    label_col=(18, 22, 55),
                    well_fill=(140, 143, 155), well_rim=(100, 103, 115))

    # Input jacks (two small circles = NORMAL / BRIGHT channels)
    for jy, lbl in [(62, "NORM"), (88, "BRITE")]:
        d.ellipse([730, jy, 748, jy+16], fill=(20, 20, 25), outline=(110, 113, 125))
        d.ellipse([733, jy+3, 745, jy+13], fill=(55, 57, 65))
        d.text((752, jy+2), lbl, fill=(40, 42, 55))

    _amp_power_leds(d, x0=762)

    arr = np.array(d._image, dtype=np.float32)
    save_bmp32(Image.fromarray(clamp_u8(arr)), "fender_amp.bmp")

# ── Vox AC30 ──────────────────────────────────────────────────────────

def gen_amp_vox():
    W, H = 804, 200
    arr = np.zeros((H, W, 3), dtype=np.float32)
    arr = _amp_cabinet_and_chrome(arr, W, H)
    # Vox grill cloth is a lighter warm gray/beige
    arr = _amp_grill(arr, [20, 18, 16], grill_seed=77)

    # Cream/ivory control panel — the AC30's signature look
    ctrl_x0, ctrl_x1, ctrl_y0, ctrl_y1 = 210, 795, 30, 170
    sub = np.zeros((ctrl_y1 - ctrl_y0, ctrl_x1 - ctrl_x0, 3), dtype=np.float32)
    for y in range(ctrl_y1 - ctrl_y0):
        t = y / (ctrl_y1 - ctrl_y0)
        sub[y, :] = blend(np.array([232, 228, 210], dtype=np.float32),
                          np.array([210, 205, 188], dtype=np.float32), t)
    arr[ctrl_y0:ctrl_y1, ctrl_x0:ctrl_x1] = sub
    arr[ctrl_y0:ctrl_y1, ctrl_x0:ctrl_x1] += \
        (noise2d(ctrl_x1-ctrl_x0, ctrl_y1-ctrl_y0, scale=0.9, seed=91)[..., None] - 0.5) * 4

    knob_y   = 100
    knob_cxs = [270, 365, 460, 555, 655]
    labels   = ["VOLUME", "TREBLE", "BASS", "CUT", "MASTER"]

    pil = Image.fromarray(clamp_u8(arr))
    d   = ImageDraw.Draw(pil)

    # Vox diamond / AC30 logo (dark text on cream)
    d.text((218, 30), "VOX",   fill=(15, 12, 10))
    d.text((218, 48), "AC 30", fill=(30, 27, 22))
    d.text((218, 68), "TOP BOOST", fill=(55, 52, 44))

    # Decorative diamond motif around logo
    cx_d, cy_d = 218 + 35, 44
    for r in [14, 11]:
        pts = [(cx_d, cy_d-r), (cx_d+r, cy_d), (cx_d, cy_d+r), (cx_d-r, cy_d)]
        d.polygon(pts, outline=(80, 75, 60) if r == 14 else (130, 125, 105))

    # Normal channel controls indicator
    d.text((218, 90), "NORMAL CH",  fill=(60, 57, 48))
    d.text((218, 103), "TOP BOOST", fill=(60, 57, 48))

    # Knob wells (dark bronze-tinted wells on cream panel)
    _amp_knob_wells(d, knob_cxs, labels, knob_y,
                    label_col=(30, 25, 18),
                    well_fill=(90, 82, 70), well_rim=(60, 55, 44))

    # Vox cut-filter "CUT" label has an extra line to indicate it's a reverse-taper control
    cut_x = knob_cxs[3]
    d.line([cut_x - 28, knob_y + 24, cut_x + 28, knob_y + 24],
           fill=(120, 115, 98), width=1)

    # Power indicator (red Vox jewel)
    d.ellipse([764, 38, 782, 56], fill=(160, 12, 12), outline=(210, 50, 50))
    d.ellipse([768, 42, 778, 52], fill=(220, 40, 40))     # inner jewel
    d.text((751, 60), "POWER",   fill=(35, 30, 22))
    d.ellipse([764, 76, 782, 94], fill=(12, 100, 12), outline=(40, 180, 40))
    d.text((745, 98), "STANDBY", fill=(35, 30, 22))

    arr = np.array(d._image, dtype=np.float32)
    save_bmp32(Image.fromarray(clamp_u8(arr)), "vox_amp.bmp")

# ── KNOB BASE (64×64) — static part, indicator drawn in C ─────────────

def gen_knob_base():
    """High quality Davies 1900H-style knob, indicator pointing up.
    The C code will rotate the indicator image over this base."""
    SZ = 64
    arr = np.zeros((SZ, SZ, 4), dtype=np.float32)   # RGBA

    cx, cy = SZ // 2, SZ // 2
    R = SZ // 2 - 2     # outer radius

    xs, ys = np.meshgrid(np.arange(SZ), np.arange(SZ))
    dist = np.sqrt((xs - cx)**2 + (ys - cy)**2).astype(np.float32)

    # === Outer chrome ring (R-2..R) ===
    for y in range(SZ):
        for x in range(SZ):
            d = dist[y, x]
            if d > R:
                continue  # transparent

            angle = math.atan2(y - cy, x - cx)  # -pi..pi

            # chrome ring (R-3 < d <= R)
            if d > R - 4:
                # chrome with angular variation
                chrome_t = (math.sin(angle * 2 + 0.8) * 0.5 + 0.5)
                cr = int(blend(130, 220, chrome_t))
                cg = int(blend(133, 225, chrome_t))
                cb = int(blend(140, 235, chrome_t))
                arr[y, x] = [cr, cg, cb, 255]
                continue

            # === body (d <= R-4) ===
            body_r = R - 4
            dn = d / body_r  # 0..1

            # base: very dark rubber/plastic
            base = np.array([22, 22, 28], dtype=np.float32)

            # diffuse: light from upper-left
            lx_n = -0.577
            ly_n = -0.577
            lz_n = 0.577
            # normal on spherical body
            if dn < 1.0:
                nz = math.sqrt(max(0, 1 - dn * dn))
                nx = (x - cx) / (body_r)
                ny = (y - cy) / (body_r)
                diffuse = max(0, nx * lx_n + ny * ly_n + nz * lz_n)
            else:
                diffuse = 0
                nz = 0
                nx = (x - cx) / (body_r)
                ny = (y - cy) / (body_r)

            # specular (Phong)
            if dn < 1.0:
                rx_r = 2 * (nx * lx_n + ny * ly_n + nz * lz_n) * nx - lx_n
                ry_r = 2 * (nx * lx_n + ny * ly_n + nz * lz_n) * ny - ly_n
                rz_r = 2 * (nx * lx_n + ny * ly_n + nz * lz_n) * nz - lz_n
                spec = max(0, rz_r) ** 40  # view from above
            else:
                spec = 0

            # compose
            col = base + diffuse * 35 + spec * 220
            col = np.clip(col, 0, 255)

            # edge shadow
            edge_shadow = max(0, (dn - 0.8) / 0.2) * 0.7
            col *= (1 - edge_shadow)

            arr[y, x] = [col[0], col[1], col[2], 255]

    # === Tick marks (arc from 135° to 45° going clockwise) ===
    img = Image.fromarray(clamp_u8(arr), mode='RGBA')
    d = ImageDraw.Draw(img)

    start_deg = 135   # 7 o'clock (lower-left) in Pillow convention
    end_deg   = 45    # 5 o'clock (lower-right)
    # Pillow angles: 0=right, +clockwise. Range 135..360+45 = 135..405
    tick_r_outer = R - 2
    tick_r_inner = R - 8

    for t in range(11):
        # 0=start_deg(135), 10=end_deg(45 via 360+45=405)
        start_rad = math.radians(135)
        end_rad   = math.radians(135 + 270)   # 270 degrees clockwise
        frac = t / 10.0
        angle_rad = start_rad + frac * (end_rad - start_rad)
        ox = int(cx + math.cos(angle_rad) * tick_r_outer)
        oy = int(cy + math.sin(angle_rad) * tick_r_outer)
        ix = int(cx + math.cos(angle_rad) * tick_r_inner)
        iy = int(cy + math.sin(angle_rad) * tick_r_inner)
        col = (200, 200, 210) if (t == 0 or t == 5 or t == 10) else (100, 100, 110)
        d.line([ix, iy, ox, oy], fill=col, width=1)

    arr = np.array(img, dtype=np.float32)
    # Paint outside-circle pixels magenta so gfx_icon_blit skips them (GFX_ICON_TRANSPARENT)
    arr[dist > R, :3] = [255, 0, 255]
    out = Image.fromarray(clamp_u8(arr), mode='RGBA')
    out.convert('RGB').save(os.path.join(OUT, "knob_base.bmp"), format="BMP")
    print(f"  knob_base.bmp  ({SZ}×{SZ})")

    # Also save the transparency mask as a separate image
    mask = Image.fromarray(clamp_u8(arr[:, :, 3:4].repeat(3, axis=2)), mode='RGB')
    mask.save(os.path.join(OUT, "knob_mask.bmp"), format="BMP")
    print(f"  knob_mask.bmp  ({SZ}×{SZ})")

    return out

# ── STOMP SWITCH STATES (100×60) ──────────────────────────────────────

def gen_stomp(state_name, active):
    W, H = 100, 60
    img = Image.new('RGB', (W, H), TRANSPARENT_COL)  # corners become transparent
    d = ImageDraw.Draw(img)

    # Raised collar
    d.rounded_rectangle([2, 2, W-2, H-2], radius=14,
                         fill=(38, 38, 45), outline=(70, 70, 80), width=2)

    # Rubber dome
    rubber_col = (45, 40, 48) if not active else (60, 50, 65)
    d.rounded_rectangle([6, 6, W-6, H-6], radius=12,
                         fill=rubber_col, outline=(85, 80, 95), width=1)

    # Embossed centre ring
    cx, cy = W // 2, H // 2
    d.ellipse([cx-22, cy-22, cx+22, cy+22], outline=(55, 55, 65), width=1)
    d.ellipse([cx-19, cy-19, cx+19, cy+19],
              fill=(50, 45, 55) if not active else (70, 60, 75))

    # Highlight (top-left glow on dome)
    d.ellipse([cx-14, cy-17, cx-2, cy-8],
              fill=(90, 85, 95) if not active else (110, 100, 118))

    save_bmp32(img, f"stomp_{state_name}.bmp")

# ── CABLE (horizontal, 144×24) ────────────────────────────────────────

CABLE_COLORS = [
    ("red",   (200, 20, 20),   (140, 10, 10)),
    ("black", (50, 50, 55),    (30, 30, 35)),
    ("blue",  (20, 80, 200),   (12, 50, 140)),
]

def gen_cable(color_name, col_hi, col_lo):
    W, H = 160, 22
    arr = np.zeros((H, W, 3), dtype=np.float32)

    # cable body gradient (top lit, bottom shadowed)
    for y in range(H):
        t = y / (H - 1)
        if t < 0.3:
            # top highlight
            fac = t / 0.3
            c = blend(np.array([255, 255, 255], dtype=np.float32),
                      np.array(col_hi, dtype=np.float32), fac)
        elif t < 0.6:
            c = np.array(col_hi, dtype=np.float32)
        else:
            fac = (t - 0.6) / 0.4
            c = blend(np.array(col_hi, dtype=np.float32),
                      np.array(col_lo, dtype=np.float32), fac)
        arr[y, :] = c

    # subtle cable braid texture
    xs, ys = np.meshgrid(np.arange(W), np.arange(H))
    braid = np.sin((xs + ys * 2) * 0.8) * 0.5 + 0.5
    arr += (braid[..., None] - 0.5) * 10

    # edge shadows
    for y in [0, 1, H-2, H-1]:
        arr[y, :] *= 0.4 if y in (0, H-1) else 0.7

    img = Image.fromarray(clamp_u8(arr))
    save_bmp32(img, f"cable_{color_name}.bmp")

# ── LED ON/OFF (20×20) ────────────────────────────────────────────────

def gen_led(color_name, on_col, off_col):
    for state, col, glow_col in [
        ("on", on_col, tuple(min(255, c + 80) for c in on_col)),
        ("off", off_col, off_col),
    ]:
        W = H = 20
        img = Image.new('RGB', (W, H), TRANSPARENT_COL)  # corners become transparent
        d = ImageDraw.Draw(img)
        # bezel
        d.ellipse([0, 0, W-1, H-1], fill=(20, 20, 25), outline=(80, 82, 90))
        if state == "on":
            # glow
            d.ellipse([1, 1, W-2, H-2], fill=glow_col)
            d.ellipse([3, 3, W-4, H-4], fill=col)
            # specular
            d.ellipse([5, 4, 10, 8], fill=tuple(min(255, c + 100) for c in col))
        else:
            d.ellipse([2, 2, W-3, H-3], fill=col)
        save_bmp32(img, f"led_{color_name}_{state}.bmp")

# ── TUNER BG (804×496) ────────────────────────────────────────────────

def gen_tuner_bg():
    W, H = 804, 496
    arr = np.zeros((H, W, 3), dtype=np.float32)
    # Deep space-like dark background
    arr[:] = np.array([4, 5, 10], dtype=np.float32)
    # subtle radial glow from center
    cx, cy = W // 2, H // 2
    rg = radial_gradient(W, H, cx, cy, min(W, H) * 0.6,
                          [8, 10, 22], [4, 5, 10])
    arr = rg

    # fine stars/dots
    rng = np.random.default_rng(42)
    stars_y = rng.integers(0, H, 200)
    stars_x = rng.integers(0, W, 200)
    bright  = rng.random(200) * 40 + 20
    for sy, sx, sb in zip(stars_y, stars_x, bright):
        arr[sy, sx] = [sb, sb * 0.9, sb]

    pil_out = Image.fromarray(clamp_u8(arr))
    save_bmp32(pil_out, "tuner_bg.bmp")

# ── MAIN ──────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("Generating AetherGuitar assets...")

    print("\n[1/8] Pedalboard background")
    gen_pedalboard_bg()

    print("\n[2/8] Effect pedals")
    for color_name, base, hi, name, short in PEDAL_COLORS:
        gen_pedal(color_name, base, hi, name, short)

    print("\n[3/8] Amp head face plates (Marshall / Fender / Vox)")
    gen_amp_marshall()
    gen_amp_fender()
    gen_amp_vox()

    print("\n[4/8] Knob base")
    gen_knob_base()

    print("\n[5/8] Stomp switches")
    gen_stomp("off", active=False)
    gen_stomp("on",  active=True)

    print("\n[6/8] Cables")
    for cn, ch, cl in CABLE_COLORS:
        gen_cable(cn, ch, cl)

    print("\n[7/8] LEDs")
    gen_led("green", (40, 220, 40),   (12, 55, 12))
    gen_led("red",   (230, 40, 40),   (60, 12, 12))
    gen_led("amber", (230, 170, 10),  (70, 48, 5))

    print("\n[8/8] Tuner background")
    gen_tuner_bg()

    print(f"\nDone — {len(os.listdir(OUT))} files in {OUT}/")
