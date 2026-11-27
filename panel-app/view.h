/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "api.h"
#include "draw.h"
#include "lane.h"

enum { PAGE = 8, ART_SLOTS = 20, SETTINGS_COUNT = 9 };
enum page { LIBRARY, DETAIL, SETTINGS, PHONE, DOWNLOADS };

enum {
    PANEL_LEFT = 80,
    PANEL_RIGHT = 1840,
    PANEL_LIST_TOP = 196,
    PANEL_LIST_BOTTOM = 1000,
    PANEL_ROW = 92,
    PANEL_OPEN_ROW = 240,
    PANEL_EDGE = 24,
    PANEL_HEADER_BOTTOM = 112,
    PANEL_WHEEL_W = 380,
    PANEL_WHEEL_H = 96,
};

typedef struct {
    char id[10];
    uint32_t *pixels;
    uint32_t *thumb64;
    uint32_t accent;
    int opaque;
    uint32_t gen;
} artwork;

typedef struct {
    const char *key, *en, *es, *help_en, *help_es;
    size_t offset;
    int min, max;
} setting;

extern const setting fields[SETTINGS_COUNT];
extern const int setting_start[4];

typedef struct {
    enum page page;
    enum section section;
    int spanish, online, selected, in_sidebar, visible_count;
    const int *indices, *counts;
    const library *catalog;
    const game *focused;
    const artwork *art;
    const settings *config;
    int config_ready, category, setting_row;
    enum action holding;
    int held, wheel, wheel_at; /* held: thousandths of the hold; wheel_at: -1 before pointing */
    const downloads *links;
    const destinations *dests;
    int links_ready, dest_ready, dest, link_writing, waiting;
    int loading_library, loading_links, loading_config, loading_destinations;
    const char *query, *feedback, *entered_url, *local_url;
    uint32_t catalog_gen, art_gen, links_gen, config_gen, dests_gen;
    char art_last_id[10];
} panel_view;

/* Full-screen reference render. */
void panel_draw(const panel_view *v, canvas c);

/* Layout geometry & surgical partial-redraw primitives (pixel-exact with panel_draw). */
rect panel_header_rect(void);
rect panel_footer_rect(void);
rect panel_list_rect(void);
rect panel_wheel_pill_rect(int idx);
int panel_open_row_y(const panel_view *v);
rect panel_open_row_rect(const panel_view *v);
rect panel_hold_bar_rect(const panel_view *v);
rect panel_detail_verbs_rect(void);
rect panel_settings_row_rect(int row, int count);
rect panel_settings_help_rect(void);
int panel_find_cover_rect(const panel_view *v, const char *id, rect *out);

void panel_draw_header_region(const panel_view *v, canvas c);
void panel_draw_footer_region(const panel_view *v, canvas c);
void panel_draw_library_list_region(const panel_view *v, canvas c, rect clip);
void panel_draw_open_row_region(const panel_view *v, canvas c);
void panel_draw_hold_bar_delta(const panel_view *v, canvas c, int prev_held, rect *out_damage);
void panel_draw_cover_region(const panel_view *v, canvas c, const char *id, rect r);
void panel_draw_wheel_open(const panel_view *v, canvas c, uint32_t *pill_bg[WHEEL_ITEMS]);
void panel_draw_wheel_pill(const panel_view *v, canvas c, int idx, const uint32_t *pill_bg);
void panel_draw_detail_verbs_region(const panel_view *v, canvas c);
void panel_draw_settings_row_region(const panel_view *v, canvas c, int row);
void panel_draw_settings_help_region(const panel_view *v, canvas c);
