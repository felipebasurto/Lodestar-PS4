/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum { WIDTH = 1920, HEIGHT = 1080, ART_SIZE = 256, ART_THUMB = 64, BRAND_WIDTH = 360, BRAND_HEIGHT = 64 };

typedef struct {
    int x, y, w, h;
} rect;

typedef struct {
    uint32_t *pixels;
    int clip_x0, clip_y0, clip_x1, clip_y1;
} canvas;

typedef struct {
    uint64_t glyph_rasterizations;
    uint64_t glyph_cache_hits;
    uint64_t bg_bytes_copied;
    uint64_t art_scales;
    uint64_t art_cache_hits;
} draw_stats;

static inline canvas canvas_full(uint32_t *pixels) {
    return (canvas){.pixels = pixels, .clip_x0 = 0, .clip_y0 = 0, .clip_x1 = WIDTH, .clip_y1 = HEIGHT};
}

static inline canvas canvas_clip(uint32_t *pixels, rect r) {
    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > WIDTH ? WIDTH : r.x + r.w;
    int y1 = r.y + r.h > HEIGHT ? HEIGHT : r.y + r.h;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return (canvas){.pixels = pixels, .clip_x0 = x0, .clip_y0 = y0, .clip_x1 = x1, .clip_y1 = y1};
}

/* dir holds the fonts and pictures the app ships with. */
int font_open(const char *dir);
void font_close(void);
void draw_stats_reset(void);
draw_stats draw_stats_get(void);
void draw_set_cache_enabled(int enabled);
void draw_art_cache_invalidate(void);

unsigned utf8_read(const char **text);
size_t utf8_write(unsigned cp, char *out, size_t size);
void fill(canvas c, int x, int y, int w, int h, uint32_t color);
void shade(canvas c, int x, int y, int w, int h, uint32_t color, unsigned alpha);
void frame(canvas c, int x, int y, int w, int h, int weight, uint32_t color);
void ring(canvas c, int cx, int cy, int radius, int weight, uint32_t color);
void line(canvas c, int x1, int y1, int x2, int y2, uint32_t color);
/* Two voices, both as written: medium and bold. */
void text(canvas c, int x, int y, int width, int size, uint32_t color, const char *label);
void bold(canvas c, int x, int y, int width, int size, uint32_t color, const char *label);
int text_width(int size, const char *label);
void brand(canvas c, int x, int y);
void background(canvas c);
void background_rect(canvas c, int x, int y, int w, int h);
void image(canvas c, int x, int y, int size, const uint32_t *pixels);
void image_art(canvas c, int x, int y, int size, const uint32_t *pixels,
               const uint32_t *thumb64, int opaque, uint32_t art_gen);
uint32_t *decode_art(const char *body, size_t size, uint32_t *accent);
uint32_t *decode_art_ex(const char *body, size_t size, uint32_t *accent,
                        uint32_t **out_thumb64, int *out_opaque);
/* ARGB pixels of a PNG of exactly that size, or NULL. */
uint32_t *decode_picture(const char *body, size_t size, int width, int height);
