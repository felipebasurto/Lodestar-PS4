/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "render.h"

#include <stdlib.h>
#include <string.h>

static rect clip_screen(rect r) {
    int x0 = r.x < 0 ? 0 : r.x;
    int y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w > WIDTH ? WIDTH : r.x + r.w;
    int y1 = r.y + r.h > HEIGHT ? HEIGHT : r.y + r.h;
    if (x1 <= x0 || y1 <= y0)
        return (rect){0, 0, 0, 0};
    return (rect){x0, y0, x1 - x0, y1 - y0};
}

static int rects_should_merge(rect a, rect b) {
    /* Strictly overlapping rects must merge so blits never write a pixel twice. */
    if (a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h)
        return 1;
    /* Exact-edge touching rects merge with zero wasted area. */
    if (a.x == b.x && a.w == b.w && (a.y + a.h == b.y || b.y + b.h == a.y))
        return 1;
    if (a.y == b.y && a.h == b.h && (a.x + a.w == b.x || b.x + b.w == a.x))
        return 1;
    return 0;
}

static rect rect_union(rect a, rect b) {
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (rect){x0, y0, x1 - x0, y1 - y0};
}

void damage_clear(damage_set *d) {
    d->full = 0;
    d->count = 0;
}

void damage_mark_full(damage_set *d) {
    d->full = 1;
    d->count = 0;
}

int damage_empty(const damage_set *d) {
    return !d->full && d->count == 0;
}

void damage_add(damage_set *d, rect r) {
    if (d->full)
        return;
    r = clip_screen(r);
    if (r.w <= 0 || r.h <= 0)
        return;
    if (r.x == 0 && r.y == 0 && r.w == WIDTH && r.h == HEIGHT) {
        damage_mark_full(d);
        return;
    }

    /* Merge transitively with any overlapping or exact-edge touching rect so blits stay disjoint. */
    int merged;
    do {
        merged = 0;
        for (int i = 0; i < d->count; i++) {
            if (rects_should_merge(r, d->rects[i])) {
                r = rect_union(r, d->rects[i]);
                d->rects[i] = d->rects[--d->count];
                merged = 1;
                break;
            }
        }
    } while (merged);

    if (r.x == 0 && r.y == 0 && r.w == WIDTH && r.h == HEIGHT) {
        damage_mark_full(d);
        return;
    }
    if (d->count >= DAMAGE_MAX_RECTS) {
        damage_mark_full(d);
        return;
    }
    d->rects[d->count++] = r;

    /* If disjoint damage exceeds 85% of the screen, one full sequential copy is cheaper. */
    uint64_t area = 0;
    for (int i = 0; i < d->count; i++)
        area += (uint64_t)d->rects[i].w * (uint64_t)d->rects[i].h;
    if (area * 100 >= (uint64_t)WIDTH * HEIGHT * 85)
        damage_mark_full(d);
}

void damage_merge(damage_set *dst, const damage_set *src) {
    if (src->full) {
        damage_mark_full(dst);
        return;
    }
    for (int i = 0; i < src->count; i++)
        damage_add(dst, src->rects[i]);
}

int panel_renderer_init(panel_renderer *r) {
    memset(r, 0, sizeof(*r));
    r->shadow = malloc((size_t)WIDTH * HEIGHT * sizeof(*r->shadow));
    if (!r->shadow)
        return -1;
    for (int i = 0; i < WHEEL_ITEMS; i++) {
        r->wheel_bg[i] = malloc((size_t)PANEL_WHEEL_W * PANEL_WHEEL_H * sizeof(uint32_t));
        if (!r->wheel_bg[i]) {
            panel_renderer_close(r);
            return -1;
        }
    }
    damage_mark_full(&r->dirty[0]);
    damage_mark_full(&r->dirty[1]);
    return 0;
}

void panel_renderer_close(panel_renderer *r) {
    free(r->shadow);
    r->shadow = NULL;
    for (int i = 0; i < WHEEL_ITEMS; i++) {
        free(r->wheel_bg[i]);
        r->wheel_bg[i] = NULL;
    }
    r->prev.valid = 0;
    damage_clear(&r->dirty[0]);
    damage_clear(&r->dirty[1]);
}

void panel_renderer_invalidate(panel_renderer *r) {
    r->prev.valid = 0;
    damage_mark_full(&r->dirty[0]);
    damage_mark_full(&r->dirty[1]);
}

void panel_renderer_stats_reset(panel_renderer *r) { r->stats = (render_stats){0}; }
render_stats panel_renderer_stats(const panel_renderer *r) { return r->stats; }

static void copy_str(char *dst, size_t cap, const char *src) {
    if (!src) {
        dst[0] = 0;
        return;
    }
    size_t len = strlen(src);
    if (len >= cap)
        len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static void snapshot_from_view(view_snapshot *s, const panel_view *v) {
    memset(s, 0, sizeof(*s));
    s->valid = 1;
    s->page = v->page;
    s->section = v->section;
    s->spanish = v->spanish;
    s->online = v->online;
    s->has_catalog = v->catalog != NULL;
    s->selected = v->selected;
    s->in_sidebar = v->in_sidebar;
    s->visible_count = v->visible_count;
    s->config_ready = v->config_ready;
    s->category = v->category;
    s->setting_row = v->setting_row;
    s->holding = v->holding;
    s->held = v->held;
    s->wheel = v->wheel;
    s->wheel_at = v->wheel_at;
    s->links_ready = v->links_ready;
    s->links_configured = v->links ? v->links->configured : 0;
    s->dest_ready = v->dest_ready;
    s->dest = v->dest;
    s->link_writing = v->link_writing;
    s->waiting = v->waiting;
    s->loading_library = v->loading_library;
    s->loading_links = v->loading_links;
    s->loading_config = v->loading_config;
    s->loading_destinations = v->loading_destinations;
    s->catalog_gen = v->catalog_gen;
    s->art_gen = v->art_gen;
    s->links_gen = v->links_gen;
    s->config_gen = v->config_gen;
    s->dests_gen = v->dests_gen;
    copy_str(s->focused_id, sizeof(s->focused_id), v->focused ? v->focused->id : "");
    copy_str(s->query, sizeof(s->query), v->query);
    copy_str(s->feedback, sizeof(s->feedback), v->feedback);
    copy_str(s->local_url, sizeof(s->local_url), v->local_url);
    copy_str(s->entered_url, sizeof(s->entered_url), v->entered_url);
    if (v->config)
        s->config = *v->config;
    s->slot_count = panel_compute_library_slots(v, s->slots);
}

static int footer_differs(const view_snapshot *a, const view_snapshot *b) {
    if (!a->valid)
        return 1;
    if (a->waiting != b->waiting || a->link_writing != b->link_writing ||
        strcmp(a->feedback, b->feedback) != 0)
        return 1;
    if (b->page == LIBRARY &&
        ((a->visible_count > 0) != (b->visible_count > 0) ||
         (a->query[0] != 0) != (b->query[0] != 0)))
        return 1;
    if (b->page == DOWNLOADS &&
        ((a->entered_url[0] != 0) != (b->entered_url[0] != 0)))
        return 1;
    return 0;
}

static int needs_full_page_redraw(const view_snapshot *p, const view_snapshot *c) {
    if (!p->valid)
        return 1;
    if (p->page != c->page || p->spanish != c->spanish || p->online != c->online ||
        p->has_catalog != c->has_catalog || p->catalog_gen != c->catalog_gen)
        return 1;
    if (p->wheel && !c->wheel)
        return 1; /* wheel closed: repaint underlying screen */
    if (c->wheel && (p->art_gen != c->art_gen || p->selected != c->selected ||
                     p->setting_row != c->setting_row || p->category != c->category ||
                     p->in_sidebar != c->in_sidebar || p->section != c->section ||
                     p->visible_count != c->visible_count || p->dest != c->dest ||
                     p->links_gen != c->links_gen || p->config_gen != c->config_gen ||
                     p->dests_gen != c->dests_gen ||
                     p->waiting != c->waiting || p->link_writing != c->link_writing ||
                     strcmp(p->query, c->query) != 0 ||
                     strcmp(p->feedback, c->feedback) != 0 ||
                     strcmp(p->local_url, c->local_url) != 0 ||
                     strcmp(p->entered_url, c->entered_url) != 0 ||
                     memcmp(&p->config, &c->config, sizeof(settings)) != 0))
        return 1;

    if (c->page == LIBRARY) {
        if (!c->online && p->loading_library != c->loading_library)
            return 1;
    } else if (c->page == DETAIL) {
        if (strcmp(p->focused_id, c->focused_id) != 0)
            return 1;
    } else if (c->page == SETTINGS) {
        if (p->config_ready != c->config_ready)
            return 1;
        if (!c->config_ready && p->loading_config != c->loading_config)
            return 1;
    } else if (c->page == DOWNLOADS) {
        if (p->links_ready != c->links_ready || p->links_configured != c->links_configured)
            return 1;
        if ((!c->links_ready || !c->links_configured) && p->loading_links != c->loading_links)
            return 1;
    }
    return 0;
}

static void draw_full_shadow(panel_renderer *r, const panel_view *v) {
    canvas sh = canvas_full(r->shadow);
    if (v->wheel) {
        panel_view base = *v;
        base.wheel = 0;
        base.wheel_at = -1;
        panel_draw(&base, sh);
        panel_draw_wheel_open(v, sh, r->wheel_bg);
    } else {
        panel_draw(v, sh);
    }
}

/* Streams only the damaged rects from cached CPU shadow to hardware display buffer (write-only). */
static void blit_damage(panel_renderer *r, const damage_set *d, uint32_t *hw_back) {
    if (d->full) {
        size_t bytes = (size_t)WIDTH * HEIGHT * sizeof(*hw_back);
        memcpy(hw_back, r->shadow, bytes);
        r->stats.fb_blits++;
        r->stats.fb_full_copies++;
        r->stats.fb_bytes_written += bytes;
        return;
    }
    for (int i = 0; i < d->count; i++) {
        rect rc = d->rects[i];
        size_t row_bytes = (size_t)rc.w * sizeof(*hw_back);
        for (int row = 0; row < rc.h; row++) {
            size_t off = (size_t)(rc.y + row) * WIDTH + (size_t)rc.x;
            memcpy(hw_back + off, r->shadow + off, row_bytes);
        }
        r->stats.fb_blits++;
        r->stats.fb_bytes_written += (uint64_t)rc.h * row_bytes;
    }
}

int panel_renderer_present(panel_renderer *r, const panel_view *v, int back_idx, uint32_t *hw_back) {
    if (!r || !r->shadow || !v || !hw_back || (back_idx != 0 && back_idx != 1))
        return 0;

    view_snapshot cur;
    snapshot_from_view(&cur, v);
    const view_snapshot *prev = &r->prev;
    canvas sh = canvas_full(r->shadow);

    damage_set frame_dmg;
    damage_clear(&frame_dmg);

    int full = needs_full_page_redraw(prev, &cur);
    int footer_changed = footer_differs(prev, &cur);

    if (full) {
        draw_full_shadow(r, v);
        damage_mark_full(&frame_dmg);
        r->stats.shadow_full_redraws++;
    } else if (!prev->wheel && cur.wheel) {
        /* Apply any same-frame underlying hold cancel before taking wheel backdrop. */
        if (cur.page == LIBRARY && (prev->holding != cur.holding || prev->held != cur.held))
            panel_draw_open_row_region(v, sh);
        else if (cur.page == DETAIL && (prev->holding != cur.holding || prev->held != cur.held))
            panel_draw_detail_verbs_region(v, sh);
        panel_draw_wheel_open(v, sh, r->wheel_bg);
        damage_mark_full(&frame_dmg);
        r->stats.shadow_full_redraws++;
    } else if (prev->wheel && cur.wheel) {
        if (prev->wheel_at != cur.wheel_at) {
            if (prev->wheel_at >= 0) {
                panel_draw_wheel_pill(v, sh, prev->wheel_at, r->wheel_bg[prev->wheel_at]);
                damage_add(&frame_dmg, panel_wheel_pill_rect(prev->wheel_at));
            }
            if (cur.wheel_at >= 0) {
                panel_draw_wheel_pill(v, sh, cur.wheel_at, r->wheel_bg[cur.wheel_at]);
                damage_add(&frame_dmg, panel_wheel_pill_rect(cur.wheel_at));
            }
        }
        if (footer_changed) {
            panel_draw_footer_region(v, sh);
            damage_add(&frame_dmg, panel_footer_rect());
        }
        if (!damage_empty(&frame_dmg))
            r->stats.shadow_partial_redraws++;
    } else {
        int list_repainted = 0;

        if (cur.page == LIBRARY && cur.online && cur.has_catalog) {
            int section_or_query = prev->section != cur.section || strcmp(prev->query, cur.query) != 0;
            int vis_changed = prev->visible_count != cur.visible_count;
            if (section_or_query || vis_changed) {
                if (section_or_query) {
                    panel_draw_section_bar_region(v, sh);
                    damage_add(&frame_dmg, panel_section_bar_rect());
                }
                rect body = panel_list_rect();
                panel_draw_library_body_region(v, sh, body);
                damage_add(&frame_dmg, body);
                list_repainted = 1;
            } else if (cur.visible_count) {
                if (prev->selected != cur.selected) {
                    int prev_first = panel_list_first(prev->selected, prev->visible_count);
                    int cur_first = panel_list_first(cur.selected, cur.visible_count);
                    int same_slot_layout = prev->slot_count == cur.slot_count && cur.slot_count > 0 &&
                                           prev->holding == NONE && cur.holding == NONE;
                    for (int k = 0; same_slot_layout && k < cur.slot_count; k++) {
                        if (prev->slots[k].y != cur.slots[k].y || prev->slots[k].is_open != cur.slots[k].is_open)
                            same_slot_layout = 0;
                    }
                    if (same_slot_layout) {
                        for (int k = 0; k < cur.slot_count; k++) {
                            const panel_slot_span *ps = &prev->slots[k];
                            const panel_slot_span *cs = &cur.slots[k];
                            int max_x1 = ps->left_x1 > cs->left_x1 ? ps->left_x1 : cs->left_x1;
                            if (cs->is_open) {
                                rect lbox = {PANEL_LEFT, cs->y + 14, max_x1 - PANEL_LEFT, 212};
                                panel_draw_open_row_content_clipped(v, sh, cs->y, lbox);
                                damage_add(&frame_dmg, lbox);
                                if (ps->verbs_sig != cs->verbs_sig) {
                                    rect vbox = {1316, cs->y + 24, 528, 208};
                                    panel_draw_open_row_content_clipped(v, sh, cs->y, vbox);
                                    damage_add(&frame_dmg, vbox);
                                }
                            } else {
                                rect lbox = {PANEL_LEFT, cs->y + 10, max_x1 - PANEL_LEFT, 72};
                                panel_draw_closed_row_clipped(v, sh, cur_first + k, cs->y, lbox);
                                damage_add(&frame_dmg, lbox);
                                if (ps->right_x0 || cs->right_x0) {
                                    int rx0 = ps->right_x0 && cs->right_x0
                                                  ? (ps->right_x0 < cs->right_x0 ? ps->right_x0 : cs->right_x0)
                                                  : (ps->right_x0 ? ps->right_x0 : cs->right_x0);
                                    rect rbox = {rx0, cs->y + 24, PANEL_RIGHT + 16 - rx0, 50};
                                    panel_draw_closed_row_clipped(v, sh, cur_first + k, cs->y, rbox);
                                    damage_add(&frame_dmg, rbox);
                                }
                            }
                        }
                    } else {
                        rect sub = panel_list_rect();
                        if (prev_first == cur_first) {
                            int min_sel = prev->selected < cur.selected ? prev->selected : cur.selected;
                            int max_sel = prev->selected > cur.selected ? prev->selected : cur.selected;
                            int y0 = PANEL_LIST_TOP + (min_sel - cur_first) * PANEL_ROW;
                            int y1 = PANEL_LIST_TOP + (max_sel - cur_first) * PANEL_ROW + PANEL_OPEN_ROW;
                            if (y1 > PANEL_LIST_BOTTOM)
                                y1 = PANEL_LIST_BOTTOM;
                            sub.y = y0;
                            sub.h = y1 - y0;
                        }
                        panel_draw_library_list_region(v, sh, sub);
                        damage_add(&frame_dmg, sub);
                    }
                    list_repainted = 1;
                } else if (prev->holding != cur.holding || prev->held != cur.held) {
                    if (cur.holding != NONE && cur.holding == prev->holding && cur.held > prev->held) {
                        rect delta = {0};
                        panel_draw_hold_bar_delta(v, sh, prev->held, &delta);
                        if (delta.w > 0)
                            damage_add(&frame_dmg, delta);
                    } else if (cur.holding != prev->holding || (cur.holding != NONE && cur.held < prev->held)) {
                        panel_draw_open_row_region(v, sh);
                        damage_add(&frame_dmg, panel_open_row_rect(v));
                    }
                }
            }
        } else if (cur.page == DETAIL) {
            if (prev->holding != cur.holding || prev->held != cur.held) {
                if (cur.holding != NONE && cur.holding == prev->holding && cur.held > prev->held) {
                    rect delta = {0};
                    panel_draw_hold_bar_delta(v, sh, prev->held, &delta);
                    if (delta.w > 0)
                        damage_add(&frame_dmg, delta);
                } else if (cur.holding != prev->holding || (cur.holding != NONE && cur.held < prev->held)) {
                    panel_draw_detail_verbs_region(v, sh);
                    damage_add(&frame_dmg, panel_detail_verbs_rect());
                }
            }
        } else if (cur.page == SETTINGS && cur.config_ready) {
            int count = setting_start[cur.category + 1] - setting_start[cur.category];
            if (prev->category != cur.category) {
                panel_draw_settings_sidebar_region(v, sh);
                damage_add(&frame_dmg, panel_settings_sidebar_rect());
                panel_draw_settings_body_region(v, sh);
                damage_add(&frame_dmg, panel_settings_body_rect());
            } else if (prev->in_sidebar != cur.in_sidebar) {
                panel_draw_settings_sidebar_item_region(v, sh, cur.category);
                damage_add(&frame_dmg, panel_settings_sidebar_item_rect(cur.category));
                if (prev->setting_row != cur.setting_row && prev->setting_row >= 0 && prev->setting_row <= count) {
                    panel_draw_settings_row_region(v, sh, prev->setting_row);
                    damage_add(&frame_dmg, panel_settings_row_rect(prev->setting_row, count));
                }
                if (cur.setting_row >= 0 && cur.setting_row <= count) {
                    panel_draw_settings_row_region(v, sh, cur.setting_row);
                    damage_add(&frame_dmg, panel_settings_row_rect(cur.setting_row, count));
                }
                panel_draw_settings_help_region(v, sh);
                damage_add(&frame_dmg, panel_settings_help_rect());
                if (memcmp(&prev->config, &cur.config, sizeof(settings)) != 0 || prev->config_gen != cur.config_gen) {
                    panel_draw_settings_body_region(v, sh);
                    damage_add(&frame_dmg, panel_settings_body_rect());
                }
            } else if (prev->config_gen != cur.config_gen) {
                panel_draw_settings_body_region(v, sh);
                damage_add(&frame_dmg, panel_settings_body_rect());
            } else if (!cur.in_sidebar) {
                if (prev->setting_row != cur.setting_row) {
                    panel_draw_settings_row_region(v, sh, prev->setting_row);
                    damage_add(&frame_dmg, panel_settings_row_rect(prev->setting_row, count));
                    panel_draw_settings_row_region(v, sh, cur.setting_row);
                    damage_add(&frame_dmg, panel_settings_row_rect(cur.setting_row, count));
                    panel_draw_settings_help_region(v, sh);
                    damage_add(&frame_dmg, panel_settings_help_rect());
                } else if (memcmp(&prev->config, &cur.config, sizeof(settings)) != 0) {
                    panel_draw_settings_row_region(v, sh, cur.setting_row);
                    damage_add(&frame_dmg, panel_settings_row_rect(cur.setting_row, count));
                }
            } else if (memcmp(&prev->config, &cur.config, sizeof(settings)) != 0) {
                panel_draw_settings_body_region(v, sh);
                damage_add(&frame_dmg, panel_settings_body_rect());
            }
        } else if (cur.page == PHONE) {
            if (strcmp(prev->local_url, cur.local_url) != 0) {
                panel_draw_phone_addr_region(v, sh);
                damage_add(&frame_dmg, panel_phone_addr_rect());
            }
        } else if (cur.page == DOWNLOADS && cur.links_ready && cur.links_configured) {
            int url_or_dests = strcmp(prev->entered_url, cur.entered_url) != 0 ||
                               prev->dest_ready != cur.dest_ready ||
                               prev->dests_gen != cur.dests_gen ||
                               prev->loading_destinations != cur.loading_destinations;
            if (url_or_dests) {
                panel_draw_downloads_body_region(v, sh);
                damage_add(&frame_dmg, panel_downloads_body_rect());
            } else {
                if (prev->dest != cur.dest) {
                    panel_draw_downloads_dest_region(v, sh);
                    damage_add(&frame_dmg, panel_downloads_dest_rect());
                }
                if (prev->links_gen != cur.links_gen) {
                    panel_draw_downloads_status_region(v, sh);
                    damage_add(&frame_dmg, panel_downloads_status_rect());
                }
            }
        }

        if (prev->art_gen != cur.art_gen && !list_repainted) {
            if (cur.art_gen == prev->art_gen + 1 && v->art_last_id[0]) {
                rect cr;
                if (panel_find_cover_rect(v, v->art_last_id, &cr)) {
                    panel_draw_cover_region(v, sh, v->art_last_id, cr);
                    damage_add(&frame_dmg, cr);
                }
            } else if (cur.page == LIBRARY && cur.online && cur.visible_count) {
                rect list = panel_list_rect();
                panel_draw_library_list_region(v, sh, list);
                damage_add(&frame_dmg, list);
            } else if (cur.page == DETAIL && v->focused) {
                rect cr = {PANEL_LEFT, 156, 420, 420};
                panel_draw_cover_region(v, sh, v->focused->id, cr);
                damage_add(&frame_dmg, cr);
            }
        }

        if (footer_changed) {
            panel_draw_footer_region(v, sh);
            damage_add(&frame_dmg, panel_footer_rect());
        }

        if (!damage_empty(&frame_dmg))
            r->stats.shadow_partial_redraws++;
    }

    r->prev = cur;

    if (damage_empty(&frame_dmg)) {
        r->stats.skipped_frames++;
        return 0;
    }

    damage_merge(&r->dirty[0], &frame_dmg);
    damage_merge(&r->dirty[1], &frame_dmg);

    blit_damage(r, &r->dirty[back_idx], hw_back);
    damage_clear(&r->dirty[back_idx]);
    return 1;
}
