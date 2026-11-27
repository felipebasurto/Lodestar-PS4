/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

/* What the host run cost, from the renderer and drawing counters of the app plus what the
 * fake hardware saw. Counted from the last panel_metrics_reset() to the end of the run. */
typedef struct {
    uint64_t full_frame_copies;
    uint64_t fb_bytes_written;
    uint64_t redraws;
    uint64_t inputs;
    uint64_t flips_submitted;
    uint64_t text_rasterizations;
    uint64_t art_scalings;
    uint64_t blocking_loop_calls;
    uint64_t blocking_netctl_calls;
    uint64_t blocking_usleep_calls;
    uint64_t blocking_ime_calls;
} panel_metrics_t;

void panel_metrics_reset(void);
panel_metrics_t panel_metrics_get(void);

enum {
    PANEL_BLOCK_USLEEP = 1,
    PANEL_BLOCK_NETCTL = 2,
    PANEL_BLOCK_IME = 3,
};
