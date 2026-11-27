/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "view.h"

#include <stdint.h>

enum { DAMAGE_MAX_RECTS = 16 };

typedef struct {
    int full;
    int count;
    rect rects[DAMAGE_MAX_RECTS];
} damage_set;

typedef struct {
    uint64_t fb_blits;
    uint64_t fb_full_copies;
    uint64_t fb_bytes_written;
    uint64_t fb_reads;
    uint64_t shadow_full_redraws;
    uint64_t shadow_partial_redraws;
    uint64_t skipped_frames;
} render_stats;

typedef struct {
    int valid;
    enum page page;
    enum section section;
    int spanish, online, has_catalog, selected, in_sidebar, visible_count;
    int config_ready, category, setting_row;
    enum action holding;
    int held, wheel, wheel_at;
    int links_ready, dest_ready, dest, link_writing, waiting;
    int loading_library, loading_links, loading_config, loading_destinations;
    uint32_t catalog_gen, art_gen, links_gen, config_gen, dests_gen;
    char focused_id[10];
    char query[256];
    char feedback[384];
    char local_url[48];
    char entered_url[2048];
    settings config;
} view_snapshot;

typedef struct {
    uint32_t *shadow;
    uint32_t *wheel_bg[WHEEL_ITEMS];
    damage_set dirty[2];
    view_snapshot prev;
    render_stats stats;
} panel_renderer;

/* Damage set operations (exposed for direct unit testing of double-buffering invariants). */
void damage_clear(damage_set *d);
void damage_mark_full(damage_set *d);
void damage_add(damage_set *d, rect r);
void damage_merge(damage_set *dst, const damage_set *src);
int damage_empty(const damage_set *d);

int panel_renderer_init(panel_renderer *r);
void panel_renderer_close(panel_renderer *r);
void panel_renderer_invalidate(panel_renderer *r);
void panel_renderer_stats_reset(panel_renderer *r);
render_stats panel_renderer_stats(const panel_renderer *r);

/* Updates CPU shadow buffer for minimal damage and streams only dirty[back_idx] write-only
 * into hw_back (back_idx must be 0 or 1). Returns 1 if hw_back was updated and should be flipped,
 * 0 if nothing changed visually. */
int panel_renderer_present(panel_renderer *r, const panel_view *v, int back_idx, uint32_t *hw_back);
