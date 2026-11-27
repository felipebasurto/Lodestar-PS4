/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t full_frame_copies;
    uint64_t full_frame_shades;
    uint64_t fb_bytes_written;
    uint64_t redraws;
    uint64_t inputs;
    uint64_t flips_submitted;
    uint64_t text_rasterizations;
    uint64_t text_rasterizations_measure;
    uint64_t art_scalings;
    uint64_t art_scaled_pixels;
    uint64_t blocking_loop_calls;
    uint64_t blocking_netctl_calls;
    uint64_t blocking_usleep_calls;
    uint64_t blocking_ime_calls;
} panel_metrics_t;

void panel_metrics_reset(void);
panel_metrics_t panel_metrics_get(void);
void panel_metrics_on_full_frame_copy(size_t bytes);
void panel_metrics_on_full_frame_shade(void);
void panel_metrics_on_fb_write(size_t bytes);
void panel_metrics_on_redraw(void);
void panel_metrics_on_input(void);
void panel_metrics_on_flip(void);
void panel_metrics_on_text_raster(int drawing);
void panel_metrics_on_art_scale(int size);
void panel_metrics_on_blocking_call(int kind);

enum {
    PANEL_BLOCK_USLEEP = 1,
    PANEL_BLOCK_NETCTL = 2,
    PANEL_BLOCK_IME = 3,
};
