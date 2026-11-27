/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum { WIDTH = 1920, HEIGHT = 1080, ART_SIZE = 256, BRAND_WIDTH = 360, BRAND_HEIGHT = 64 };

typedef struct {
    uint32_t *pixels;
} canvas;

/* dir holds the fonts and pictures the app ships with. */
int font_open(const char *dir);
void font_close(void);
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
void image(canvas c, int x, int y, int size, const uint32_t *pixels);
uint32_t *decode_art(const char *body, size_t size, uint32_t *accent);
/* ARGB pixels of a PNG of exactly that size, or NULL. */
uint32_t *decode_picture(const char *body, size_t size, int width, int height);
