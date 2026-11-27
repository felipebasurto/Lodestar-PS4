/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "draw.h"

#ifndef PANEL_MOCK_FONT
#include <ft2build.h>
#include FT_FREETYPE_H
#ifdef __FreeBSD__
#include <proto-include.h> /* OpenOrbis keeps these library declarations separately. */
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Barlow Condensed for the menus; Roboto covers the scripts it has no glyphs for. */
enum { MEDIUM, BOLD, FALLBACK, FACES };

enum {
    GLYPH_TABLE_SIZE = 2048, /* power of two */
    GLYPH_MAX_LOAD = 1536,
    GLYPH_POOL_BYTES = 2 * 1024 * 1024,
    SCALED_ART_SLOTS = 6,
};

typedef struct {
    uint64_t key;
    int16_t advance, left, top;
    uint16_t w, h;
    uint32_t offset;
} glyph_entry;

typedef struct {
    const uint32_t *src;
    uint32_t art_gen;
    int size;
    int opaque;
    uint64_t last_use;
    uint32_t *pixels;
} scaled_art_slot;

#ifndef PANEL_MOCK_FONT
static FT_Library ft;
static FT_Face faces[FACES];
static int sizes[FACES];
#else
static int mock_open;
static uint8_t mock_scratch[128 * 128];
#endif

static uint32_t *ground, *brand_pixels;
static glyph_entry *glyph_table;
static uint8_t *glyph_pool;
static uint32_t glyph_count, glyph_pool_used;
static scaled_art_slot scaled_art[SCALED_ART_SLOTS];
static uint64_t scaled_art_clock;
static int cache_enabled = 1;
static draw_stats stats;

static void paint_ground(uint32_t *pixels);

static void clip_bounds(canvas c, int *x0, int *y0, int *x1, int *y1) {
    if (c.clip_x1 <= 0 && c.clip_y1 <= 0) {
        *x0 = 0;
        *y0 = 0;
        *x1 = WIDTH;
        *y1 = HEIGHT;
    } else {
        *x0 = c.clip_x0 < 0 ? 0 : c.clip_x0;
        *y0 = c.clip_y0 < 0 ? 0 : c.clip_y0;
        *x1 = c.clip_x1 > WIDTH ? WIDTH : c.clip_x1;
        *y1 = c.clip_y1 > HEIGHT ? HEIGHT : c.clip_y1;
    }
}

void draw_stats_reset(void) { stats = (draw_stats){0}; }
draw_stats draw_stats_get(void) { return stats; }

static void clear_glyph_cache(void) {
    if (glyph_table)
        memset(glyph_table, 0, GLYPH_TABLE_SIZE * sizeof(*glyph_table));
    glyph_count = 0;
    glyph_pool_used = 0;
}

void draw_art_cache_invalidate(void) {
    for (int i = 0; i < SCALED_ART_SLOTS; i++) {
        free(scaled_art[i].pixels);
        scaled_art[i] = (scaled_art_slot){0};
    }
}

void draw_set_cache_enabled(int enabled) {
    cache_enabled = enabled ? 1 : 0;
    if (!cache_enabled) {
        clear_glyph_cache();
        draw_art_cache_invalidate();
    }
}

static uint32_t *load_picture(const char *dir, const char *name, int width, int height) {
    if (!dir || !*dir)
        return NULL;
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
#ifndef PANEL_MOCK_FONT
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
#else
    mock_open = 1;
#endif
    glyph_table = calloc(GLYPH_TABLE_SIZE, sizeof(*glyph_table));
    glyph_pool = malloc(GLYPH_POOL_BYTES);
    glyph_count = 0;
    glyph_pool_used = 0;

    /* Both are decoration: without them the menus draw on a flat ground with no mark. */
    ground = malloc((size_t)WIDTH * HEIGHT * sizeof(*ground));
    if (ground)
        paint_ground(ground);
    brand_pixels = load_picture(dir, "Brand.png", BRAND_WIDTH, BRAND_HEIGHT);
    return 0;
}

void font_close(void) {
#ifndef PANEL_MOCK_FONT
    if (ft) {
        FT_Done_FreeType(ft); /* frees the faces too */
        ft = NULL;
    }
    memset(faces, 0, sizeof(faces));
    memset(sizes, 0, sizeof(sizes));
#else
    mock_open = 0;
#endif
    free(ground);
    free(brand_pixels);
    free(glyph_table);
    free(glyph_pool);
    ground = brand_pixels = NULL;
    glyph_table = NULL;
    glyph_pool = NULL;
    glyph_count = 0;
    glyph_pool_used = 0;
    draw_art_cache_invalidate();
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

static inline uint32_t blend(uint32_t bg, uint32_t fg, unsigned a) {
    unsigned rb = (((bg & 0xff00ff) * (256 - a) + (fg & 0xff00ff) * a) >> 8) & 0xff00ff;
    unsigned g = (((bg & 0xff00) * (256 - a) + (fg & 0xff00) * a) >> 8) & 0xff00;
    return 0xff000000 | rb | g;
}

static inline void point_clipped(canvas c, int x, int y, int cx0, int cy0, int cx1, int cy1, uint32_t color, unsigned alpha) {
    if (alpha && x >= cx0 && x < cx1 && y >= cy0 && y < cy1) {
        uint32_t *dest = c.pixels + (size_t)y * WIDTH + x;
        *dest = alpha >= 256 ? (0xff000000 | color) : blend(*dest, color, alpha);
    }
}

void fill(canvas c, int x, int y, int w, int h, uint32_t color) {
    if (!c.pixels || w <= 0 || h <= 0)
        return;
    int cx0, cy0, cx1, cy1;
    clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int left = x < cx0 ? cx0 : x;
    int top = y < cy0 ? cy0 : y;
    int right = x + w > cx1 ? cx1 : x + w;
    int bottom = y + h > cy1 ? cy1 : y + h;
    uint32_t px = 0xff000000 | color;
    for (int row = top; row < bottom; row++) {
        uint32_t *dst = c.pixels + (size_t)row * WIDTH + left;
        for (int i = 0, n = right - left; i < n; i++)
            dst[i] = px;
    }
}

void shade(canvas c, int x, int y, int w, int h, uint32_t color, unsigned alpha) {
    if (!c.pixels || w <= 0 || h <= 0 || !alpha)
        return;
    int cx0, cy0, cx1, cy1;
    clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int left = x < cx0 ? cx0 : x;
    int top = y < cy0 ? cy0 : y;
    int right = x + w > cx1 ? cx1 : x + w;
    int bottom = y + h > cy1 ? cy1 : y + h;
    if (left >= right || top >= bottom)
        return;
    unsigned inv = 256 - alpha;
    unsigned fg_rb = (color & 0xff00ff) * alpha;
    unsigned fg_g = (color & 0xff00) * alpha;
    for (int row = top; row < bottom; row++) {
        uint32_t *dst = c.pixels + (size_t)row * WIDTH + left;
        for (int i = 0, n = right - left; i < n; i++) {
            uint32_t bg = dst[i];
            unsigned rb = (((bg & 0xff00ff) * inv + fg_rb) >> 8) & 0xff00ff;
            unsigned g = (((bg & 0xff00) * inv + fg_g) >> 8) & 0xff00;
            dst[i] = 0xff000000 | rb | g;
        }
    }
}

void frame(canvas c, int x, int y, int w, int h, int weight, uint32_t color) {
    fill(c, x, y, w, weight, color);
    fill(c, x, y + h - weight, w, weight, color);
    fill(c, x, y, weight, h, color);
    fill(c, x + w - weight, y, weight, h, color);
}

void ring(canvas c, int cx, int cy, int radius, int weight, uint32_t color) {
    if (!c.pixels || radius <= 0)
        return;
    int cx0, cy0, cx1, cy1;
    clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    if (cx + radius < cx0 || cx - radius >= cx1 || cy + radius < cy0 || cy - radius >= cy1)
        return;
    /* 4x4 samples per pixel, in eighths of a pixel, for a smooth edge. */
    long long inner = 64LL * (radius - weight) * (radius - weight), outer = 64LL * radius * radius;
    long long skip_in = radius - weight > 1 ? (long long)(radius - weight - 1) * (radius - weight - 1) : -1;
    long long skip_out = (long long)(radius + 1) * (radius + 1);
    int ymin = cy - radius < cy0 ? cy0 - cy : -radius;
    int ymax = cy + radius >= cy1 ? cy1 - 1 - cy : radius;
    int xmin = cx - radius < cx0 ? cx0 - cx : -radius;
    int xmax = cx + radius >= cx1 ? cx1 - 1 - cx : radius;
    for (int y = ymin; y <= ymax; y++) {
        long long y2 = (long long)y * y;
        uint32_t *row = c.pixels + (size_t)(cy + y) * WIDTH;
        for (int x = xmin; x <= xmax; x++) {
            long long r2 = (long long)x * x + y2;
            if (r2 > skip_out || (skip_in >= 0 && r2 < skip_in))
                continue;
            unsigned hits = 0;
            for (int j = -3; j <= 3; j += 2) {
                long long dy = 8 * y + j;
                long long dy2 = dy * dy;
                for (int i = -3; i <= 3; i += 2) {
                    long long dx = 8 * x + i, d = dx * dx + dy2;
                    hits += d <= outer && d > inner;
                }
            }
            if (hits)
                row[cx + x] = blend(row[cx + x], color, hits * 16);
        }
    }
}

void line(canvas c, int x1, int y1, int x2, int y2, uint32_t color) {
    if (!c.pixels)
        return;
    int cx0, cy0, cx1, cy1;
    clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int dx = abs(x2 - x1), dy = -abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1, error = dx + dy;
    for (;;) {
        point_clipped(c, x1, y1, cx0, cy0, cx1, cy1, color, 256);
        if (x1 == x2 && y1 == y2)
            break;
        int twice = 2 * error;
        if (twice >= dy) { error += dy; x1 += sx; }
        if (twice <= dx) { error += dx; y1 += sy; }
    }
}

typedef struct {
    int advance, left, top, w, h, pitch;
    const uint8_t *bitmap;
} raster_glyph;

#ifndef PANEL_MOCK_FONT
/* The face that has the glyph, sized; NULL if the size cannot be set. */
static FT_Face glyph_face(int style, int size, unsigned *cp) {
    if (!faces[style])
        return NULL;
    int which = FT_Get_Char_Index(faces[style], *cp) ? style : FALLBACK;
    if (which == FALLBACK && (!faces[FALLBACK] || !FT_Get_Char_Index(faces[FALLBACK], *cp))) {
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

static int rasterize_backend(int style, int size, unsigned cp, raster_glyph *out) {
    FT_Face face = glyph_face(style, size, &cp);
    if (!face || FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP))
        return -1;
    stats.glyph_rasterizations++;
    FT_GlyphSlot g = face->glyph;
    out->advance = (int)(g->advance.x >> 6);
    out->left = g->bitmap_left;
    out->top = g->bitmap_top;
    out->w = (int)g->bitmap.width;
    out->h = (int)g->bitmap.rows;
    out->pitch = g->bitmap.pitch;
    out->bitmap = g->bitmap.buffer;
    return 0;
}
#else
static int rasterize_backend(int style, int size, unsigned cp, raster_glyph *out) {
    if (!mock_open || size <= 0)
        return -1;
    stats.glyph_rasterizations++;
    int adv = (size * (style == BOLD ? 52 : 46)) / 100;
    if (cp == ' ')
        adv = size / 4;
    if (adv < 4) adv = 4;
    out->advance = adv;
    if (cp == ' ') {
        out->left = out->top = out->w = out->h = out->pitch = 0;
        out->bitmap = NULL;
        return 0;
    }
    int gw = adv > 2 ? adv - 2 : 2;
    int gh = (size * 72) / 100;
    if (gw > 120) gw = 120;
    if (gh > 120) gh = 120;
    if (gh < 4) gh = 4;
    /* Deterministic non-trivial procedural glyph pattern depending on (cp, style, size). */
    uint32_t seed = (uint32_t)cp * 2654435761u ^ ((uint32_t)style * 0x9e3779b9u) ^ (uint32_t)size;
    for (int r = 0; r < gh; r++) {
        for (int col = 0; col < gw; col++) {
            int border = (r == 0 || r == gh - 1 || col == 0 || col == gw - 1);
            int stroke = style == BOLD && (r == 1 || col == 1);
            uint32_t h = (seed ^ ((uint32_t)(r / 2) * 73856093u) ^ ((uint32_t)(col / 2) * 19349663u));
            uint8_t a = (border || stroke) ? 220 : ((h & 3u) == 0 ? 180 : ((h & 7u) == 1 ? 96 : 0));
            mock_scratch[r * gw + col] = a;
        }
    }
    out->left = 1;
    out->top = gh;
    out->w = gw;
    out->h = gh;
    out->pitch = gw;
    out->bitmap = mock_scratch;
    return 0;
}
#endif

static uint64_t glyph_key(int style, int size, unsigned cp) {
    return ((uint64_t)cp << 16) | ((uint64_t)(style & 0xff) << 8) | (uint64_t)(size & 0xff);
}

static size_t glyph_hash(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (size_t)key & (GLYPH_TABLE_SIZE - 1);
}

static int fetch_glyph(int style, int size, unsigned cp, raster_glyph *out) {
    if (!cache_enabled || !glyph_table || !glyph_pool || size <= 0 || size > 255)
        return rasterize_backend(style, size, cp, out);

    uint64_t key = glyph_key(style, size, cp);
    size_t slot = glyph_hash(key);
    for (size_t probe = 0; probe < GLYPH_TABLE_SIZE; probe++) {
        glyph_entry *e = &glyph_table[(slot + probe) & (GLYPH_TABLE_SIZE - 1)];
        if (e->key == key) {
            stats.glyph_cache_hits++;
            out->advance = e->advance;
            out->left = e->left;
            out->top = e->top;
            out->w = e->w;
            out->h = e->h;
            out->pitch = e->w;
            out->bitmap = e->w && e->h ? glyph_pool + e->offset : NULL;
            return 0;
        }
        if (e->key == 0)
            break;
    }

    if (rasterize_backend(style, size, cp, out))
        return -1;

    size_t bytes = (size_t)out->w * (size_t)out->h;
    if (bytes > GLYPH_POOL_BYTES)
        return 0;
    if (glyph_count >= GLYPH_MAX_LOAD || glyph_pool_used + bytes > GLYPH_POOL_BYTES)
        clear_glyph_cache();

    uint32_t offset = glyph_pool_used;
    if (bytes > 0 && out->bitmap) {
        for (int r = 0; r < out->h; r++)
            memcpy(glyph_pool + offset + (size_t)r * (size_t)out->w,
                   out->bitmap + (size_t)r * (size_t)out->pitch, (size_t)out->w);
        glyph_pool_used += (uint32_t)bytes;
    }

    slot = glyph_hash(key);
    for (size_t probe = 0; probe < GLYPH_TABLE_SIZE; probe++) {
        glyph_entry *e = &glyph_table[(slot + probe) & (GLYPH_TABLE_SIZE - 1)];
        if (e->key == 0) {
            e->key = key;
            e->advance = (int16_t)out->advance;
            e->left = (int16_t)out->left;
            e->top = (int16_t)out->top;
            e->w = (uint16_t)out->w;
            e->h = (uint16_t)out->h;
            e->offset = offset;
            glyph_count++;
            out->pitch = out->w;
            out->bitmap = bytes ? glyph_pool + offset : NULL;
            break;
        }
    }
    return 0;
}

/* Draws the label (c.pixels may be NULL to only measure) and returns its width. */
static int write_label(canvas c, int x, int y, int width, int size, uint32_t color, const char *label, int style) {
    if (!label)
        return 0;
    int cx0 = 0, cy0 = 0, cx1 = WIDTH, cy1 = HEIGHT;
    if (c.pixels) {
        clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
        /* Fast reject against clip rect when drawing, with generous glyph overhang margin. */
        if (x - 16 >= cx1 || x + width + 16 <= cx0 || y - size - 24 >= cy1 || y + size <= cy0)
            return 0;
    }
    int start = x, right = x + width;
    while (*label) {
        unsigned cp = utf8_read(&label);
        if (cp < 32)
            cp = ' ';
        raster_glyph g;
        if (fetch_glyph(style, size, cp, &g))
            continue;
        int advance = g.advance;
        if (x + advance > right || (*label && x + advance + size / 2 > right)) {
            for (int i = 0; c.pixels && i < 3; i++)
                fill(c, x + i * (size / 6), y - 3, size / 9, size / 9, color);
            return width;
        }
        if (c.pixels && g.bitmap && g.w > 0 && g.h > 0) {
            int gx0 = x + g.left;
            int gy0 = y - g.top;
            int r0 = gy0 < cy0 ? cy0 - gy0 : 0;
            int r1 = gy0 + g.h > cy1 ? cy1 - gy0 : g.h;
            int c0 = gx0 < cx0 ? cx0 - gx0 : 0;
            int c1 = gx0 + g.w > cx1 ? cx1 - gx0 : g.w;
            for (int row = r0; row < r1; row++) {
                uint32_t *dst = c.pixels + (size_t)(gy0 + row) * WIDTH + gx0;
                const uint8_t *src = g.bitmap + (size_t)row * (size_t)g.pitch;
                for (int col = c0; col < c1; col++) {
                    unsigned a = src[col];
                    if (a)
                        dst[col] = blend(dst[col], color, a);
                }
            }
        }
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

void background_rect(canvas c, int x, int y, int w, int h) {
    if (!c.pixels || w <= 0 || h <= 0)
        return;
    int cx0, cy0, cx1, cy1;
    clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int left = x < cx0 ? cx0 : x;
    int top = y < cy0 ? cy0 : y;
    int right = x + w > cx1 ? cx1 : x + w;
    int bottom = y + h > cy1 ? cy1 : y + h;
    if (left >= right || top >= bottom)
        return;
    size_t row_bytes = (size_t)(right - left) * sizeof(*c.pixels);
    if (ground) {
        for (int row = top; row < bottom; row++) {
            memcpy(c.pixels + (size_t)row * WIDTH + left,
                   ground + (size_t)row * WIDTH + left, row_bytes);
        }
        stats.bg_bytes_copied += (uint64_t)(bottom - top) * row_bytes;
    } else {
        fill(c, left, top, right - left, bottom - top, 0x0a0b0d);
        stats.bg_bytes_copied += (uint64_t)(bottom - top) * row_bytes;
    }
}

void background(canvas c) {
    int cx0, cy0, cx1, cy1;
    clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    background_rect(c, cx0, cy0, cx1 - cx0, cy1 - cy0);
}

void brand(canvas c, int x, int y) {
    if (!c.pixels || !brand_pixels)
        return;
    int cx0, cy0, cx1, cy1;
    clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int r0 = y < cy0 ? cy0 - y : 0;
    int r1 = y + BRAND_HEIGHT > cy1 ? cy1 - y : BRAND_HEIGHT;
    int c0 = x < cx0 ? cx0 - x : 0;
    int c1 = x + BRAND_WIDTH > cx1 ? cx1 - x : BRAND_WIDTH;
    for (int row = r0; row < r1; row++) {
        uint32_t *dst = c.pixels + (size_t)(y + row) * WIDTH + x;
        const uint32_t *src_row = brand_pixels + (size_t)row * BRAND_WIDTH;
        for (int col = c0; col < c1; col++) {
            uint32_t src = src_row[col];
            unsigned a = (src >> 24) + ((src >> 24) == 255);
            if (a)
                dst[col] = a >= 256 ? (0xff000000 | src) : blend(dst[col], src, a);
        }
    }
}

static void blit_prescaled(canvas c, int x, int y, int size, const uint32_t *pixels, int opaque) {
    int cx0, cy0, cx1, cy1;
    clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int r0 = y < cy0 ? cy0 - y : 0;
    int r1 = y + size > cy1 ? cy1 - y : size;
    int c0 = x < cx0 ? cx0 - x : 0;
    int c1 = x + size > cx1 ? cx1 - x : size;
    if (r0 >= r1 || c0 >= c1)
        return;
    if (opaque) {
        size_t row_bytes = (size_t)(c1 - c0) * sizeof(*pixels);
        for (int row = r0; row < r1; row++) {
            memcpy(c.pixels + (size_t)(y + row) * WIDTH + x + c0,
                   pixels + (size_t)row * (size_t)size + c0, row_bytes);
        }
        return;
    }
    for (int row = r0; row < r1; row++) {
        uint32_t *dst = c.pixels + (size_t)(y + row) * WIDTH + x;
        const uint32_t *src_row = pixels + (size_t)row * (size_t)size;
        for (int col = c0; col < c1; col++) {
            uint32_t src = src_row[col];
            unsigned a = (src >> 24) + ((src >> 24) == 255);
            if (a)
                dst[col] = a >= 256 ? (0xff000000 | src) : blend(dst[col], src, a);
        }
    }
}

static const uint32_t *get_scaled_art(const uint32_t *pixels, uint32_t art_gen, int size, int *out_opaque) {
    for (int i = 0; i < SCALED_ART_SLOTS; i++) {
        if (scaled_art[i].pixels && scaled_art[i].src == pixels &&
            scaled_art[i].art_gen == art_gen && scaled_art[i].size == size) {
            scaled_art[i].last_use = ++scaled_art_clock;
            stats.art_cache_hits++;
            *out_opaque = scaled_art[i].opaque;
            return scaled_art[i].pixels;
        }
    }
    int victim = 0;
    for (int i = 1; i < SCALED_ART_SLOTS; i++) {
        if (!scaled_art[i].pixels || scaled_art[i].last_use < scaled_art[victim].last_use)
            victim = i;
    }
    uint32_t *buf = malloc((size_t)size * (size_t)size * sizeof(*buf));
    if (!buf)
        return NULL;
    stats.art_scales++;
    int opaque = 1;
    for (int row = 0; row < size; row++) {
        const uint32_t *src_row = pixels + (size_t)(row * ART_SIZE / size) * ART_SIZE;
        uint32_t *dst_row = buf + (size_t)row * (size_t)size;
        for (int col = 0; col < size; col++) {
            uint32_t src = src_row[col * ART_SIZE / size];
            if ((src >> 24) != 255)
                opaque = 0;
            dst_row[col] = src;
        }
    }
    free(scaled_art[victim].pixels);
    scaled_art[victim] = (scaled_art_slot){
        .src = pixels, .art_gen = art_gen, .size = size,
        .opaque = opaque, .last_use = ++scaled_art_clock, .pixels = buf,
    };
    *out_opaque = opaque;
    return buf;
}

void image_art(canvas c, int x, int y, int size, const uint32_t *pixels,
               const uint32_t *thumb64, int opaque, uint32_t art_gen) {
    if (!c.pixels || !pixels || size <= 0)
        return;
    if (cache_enabled) {
        if (size == ART_THUMB && thumb64) {
            stats.art_cache_hits++;
            blit_prescaled(c, x, y, size, thumb64, opaque);
            return;
        }
        if (size == ART_SIZE) {
            stats.art_cache_hits++;
            blit_prescaled(c, x, y, size, pixels, opaque);
            return;
        }
        if (art_gen && size <= 512) {
            int scaled_opaque = opaque;
            const uint32_t *scaled = get_scaled_art(pixels, art_gen, size, &scaled_opaque);
            if (scaled) {
                blit_prescaled(c, x, y, size, scaled, scaled_opaque);
                return;
            }
        }
    }
    stats.art_scales++;
    int cx0, cy0, cx1, cy1;
    clip_bounds(c, &cx0, &cy0, &cx1, &cy1);
    int r0 = y < cy0 ? cy0 - y : 0;
    int r1 = y + size > cy1 ? cy1 - y : size;
    int c0 = x < cx0 ? cx0 - x : 0;
    int c1 = x + size > cx1 ? cx1 - x : size;
    for (int row = r0; row < r1; row++) {
        uint32_t *dst = c.pixels + (size_t)(y + row) * WIDTH + x;
        const uint32_t *src_row = pixels + (size_t)(row * ART_SIZE / size) * ART_SIZE;
        for (int col = c0; col < c1; col++) {
            uint32_t src = src_row[col * ART_SIZE / size];
            unsigned a = (src >> 24) + ((src >> 24) == 255);
            if (a)
                dst[col] = a >= 256 ? (0xff000000 | src) : blend(dst[col], src, a);
        }
    }
}

void image(canvas c, int x, int y, int size, const uint32_t *pixels) {
    image_art(c, x, y, size, pixels, NULL, 0, 0);
}
