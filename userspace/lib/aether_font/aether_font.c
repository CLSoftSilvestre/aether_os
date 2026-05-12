/*
 * aether_font.c — FreeType 2 wrapper for AetherOS (Phase 7.2)
 */

#include "aether_font.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <stdlib.h>
#include <string.h>
#include <sys.h>   /* sys_fs_open / sys_fs_read / sys_fs_close */

/* ── Internal types ─────────────────────────────────────────────────────── */

struct aether_font {
    FT_Face        face;
    int            cur_size;    /* last px_size set on this face */
    unsigned char *font_data;   /* heap-allocated font file; kept alive for FT_Face */
};

/* ── File loader (no fseek needed — sequential reads only) ──────────────── */

static unsigned char *_load_file(const char *path, long *out_size)
{
    long fd = sys_fs_open(path);
    if (fd < 0) return NULL;

    size_t capacity = 128 * 1024;   /* 128 KB starting point */
    size_t size     = 0;
    unsigned char *buf = malloc(capacity);
    if (!buf) { sys_fs_close(fd); return NULL; }

    while (1) {
        if (size == capacity) {
            size_t new_cap = capacity * 2;
            unsigned char *nb = realloc(buf, new_cap);
            if (!nb) { free(buf); sys_fs_close(fd); return NULL; }
            buf      = nb;
            capacity = new_cap;
        }
        long n = sys_fs_read(fd, buf + size, (long)(capacity - size));
        if (n <= 0) break;
        size += (size_t)n;
    }

    sys_fs_close(fd);
    *out_size = (long)size;
    return buf;
}

static FT_Library _ft_lib;
static int        _ft_ready;

/* ── UTF-8 decoder ──────────────────────────────────────────────────────── */

static uint32_t _utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t cp;

    if (*s < 0x80) {
        cp = *s++;
    } else if (*s < 0xE0) {
        cp  = (uint32_t)(*s++ & 0x1F) << 6;
        cp |= (uint32_t)(*s++ & 0x3F);
    } else if (*s < 0xF0) {
        cp  = (uint32_t)(*s++ & 0x0F) << 12;
        cp |= (uint32_t)(*s++ & 0x3F) << 6;
        cp |= (uint32_t)(*s++ & 0x3F);
    } else {
        cp  = (uint32_t)(*s++ & 0x07) << 18;
        cp |= (uint32_t)(*s++ & 0x3F) << 12;
        cp |= (uint32_t)(*s++ & 0x3F) << 6;
        cp |= (uint32_t)(*s++ & 0x3F);
    }
    *p = (const char *)s;
    return cp;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int aether_font_init(void)
{
    if (_ft_ready) return 0;
    FT_Error err = FT_Init_FreeType(&_ft_lib);
    if (err) return -1;
    _ft_ready = 1;
    return 0;
}

int aether_font_load(const char *path, aether_font_t **out)
{
    if (!_ft_ready || !path || !out) return -1;

    long file_size = 0;
    unsigned char *font_data = _load_file(path, &file_size);
    if (!font_data || file_size == 0) {
        free(font_data);
        return -1;
    }

    aether_font_t *f = malloc(sizeof(aether_font_t));
    if (!f) { free(font_data); return -1; }

    FT_Error err = FT_New_Memory_Face(_ft_lib, font_data, (FT_Long)file_size, 0, &f->face);
    if (err) {
        free(font_data);
        free(f);
        return -1;
    }

    f->cur_size  = 0;
    f->font_data = font_data;
    *out = f;
    return 0;
}

void aether_font_free(aether_font_t *font)
{
    if (!font) return;
    FT_Done_Face(font->face);
    free(font->font_data);
    free(font);
}

/* Set pixel size on the face only when it changes. */
static int _set_size(aether_font_t *font, int px_size)
{
    if (font->cur_size == px_size) return 0;
    FT_Error err = FT_Set_Pixel_Sizes(font->face, 0, (FT_UInt)px_size);
    if (err) return -1;
    font->cur_size = px_size;
    return 0;
}

int aether_font_draw(aether_font_t *font,
                     const char *text, int px_size, uint32_t color,
                     uint32_t *buf, int stride, int buf_w, int buf_h,
                     int x, int y)
{
    if (!font || !text || !buf || px_size <= 0) return -1;
    if (_set_size(font, px_size) != 0) return -1;

    FT_Face face = font->face;
    int pen_x = x;

    uint32_t sr = (color >> 16) & 0xFF;
    uint32_t sg = (color >>  8) & 0xFF;
    uint32_t sb =  color        & 0xFF;

    const char *p = text;
    while (*p) {
        uint32_t cp = _utf8_next(&p);
        if (cp == 0) break;

        FT_Error err = FT_Load_Char(face, cp, FT_LOAD_RENDER);
        if (err) continue;

        FT_GlyphSlot slot = face->glyph;
        FT_Bitmap   *bm   = &slot->bitmap;

        int bx = pen_x + slot->bitmap_left;
        int by = y     - slot->bitmap_top;

        for (int row = 0; row < (int)bm->rows; row++) {
            int dst_y = by + row;
            if (dst_y < 0 || dst_y >= buf_h) continue;

            for (int col = 0; col < (int)bm->width; col++) {
                int dst_x = bx + col;
                if (dst_x < 0 || dst_x >= buf_w) continue;

                uint32_t a = bm->buffer[row * bm->pitch + col];
                if (a == 0) continue;

                uint32_t *dst = &buf[dst_y * stride + dst_x];
                uint32_t ia  = 255 - a;

                uint32_t dr = (*dst >> 16) & 0xFF;
                uint32_t dg = (*dst >>  8) & 0xFF;
                uint32_t db =  *dst        & 0xFF;

                uint32_t nr = (sr * a + dr * ia) / 255;
                uint32_t ng = (sg * a + dg * ia) / 255;
                uint32_t nb = (sb * a + db * ia) / 255;

                *dst = (nr << 16) | (ng << 8) | nb;
            }
        }

        pen_x += slot->advance.x >> 6;
    }

    return pen_x - x;
}

int aether_font_measure_width(aether_font_t *font,
                               const char *text, int px_size)
{
    if (!font || !text || px_size <= 0) return -1;
    if (_set_size(font, px_size) != 0) return -1;

    FT_Face face = font->face;
    int width = 0;
    const char *p = text;

    while (*p) {
        uint32_t cp = _utf8_next(&p);
        if (cp == 0) break;
        FT_Error err = FT_Load_Char(face, cp, FT_LOAD_ADVANCE_ONLY);
        if (err) continue;
        width += face->glyph->advance.x >> 6;
    }
    return width;
}

int aether_font_get_ascent(aether_font_t *font, int px_size)
{
    if (!font || px_size <= 0) return px_size;
    if (_set_size(font, px_size) != 0) return px_size;
    return (int)((font->face->size->metrics.ascender + 63) >> 6);
}

int aether_font_get_height(aether_font_t *font, int px_size)
{
    if (!font || px_size <= 0) return px_size + 4;
    if (_set_size(font, px_size) != 0) return px_size + 4;
    int asc  = (int)((font->face->size->metrics.ascender  + 63) >> 6);
    int desc = (int)((-font->face->size->metrics.descender + 63) >> 6);
    return asc + desc;
}
