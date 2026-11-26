/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "draw.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#ifdef __FreeBSD__
#include <proto-include.h> /* OpenOrbis keeps these library declarations separately. */
#endif
#include <stdlib.h>
#include <string.h>

static FT_Library ft;
static FT_Face face;
static int font_size;

int font_open(const char *path) {
    if (FT_Init_FreeType(&ft))
        return -1;
    if (FT_New_Face(ft, path, 0, &face)) {
        FT_Done_FreeType(ft);
        return -1;
    }
    return 0;
}

void font_close(void) {
    FT_Done_Face(face);
    FT_Done_FreeType(ft);
}

unsigned utf8_read(const char **text) {
    const unsigned char *p = (const unsigned char *)*text;
    unsigned cp = *p++;
    if (cp >= 0x80) {
        int more = cp >= 0xf0 && cp < 0xf5 ? 3 : cp >= 0xe0 && cp < 0xf0 ? 2 :
                   cp >= 0xc2 && cp < 0xe0 ? 1 : 0;
        cp = more ? cp & (0x7f >> (more + 1)) : 0xfffd;
        for (int i = 0; i < more; i++) {
            if ((*p & 0xc0) != 0x80) {
                cp = 0xfffd;
                break;
            }
            cp = (cp << 6) | (*p++ & 0x3f);
        }
    }
    *text = (const char *)p;
    return cp;
}

size_t utf8_write(unsigned cp, char *out, size_t size) {
    size_t n = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
    if (n > size)
        return 0;
    for (size_t i = n - 1; i > 0; i--) {
        out[i] = (char)(0x80 | (cp & 0x3f));
        cp >>= 6;
    }
    static const unsigned prefix[] = {0, 0, 0xc0, 0xe0, 0xf0};
    out[0] = (char)(prefix[n] | cp);
    return n;
}

static uint32_t blend(uint32_t bg, uint32_t fg, unsigned a) {
    unsigned rb = (((bg & 0xff00ff) * (256 - a) + (fg & 0xff00ff) * a) >> 8) & 0xff00ff;
    unsigned g = (((bg & 0xff00) * (256 - a) + (fg & 0xff00) * a) >> 8) & 0xff00;
    return 0xff000000 | rb | g;
}

static void point(canvas c, int x, int y, uint32_t color, unsigned alpha) {
    if ((unsigned)x < WIDTH && (unsigned)y < HEIGHT) {
        uint32_t *dest = c.pixels + y * WIDTH + x;
        *dest = blend(*dest, color, alpha);
    }
}

void fill(canvas c, int x, int y, int w, int h, uint32_t color) {
    int right = x + w < WIDTH ? x + w : WIDTH, bottom = y + h < HEIGHT ? y + h : HEIGHT;
    x = x < 0 ? 0 : x;
    y = y < 0 ? 0 : y;
    for (; y < bottom; y++)
        for (int i = x; i < right; i++)
            c.pixels[y * WIDTH + i] = 0xff000000 | color;
}

static int corner_inset(int corner_y, int radius) {
    int inset = radius;
    while (inset > 0 && (radius - inset + 1) * (radius - inset + 1) + corner_y * corner_y <= radius * radius)
        inset--;
    return inset;
}

static int row_inset(int row, int height, int radius) {
    int edge = row < height / 2 ? row : height - row - 1;
    return edge < radius ? corner_inset(radius - edge, radius) : 0;
}

void rounded_fill(canvas c, int x, int y, int w, int h, int radius, uint32_t color) {
    for (int row = 0; row < h; row++) {
        int inset = row_inset(row, h, radius);
        fill(c, x + inset, y + row, w - 2 * inset, 1, color);
    }
}

void line(canvas c, int x1, int y1, int x2, int y2, uint32_t color) {
    int dx = abs(x2 - x1), dy = -abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1, error = dx + dy;
    for (;;) {
        point(c, x1, y1, color, 256);
        if (x1 == x2 && y1 == y2)
            break;
        int twice = 2 * error;
        if (twice >= dy) { error += dy; x1 += sx; }
        if (twice <= dx) { error += dx; y1 += sy; }
    }
}

void text(canvas c, int x, int y, int width, int size, uint32_t color, const char *label) {
    if (font_size != size) {
        if (FT_Set_Pixel_Sizes(face, 0, size))
            return;
        font_size = size;
    }
    int right = x + width;
    while (*label) {
        unsigned cp = utf8_read(&label);
        if (cp < 32)
            cp = ' ';
        if (!FT_Get_Char_Index(face, cp))
            cp = '?';
        if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP))
            continue;
        FT_GlyphSlot glyph = face->glyph;
        int advance = (int)(glyph->advance.x >> 6);
        if (x + advance > right || (*label && x + advance + size / 2 > right)) {
            for (int i = 0; i < 3; i++)
                fill(c, x + i * (size / 6), y - 3, size / 9, size / 9, color);
            break;
        }
        FT_Bitmap *b = &glyph->bitmap;
        for (int row = 0; row < (int)b->rows; row++)
            for (int col = 0; col < (int)b->width; col++)
                point(c, x + glyph->bitmap_left + (int)col, y - glyph->bitmap_top + (int)row,
                      color, b->buffer[(int)row * b->pitch + (int)col]);
        x += advance;
    }
}

void background(canvas c, uint32_t accent) {
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int light = 188 - abs(x - 1420) / 11 - abs(y - 215 + (x - 1420) / 8) / 5;
            c.pixels[y * WIDTH + x] = blend(0x080a15, accent, light > 0 ? (unsigned)light : 0);
        }
    }
}

void image(canvas c, int x, int y, int size, int radius, const uint32_t *pixels) {
    if (!pixels)
        return;
    for (int row = 0; row < size; row++) {
        int inset = row_inset(row, size, radius);
        for (int col = inset; col < size - inset; col++) {
            uint32_t src = pixels[(row * ART_SIZE / size) * ART_SIZE + col * ART_SIZE / size];
            point(c, x + col, y + row, src, (src >> 24) + ((src >> 24) == 255));
        }
    }
}
