/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum { WIDTH = 1920, HEIGHT = 1080, ART_SIZE = 256 };

typedef struct {
    uint32_t *pixels;
} canvas;

int font_open(const char *path);
void font_close(void);
unsigned utf8_read(const char **text);
size_t utf8_write(unsigned cp, char *out, size_t size);
void fill(canvas c, int x, int y, int w, int h, uint32_t color);
void rounded_fill(canvas c, int x, int y, int w, int h, int radius, uint32_t color);
void line(canvas c, int x1, int y1, int x2, int y2, uint32_t color);
void text(canvas c, int x, int y, int width, int size, uint32_t color, const char *label);
void background(canvas c, uint32_t accent);
void image(canvas c, int x, int y, int size, int radius, const uint32_t *pixels);
uint32_t *decode_art(const char *body, size_t size, uint32_t *accent);
