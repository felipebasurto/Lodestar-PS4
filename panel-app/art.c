/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "draw.h"

#include <limits.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 1920
/* Never link __assert_fail/abort into the PS4 eboot: malformed PNGs must return NULL. */
#define STBI_ASSERT(x) ((void)0)
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
    if (!body || !size || size > (size_t)INT_MAX || width <= 0 || height <= 0)
        return NULL;
    int w = 0, h = 0, channels = 0;
    unsigned char *src = stbi_load_from_memory((const unsigned char *)body, (int)size, &w, &h, &channels, 4);
    uint32_t *out = src && w == width && h == height ? malloc((size_t)w * (size_t)h * sizeof(*out)) : NULL;
    for (int i = 0; out && i < w * h; i++)
        out[i] = argb(src + i * 4);
    stbi_image_free(src);
    return out;
}

uint32_t *decode_art_ex(const char *body, size_t size, uint32_t *accent, uint32_t **out_thumb64, int *out_opaque) {
    if (out_thumb64)
        *out_thumb64 = NULL;
    if (out_opaque)
        *out_opaque = 0;
    if (!body || !size || size > (size_t)INT_MAX)
        return NULL;
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory((const unsigned char *)body, (int)size, &w, &h, &channels) ||
        w <= 0 || h <= 0 || w > COVER_LIMIT || h > COVER_LIMIT)
        return NULL;
    unsigned char *src = stbi_load_from_memory((const unsigned char *)body, (int)size, &w, &h, &channels, 4);
    if (!src || w <= 0 || h <= 0) {
        stbi_image_free(src);
        return NULL;
    }
    uint32_t *art = malloc((size_t)ART_SIZE * ART_SIZE * sizeof(*art));
    if (art) {
        unsigned red = 0, green = 0, blue = 0;
        int opaque = 1;
        for (int y = 0; y < ART_SIZE; y++) {
            int sy = (int)((long long)y * h / ART_SIZE);
            const unsigned char *row = src + (size_t)sy * (size_t)w * 4;
            for (int x = 0; x < ART_SIZE; x++) {
                const unsigned char *p = row + (size_t)((long long)x * w / ART_SIZE) * 4;
                unsigned r = p[0] * p[3] / 255, g = p[1] * p[3] / 255, b = p[2] * p[3] / 255;
                art[y * ART_SIZE + x] = argb(p);
                if (p[3] != 255)
                    opaque = 0;
                red += r;
                green += g;
                blue += b;
            }
        }
        if (accent)
            *accent = (red >> 17) << 16 | (green >> 17) << 8 | (blue >> 17);
        if (out_opaque)
            *out_opaque = opaque;
        if (out_thumb64) {
            uint32_t *thumb = malloc((size_t)ART_THUMB * ART_THUMB * sizeof(*thumb));
            if (thumb) {
                for (int y = 0; y < ART_THUMB; y++) {
                    const uint32_t *arow = art + (size_t)(y * ART_SIZE / ART_THUMB) * ART_SIZE;
                    for (int x = 0; x < ART_THUMB; x++)
                        thumb[y * ART_THUMB + x] = arow[x * ART_SIZE / ART_THUMB];
                }
                *out_thumb64 = thumb;
            }
        }
    }
    stbi_image_free(src);
    return art;
}

uint32_t *decode_art(const char *body, size_t size, uint32_t *accent) {
    return decode_art_ex(body, size, accent, NULL, NULL);
}
