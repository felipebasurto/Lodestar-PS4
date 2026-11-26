/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "api.h"
#include "draw.h"

enum { COLS = 5, PAGE = COLS * 2, ART_SLOTS = PAGE * 2, SETTINGS_COUNT = 9 };
enum page { LIBRARY, DETAIL, SETTINGS, PHONE, DOWNLOADS };
enum action { NONE, ADD, REMOVE, RESTORE };
enum filter { ALL, REBOOTS, OPEN };

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
int available_actions(const game *g, int online, enum action out[2]);

typedef struct {
    enum page page;
    enum action confirm;
    enum filter filter;
    int spanish, online, sidebar, selected, in_sidebar, visible_count;
    const int *indices, *counts;
    const library *catalog;
    const game *focused;
    const artwork *art;
    const settings *config;
    int config_ready, category, setting_row, action_row;
    const downloads *links;
    const destinations *dests;
    int links_ready, dest_ready, dest, link_writing, waiting;
    int loading_library, loading_links, loading_config, loading_destinations;
    const char *query, *feedback, *entered_url, *local_url;
} panel_view;

void panel_draw(const panel_view *v, canvas c);
