#!/usr/bin/env python3
"""
scripts/gen_test_images.py — Generate test images for Phase 7.1 img_test.

Writes a 64x64 4-quadrant color PNG and JPEG to assets/images/:
  TL = red (255,0,0)    TR = green (0,255,0)
  BL = blue (0,0,255)   BR = yellow (255,255,0)

Usage: python3 scripts/gen_test_images.py [output_dir]
Called by make_disk.sh before building the FAT32 disk image.
"""
import os, struct, zlib, sys

W, H = 64, 64
OUTPUT_DIR = sys.argv[1] if len(sys.argv) > 1 else \
    os.path.join(os.path.dirname(__file__), '..', 'assets', 'images')

os.makedirs(OUTPUT_DIR, exist_ok=True)

def make_pixels():
    """4-quadrant test pattern."""
    px = []
    for y in range(H):
        for x in range(W):
            if   y < H // 2 and x < W // 2: px.append((255,   0,   0))  # red
            elif y < H // 2:                 px.append((  0, 255,   0))  # green
            elif x < W // 2:                 px.append((  0,   0, 255))  # blue
            else:                            px.append((255, 255,   0))  # yellow
    return px

def write_png(path, pixels):
    def chunk(name, data):
        crc = zlib.crc32(name + data) & 0xffffffff
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', crc)

    raw = b''
    for y in range(H):
        raw += b'\x00'   # filter = None
        for x in range(W):
            r, g, b = pixels[y * W + x]
            raw += bytes([r, g, b])

    sig  = b'\x89PNG\r\n\x1a\n'
    ihdr = chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
    idat = chunk(b'IDAT', zlib.compress(raw, 9))
    iend = chunk(b'IEND', b'')

    with open(path, 'wb') as f:
        f.write(sig + ihdr + idat + iend)
    print(f'[gen] {path}  ({os.path.getsize(path)} B, PNG RGB 64x64)')

def write_jpeg(path, pixels):
    """Write JPEG via Pillow if available, else skip with a warning."""
    try:
        from PIL import Image
        img = Image.new('RGB', (W, H))
        img.putdata(pixels)
        img.save(path, 'JPEG', quality=90)
        print(f'[gen] {path}  ({os.path.getsize(path)} B, JPEG 64x64 q=90)')
    except ImportError:
        print(f'[gen] WARNING: Pillow not installed — skipping {path}')
        print('[gen]   Install with: pip3 install Pillow')
        print('[gen]   JPEG test in img_test will be skipped at runtime.')

pixels = make_pixels()
write_png (os.path.join(OUTPUT_DIR, 'test.png'),  pixels)
write_jpeg(os.path.join(OUTPUT_DIR, 'test.jpg'),  pixels)
