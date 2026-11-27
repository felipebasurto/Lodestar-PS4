/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "draw.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#ifdef __FreeBSD__
#include <proto-include.h> /* OpenOrbis keeps these library declarations separately. */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Barlow Condensed for the menus; Roboto covers the scripts it has no glyphs for. */
enum { MEDIUM, BOLD, FALLBACK, FACES };

static FT_Library ft;
static FT_Face faces[FACES];
static int sizes[FACES];
static uint32_t *ground, *brand_pixels;

static void paint_ground(uint32_t *pixels);

static uint32_t *load_picture(const char *dir, const char *name, int width, int height) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *file = fopen(path, "rb");
    if (!file)
        return NULL;
    enum { LIMIT = 4 * 1024 * 1024 };
    char *body = malloc(LIMIT);
    size_t size = body ? fread(body, 1, LIMIT, file) : 0;
    fclose(file);
    uint32_t *pixels = size && size < LIMIT ? decode_picture(body, size, width, height) : NULL;
    free(body);
    return pixels;
}

int font_open(const char *dir) {
    static const char *names[FACES] = {"BarlowCondensed-Medium.ttf", "BarlowCondensed-Bold.ttf", "Roboto-Regular.ttf"};
    if (FT_Init_FreeType(&ft))
        return -1;
    for (int i = 0; i < FACES; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        if (FT_New_Face(ft, path, 0, &faces[i])) {
            font_close();
            return -1;
        }
    }
    /* Both are decoration: without them the menus draw on a flat ground with no mark. */
    ground = malloc((size_t)WIDTH * HEIGHT * sizeof(*ground));
    if (ground)
        paint_ground(ground);
    brand_pixels = load_picture(dir, "Brand.png", BRAND_WIDTH, BRAND_HEIGHT);
    return 0;
}

void font_close(void) {
    FT_Done_FreeType(ft); /* frees the faces too */
    memset(faces, 0, sizeof(faces));
    memset(sizes, 0, sizeof(sizes));
    free(ground);
    free(brand_pixels);
    ground = brand_pixels = NULL;
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

void shade(canvas c, int x, int y, int w, int h, uint32_t color, unsigned alpha) {
    for (int row = y; row < y + h; row++)
        for (int col = x; col < x + w; col++)
            point(c, col, row, color, alpha);
}

void frame(canvas c, int x, int y, int w, int h, int weight, uint32_t color) {
    fill(c, x, y, w, weight, color);
    fill(c, x, y + h - weight, w, weight, color);
    fill(c, x, y, weight, h, color);
    fill(c, x + w - weight, y, weight, h, color);
}

void ring(canvas c, int cx, int cy, int radius, int weight, uint32_t color) {
    /* 4x4 samples per pixel, in eighths of a pixel, for a smooth edge. */
    long long inner = 64LL * (radius - weight) * (radius - weight), outer = 64LL * radius * radius;
    for (int y = -radius; y <= radius; y++)
        for (int x = -radius; x <= radius; x++) {
            unsigned hits = 0;
            for (int j = -3; j <= 3; j += 2)
                for (int i = -3; i <= 3; i += 2) {
                    long long dx = 8 * x + i, dy = 8 * y + j, d = dx * dx + dy * dy;
                    hits += d <= outer && d > inner;
                }
            point(c, cx + x, cy + y, color, hits * 16);
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

/* The face that has the glyph, sized; NULL if the size cannot be set. */
static FT_Face glyph_face(int style, int size, unsigned *cp) {
    int which = FT_Get_Char_Index(faces[style], *cp) ? style : FALLBACK;
    if (which == FALLBACK && !FT_Get_Char_Index(faces[FALLBACK], *cp)) {
        *cp = '?';
        which = style;
    }
    if (sizes[which] != size) {
        if (FT_Set_Pixel_Sizes(faces[which], 0, size))
            return NULL;
        sizes[which] = size;
    }
    return faces[which];
}

/* Draws the label (c.pixels may be NULL to only measure) and returns its width. */
static int write_label(canvas c, int x, int y, int width, int size, uint32_t color, const char *label, int style) {
    int start = x, right = x + width;
    while (*label) {
        unsigned cp = utf8_read(&label);
        if (cp < 32)
            cp = ' ';
        FT_Face face = glyph_face(style, size, &cp);
        if (!face || FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP))
            continue;
        FT_GlyphSlot glyph = face->glyph;
        int advance = (int)(glyph->advance.x >> 6);
        if (x + advance > right || (*label && x + advance + size / 2 > right)) {
            for (int i = 0; c.pixels && i < 3; i++)
                fill(c, x + i * (size / 6), y - 3, size / 9, size / 9, color);
            return width;
        }
        FT_Bitmap *b = &glyph->bitmap;
        for (int row = 0; c.pixels && row < (int)b->rows; row++)
            for (int col = 0; col < (int)b->width; col++)
                point(c, x + glyph->bitmap_left + (int)col, y - glyph->bitmap_top + (int)row,
                      color, b->buffer[(int)row * b->pitch + (int)col]);
        x += advance;
    }
    return x - start;
}

void text(canvas c, int x, int y, int width, int size, uint32_t color, const char *label) {
    write_label(c, x, y, width, size, color, label, MEDIUM);
}

void bold(canvas c, int x, int y, int width, int size, uint32_t color, const char *label) {
    write_label(c, x, y, width, size, color, label, BOLD);
}

int text_width(int size, const char *label) {
    return write_label((canvas){0}, 0, 0, WIDTH, size, 0, label, MEDIUM);
}

/* A dark ground, a little lighter towards the top right; dithered so it does not band. */
static void paint_ground(uint32_t *pixels) {
    static const int dither[4] = {0, 2, 3, 1};
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            long long dx = (x - 1536) * 1000LL / 2300, dy = (y + 108) * 1000LL / 970;
            long long light = 360000 - dx * dx - dy * dy;
            int a = light > 0 ? (int)(light * 1024 / 360000) : 0; /* quarters of an alpha step */
            pixels[y * WIDTH + x] = blend(0x0a0b0d, 0x252b34, (unsigned)(a + dither[(x & 1) | ((y & 1) << 1)]) / 4);
        }
    }
}

void background(canvas c) {
    if (ground)
        memcpy(c.pixels, ground, (size_t)WIDTH * HEIGHT * sizeof(*c.pixels));
    else
        fill(c, 0, 0, WIDTH, HEIGHT, 0x0a0b0d);
}

void brand(canvas c, int x, int y) {
    for (int row = 0; brand_pixels && row < BRAND_HEIGHT; row++)
        for (int col = 0; col < BRAND_WIDTH; col++) {
            uint32_t src = brand_pixels[row * BRAND_WIDTH + col];
            point(c, x + col, y + row, src, (src >> 24) + ((src >> 24) == 255));
        }
}

void image(canvas c, int x, int y, int size, const uint32_t *pixels) {
    if (!pixels)
        return;
    for (int row = 0; row < size; row++) {
        for (int col = 0; col < size; col++) {
            uint32_t src = pixels[(row * ART_SIZE / size) * ART_SIZE + col * ART_SIZE / size];
            point(c, x + col, y + row, src, (src >> 24) + ((src >> 24) == 255));
        }
    }
}
