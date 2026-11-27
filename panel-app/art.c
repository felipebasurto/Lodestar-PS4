/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "draw.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 1920
#define STB_IMAGE_IMPLEMENTATION
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "vendor/stb_image.h"
#pragma clang diagnostic pop

/* Covers come over the network and keep a tighter limit than the app's own pictures. */
enum { COVER_LIMIT = 1536 };

static uint32_t argb(const unsigned char *p) {
    return (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

uint32_t *decode_picture(const char *body, size_t size, int width, int height) {
    int w, h, channels;
    unsigned char *src = stbi_load_from_memory((const unsigned char *)body, (int)size, &w, &h, &channels, 4);
    uint32_t *out = src && w == width && h == height ? malloc((size_t)w * h * sizeof(*out)) : NULL;
    for (int i = 0; out && i < w * h; i++)
        out[i] = argb(src + i * 4);
    stbi_image_free(src);
    return out;
}

uint32_t *decode_art(const char *body, size_t size, uint32_t *accent) {
    int w, h, channels;
    if (!stbi_info_from_memory((const unsigned char *)body, (int)size, &w, &h, &channels) ||
        w > COVER_LIMIT || h > COVER_LIMIT)
        return NULL;
    unsigned char *src = stbi_load_from_memory((const unsigned char *)body, (int)size, &w, &h, &channels, 4);
    if (!src)
        return NULL;
    uint32_t *art = malloc(ART_SIZE * ART_SIZE * sizeof(*art));
    if (art) {
        unsigned red = 0, green = 0, blue = 0;
        for (int y = 0; y < ART_SIZE; y++) {
            for (int x = 0; x < ART_SIZE; x++) {
                const unsigned char *p = src + ((y * h / ART_SIZE) * w + x * w / ART_SIZE) * 4;
                unsigned r = p[0] * p[3] / 255, g = p[1] * p[3] / 255, b = p[2] * p[3] / 255;
                art[y * ART_SIZE + x] = argb(p);
                red += r;
                green += g;
                blue += b;
            }
        }
        *accent = (red >> 17) << 16 | (green >> 17) << 8 | (blue >> 17);
    }
    stbi_image_free(src);
    return art;
}
