/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "api.h"
#include "draw.h"
#include "lane.h"

enum { PAGE = 8, ART_SLOTS = 20, SETTINGS_COUNT = 9 };
enum page { LIBRARY, DETAIL, SETTINGS, PHONE, DOWNLOADS };

typedef struct {
    char id[10];
    uint32_t *pixels, accent;
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
} panel_view;

void panel_draw(const panel_view *v, canvas c);
