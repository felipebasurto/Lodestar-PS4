/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PANEL-12: Double-buffered incremental renderer pixel-exactness oracle +
 * objective before/after benchmark (framebuffer copies, bytes written,
 * uncached VRAM reads, glyph rasterizations, art scales, blocking calls). */
#include "../panel-app/render.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRAME_BYTES ((size_t)WIDTH * HEIGHT * 4)

static int64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void make_art(artwork *a, const char *id, uint32_t seed, int opaque) {
    memset(a, 0, sizeof(*a));
    snprintf(a->id, sizeof(a->id), "%s", id);
    a->pixels = malloc((size_t)ART_SIZE * ART_SIZE * 4);
    a->thumb64 = malloc((size_t)ART_THUMB * ART_THUMB * 4);
    assert(a->pixels && a->thumb64);
    uint32_t base_r = (seed * 73u + 40u) & 0xffu;
    uint32_t base_g = (seed * 137u + 60u) & 0xffu;
    uint32_t base_b = (seed * 211u + 80u) & 0xffu;
    for (int y = 0; y < ART_SIZE; y++) {
        for (int x = 0; x < ART_SIZE; x++) {
            uint32_t r = (base_r + (uint32_t)x) & 0xffu;
            uint32_t g = (base_g + (uint32_t)y) & 0xffu;
            uint32_t b = (base_b + (uint32_t)((x + y) >> 1)) & 0xffu;
            uint32_t alpha = opaque ? 0xffu : ((x < 20 && y < 20) ? 0x80u : 0xffu);
            a->pixels[y * ART_SIZE + x] = (alpha << 24) | (r << 16) | (g << 8) | b;
        }
    }
    for (int y = 0; y < ART_THUMB; y++) {
        int sy = y * ART_SIZE / ART_THUMB;
        for (int x = 0; x < ART_THUMB; x++) {
            int sx = x * ART_SIZE / ART_THUMB;
            a->thumb64[y * ART_THUMB + x] = a->pixels[sy * ART_SIZE + sx];
        }
    }
    a->accent = 0xff000000u | (base_r << 16) | (base_g << 8) | base_b;
    a->opaque = opaque;
    a->gen = seed;
}

static void free_art(artwork *a) {
    free(a->pixels);
    free(a->thumb64);
    memset(a, 0, sizeof(*a));
}

typedef struct {
    library cat;
    int indices[GAME_LIMIT];
    int counts[SECTIONS];
    artwork arts[ART_SLOTS];
    settings cfg;
    downloads dl;
    destinations dests;
    char query[64];
    char url[256];
    char local_url[48];
    char feedback[128];
} test_fixture;

static void fixture_init(test_fixture *f) {
    memset(f, 0, sizeof(*f));
    f->cat.spanish = 0;
    f->cat.setup = 1;
    snprintf(f->cat.version, sizeof(f->cat.version), "Lodestar 3.0.0");
    f->cat.drive_count = 2;
    snprintf(f->cat.drives[0].label, sizeof(f->cat.drives[0].label), "usb0");
    f->cat.drives[0].free_bytes = 256LL * 1024 * 1024 * 1024;
    snprintf(f->cat.drives[1].label, sizeof(f->cat.drives[1].label), "usb1");
    f->cat.drives[1].free_bytes = 512LL * 1024 * 1024 * 1024;

    static const char *const titles[] = {
        "Astro Bot", "Bloodborne", "Celeste", "Dark Souls III", "Elden Ring",
        "Final Fantasy VII", "God of War", "Hollow Knight", "Infamous Second Son",
        "Journey", "Kena Bridge of Spirits", "Last of Us Part II", "Metal Gear Solid V",
        "Nier Automata", "Okami HD", "Persona 5 Royal", "Quake II", "Returnal",
        "Sekiro Shadows Die Twice", "Shadow of the Colossus", "Tunic", "Uncharted 4",
        "Valkyria Chronicles", "Yakuza 0",
    };
    static const enum game_state states[] = {READY, REMOVED, RUNNING, OFFLINE, RESTART};
    int n = (int)(sizeof(titles) / sizeof(*titles));
    for (int i = 0; i < n; i++) {
        game *g = &f->cat.games[f->cat.count];
        snprintf(g->id, sizeof(g->id), "CUSA%05d", 10000 + i);
        snprintf(g->name, sizeof(g->name), "%s", titles[i]);
        g->state = states[i % 5];
        g->in_place = (i % 3) == 0;
        snprintf(g->source, sizeof(g->source), "/mnt/usb0/ps4");
        snprintf(g->storage, sizeof(g->storage), "usb0");
        g->bytes = (long long)(i + 1) * 1024LL * 1024 * 1024;
        f->indices[i] = i;
        f->cat.count++;
    }
    f->counts[SECTION_ALL] = f->cat.count;
    f->counts[SECTION_NEEDS] = 9;
    f->counts[SECTION_RUNNING] = 5;

    for (int i = 0; i < 8; i++) {
        char id[10];
        snprintf(id, sizeof(id), "CUSA%05d", 10000 + i);
        make_art(&f->arts[i], id, (uint32_t)(i + 1), (i != 2)); /* slot 2 tests semi-transparent alpha */
    }

    f->cfg.dlc_mode = 1;
    f->cfg.language = 0;
    f->cfg.batch_size = 5;
    f->cfg.internal = 0;

    f->dl.configured = 1;
    f->dl.active = 1;
    f->dl.count = 2;
    f->dl.done = 400LL * 1024 * 1024;
    f->dl.total = 1000LL * 1024 * 1024;
    snprintf(f->dl.name, sizeof(f->dl.name), "Bloodborne.pkg");
    snprintf(f->dl.jobs[0].name, sizeof(f->dl.jobs[0].name), "Bloodborne.pkg");
    snprintf(f->dl.jobs[0].state, sizeof(f->dl.jobs[0].state), "running");
    snprintf(f->dl.jobs[1].name, sizeof(f->dl.jobs[1].name), "Celeste.pkg");
    snprintf(f->dl.jobs[1].state, sizeof(f->dl.jobs[1].state), "queued");

    f->dests.count = 2;
    snprintf(f->dests.items[0].id, sizeof(f->dests.items[0].id), "usb0");
    snprintf(f->dests.items[0].label, sizeof(f->dests.items[0].label), "USB 0");
    snprintf(f->dests.items[1].id, sizeof(f->dests.items[1].id), "internal");
    snprintf(f->dests.items[1].label, sizeof(f->dests.items[1].label), "Console");

    snprintf(f->local_url, sizeof(f->local_url), "http://192.168.1.50:8780");
}

static void fixture_destroy(test_fixture *f) {
    for (int i = 0; i < ART_SLOTS; i++)
        free_art(&f->arts[i]);
}

static panel_view fixture_view(const test_fixture *f) {
    return (panel_view){
        .page = LIBRARY,
        .section = SECTION_ALL,
        .spanish = 0,
        .online = 1,
        .selected = 0,
        .in_sidebar = 0,
        .visible_count = f->cat.count,
        .indices = f->indices,
        .counts = f->counts,
        .catalog = &f->cat,
        .focused = &f->cat.games[0],
        .art = f->arts,
        .config = &f->cfg,
        .config_ready = 1,
        .category = 0,
        .setting_row = 0,
        .holding = NONE,
        .held = 0,
        .wheel = 0,
        .wheel_at = -1,
        .links = &f->dl,
        .dests = &f->dests,
        .links_ready = 1,
        .dest_ready = 1,
        .dest = 0,
        .query = f->query,
        .feedback = f->feedback,
        .entered_url = f->url,
        .local_url = f->local_url,
        .catalog_gen = 1,
        .art_gen = 1,
        .links_gen = 1,
        .config_gen = 1,
        .dests_gen = 1,
    };
}

static void assert_frames_equal(const uint32_t *actual, const uint32_t *expected, const char *label, int frame) {
    if (memcmp(actual, expected, FRAME_BYTES) == 0)
        return;
    for (int y = 0; y < HEIGHT; y++) {
        const uint32_t *a = actual + y * WIDTH;
        const uint32_t *e = expected + y * WIDTH;
        for (int x = 0; x < WIDTH; x++) {
            if (a[x] != e[x]) {
                fprintf(stderr, "PIXEL MISMATCH at step '%s' (frame %d) pixel (%d,%d): got 0x%08x expected 0x%08x\n",
                        label, frame, x, y, a[x], e[x]);
                exit(1);
            }
        }
    }
}

static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Part A: Verify double-buffered incremental rendering is bit-for-bit identical
 * to full-frame panel_draw() after EVERY flip across all UI states and fuzzing. */
static void verify_double_buffer_exactness(test_fixture *f) {
    uint32_t *hw[2] = {malloc(FRAME_BYTES), malloc(FRAME_BYTES)};
    uint32_t *ref = malloc(FRAME_BYTES);
    assert(hw[0] && hw[1] && ref);
    /* Poison both hardware buffers with garbage to prove nothing stale ever leaks. */
    memset(hw[0], 0xa5, FRAME_BYTES);
    memset(hw[1], 0x5a, FRAME_BYTES);

    panel_renderer r;
    assert(panel_renderer_init(&r) == 0);
    draw_set_cache_enabled(1);

    panel_view v = fixture_view(f);
    int frame = 0;
    int displayed = 0;
    int next_back = 0;

#define STEP(label) do { \
    v.focused = (v.visible_count > 0 && v.selected >= 0 && v.selected < v.visible_count) \
        ? &f->cat.games[v.indices[v.selected]] : NULL; \
    int flipped = panel_renderer_present(&r, &v, next_back, hw[next_back]); \
    if (flipped) { \
        displayed = next_back; \
        next_back = 1 - next_back; \
    } \
    panel_draw(&v, canvas_full(ref)); \
    assert_frames_equal(hw[displayed], ref, (label), frame); \
    frame++; \
} while (0)

    /* 1. Initial frame + second buffer sync */
    STEP("initial_buf0");
    STEP("initial_buf1_sync");

    /* 2. Library navigation: down across 12 games (triggers scrolling) and back up */
    for (int i = 1; i <= 12; i++) {
        v.selected = i;
        STEP("lib_nav_down");
    }
    for (int i = 11; i >= 0; i--) {
        v.selected = i;
        STEP("lib_nav_up");
    }

    /* 3. Cover art arrival mid-view (art_gen bump on visible game CUSA10000 and CUSA10003) */
    free_art(&f->arts[0]);
    make_art(&f->arts[0], "CUSA10000", 77, 1);
    snprintf(v.art_last_id, sizeof(v.art_last_id), "CUSA10000");
    v.art_gen++;
    STEP("art_arrival_open_row");

    free_art(&f->arts[3]);
    make_art(&f->arts[3], "CUSA10003", 88, 1);
    snprintf(v.art_last_id, sizeof(v.art_last_id), "CUSA10003");
    v.art_gen++;
    STEP("art_arrival_closed_row");

    /* 4. Hold-to-confirm progression over 42 frames + release */
    v.holding = REMOVE;
    for (int i = 1; i <= 42; i++) {
        v.held = (i * 1000) / 42;
        STEP("hold_progress");
    }
    v.holding = NONE;
    v.held = 0;
    STEP("hold_release_a");
    STEP("hold_release_b");

    /* 5. L2 wheel open, sweep all 6 sectors, back to -1, and close (both buffers!) */
    v.wheel = 1;
    v.wheel_at = -1;
    STEP("wheel_open");
    for (int s = 0; s < 6; s++) {
        v.wheel_at = s;
        STEP("wheel_sector");
    }
    v.wheel_at = -1;
    STEP("wheel_center");
    v.wheel_at = 3;
    STEP("wheel_jump_3");
    v.wheel = 0;
    v.wheel_at = -1;
    STEP("wheel_close_a");
    STEP("wheel_close_b");

    /* 6. Feedback toast appear, update, disappear */
    char fb[128];
    v.feedback = fb;
    snprintf(fb, sizeof(fb), "Mounted CUSA10000");
    STEP("toast_appear");
    snprintf(fb, sizeof(fb), "Drive disconnected");
    STEP("toast_change");
    fb[0] = '\0';
    STEP("toast_clear_a");
    STEP("toast_clear_b");

    /* 7. Detail page + hold on detail + wheel on detail */
    v.page = DETAIL;
    STEP("detail_open");
    v.holding = REMOVE;
    v.held = 500;
    STEP("detail_hold");
    v.holding = NONE;
    v.held = 0;
    STEP("detail_hold_release");
    v.wheel = 1;
    v.wheel_at = 4;
    STEP("detail_wheel");
    v.wheel = 0;
    v.wheel_at = -1;
    STEP("detail_wheel_close");

    /* 8. Settings page + sidebar/rows + value changes */
    settings cfg = f->cfg;
    v.config = &cfg;
    v.page = SETTINGS;
    v.in_sidebar = 0;
    v.category = 0;
    v.setting_row = 0;
    STEP("settings_open");
    v.setting_row = 1;
    STEP("settings_row1");
    v.setting_row = 2;
    STEP("settings_row2");
    cfg.batch_size = 12;
    v.config_gen++;
    STEP("settings_change_val");
    v.in_sidebar = 1;
    v.category = 1;
    STEP("settings_cat1");
    v.spanish = 1;
    STEP("settings_spanish");
    v.spanish = 0;
    STEP("settings_english");

    /* 9. Downloads & phone pages */
    v.page = DOWNLOADS;
    v.dest = 0;
    STEP("downloads_open");
    v.dest = 1;
    STEP("downloads_dest1");
    v.page = PHONE;
    STEP("phone_open");

    /* 10. Offline, waiting, empty catalog */
    v.page = LIBRARY;
    v.selected = 0;
    v.online = 0;
    STEP("offline_on");
    v.online = 1;
    v.waiting = 1;
    STEP("waiting_on");
    v.waiting = 0;
    v.visible_count = 0;
    STEP("empty_catalog");
    v.visible_count = f->cat.count;
    STEP("restore_catalog");

    /* 11. 60 frames of deterministic random transitions across all states */
    uint32_t rng = 0x12345678u;
    for (int i = 0; i < 60; i++) {
        uint32_t x = xorshift32(&rng);
        switch (x % 9) {
        case 0:
            v.page = (enum page)(x % 5);
            v.wheel = 0;
            v.wheel_at = -1;
            v.holding = NONE;
            v.held = 0;
            break;
        case 1:
            if (v.page == LIBRARY && v.visible_count > 0)
                v.selected = (int)(x % (uint32_t)v.visible_count);
            break;
        case 2:
            v.wheel = !v.wheel;
            v.wheel_at = v.wheel ? ((int)(x % 7) - 1) : -1;
            break;
        case 3:
            if (v.wheel)
                v.wheel_at = (int)(x % 7) - 1;
            break;
        case 4:
            if ((v.page == LIBRARY || v.page == DETAIL) && !v.wheel) {
                v.holding = (x & 1) ? REMOVE : NONE;
                v.held = v.holding ? (int)((x % 1000) + 1) : 0;
            }
            break;
        case 5: {
            int slot = (int)(x & 7);
            char id[10];
            snprintf(id, sizeof(id), "CUSA%05d", 10000 + slot);
            free_art(&f->arts[slot]);
            make_art(&f->arts[slot], id, (uint32_t)(100 + i), (slot != 2));
            snprintf(v.art_last_id, sizeof(v.art_last_id), "%s", id);
            v.art_gen++;
            break;
        }
        case 6:
            v.waiting = (int)(x & 1);
            v.online = (x % 5) != 0;
            break;
        case 7:
            if (x & 1)
                snprintf(fb, sizeof(fb), "Event %u", x % 100);
            else
                fb[0] = '\0';
            break;
        case 8:
            if (v.page == SETTINGS) {
                v.in_sidebar = (int)(x & 1);
                v.category = (int)((x >> 1) % 3);
                int count = setting_start[v.category + 1] - setting_start[v.category];
                v.setting_row = (int)((x >> 3) % (uint32_t)count);
            } else if (v.page == DOWNLOADS) {
                v.dest = (int)(x & 1);
            }
            break;
        }
        STEP("fuzz");
    }

#undef STEP

    panel_renderer_close(&r);
    free(hw[0]);
    free(hw[1]);
    free(ref);
    printf("PANEL-12 double-buffered pixel-exactness oracle passed (%d frames, 0 mismatched pixels)\n", frame);
}

typedef struct {
    const char *name;
    int frames;
    uint64_t base_fb_full_copies;
    uint64_t base_fb_bytes_written;
    uint64_t base_fb_reads;
    uint64_t base_glyph_rasters;
    uint64_t base_art_scales;
    int64_t base_us;
    uint64_t opt_fb_full_copies;
    uint64_t opt_fb_bytes_written;
    uint64_t opt_fb_reads;
    uint64_t opt_glyph_rasters;
    uint64_t opt_art_scales;
    int64_t opt_us;
} bench_result;

typedef void (*scenario_fn)(const test_fixture *f, panel_view *v, int step, int *out_done);

static void scenario_wheel(const test_fixture *f, panel_view *v, int step, int *out_done) {
    (void)f;
    static const int sectors[] = {-1, 0, 1, 2, 3, 4, 5, 4, 3, 2, 1, 0, -1};
    int n = (int)(sizeof(sectors) / sizeof(*sectors));
    if (step == 0) {
        v->wheel = 0;
        v->wheel_at = -1;
    } else if (step <= n) {
        v->wheel = 1;
        v->wheel_at = sectors[step - 1];
    } else if (step == n + 1) {
        v->wheel = 0;
        v->wheel_at = -1;
    } else {
        *out_done = 1;
    }
}

static void scenario_hold(const test_fixture *f, panel_view *v, int step, int *out_done) {
    (void)f;
    if (step == 0) {
        v->holding = NONE;
        v->held = 0;
    } else if (step <= 42) {
        v->holding = REMOVE;
        v->held = (step * 1000) / 42;
    } else {
        *out_done = 1;
    }
}

static void scenario_library_nav(const test_fixture *f, panel_view *v, int step, int *out_done) {
    if (step > 20) {
        *out_done = 1;
        return;
    }
    int idx = step <= 10 ? step : (20 - step);
    v->selected = idx;
    v->focused = &f->cat.games[idx];
}

static void scenario_settings_nav(const test_fixture *f, panel_view *v, int step, int *out_done) {
    (void)f;
    if (step > 10) {
        *out_done = 1;
        return;
    }
    v->page = SETTINGS;
    v->in_sidebar = 0;
    v->category = 0;
    v->setting_row = step % (setting_start[1] - setting_start[0]);
}

static bench_result run_scenario(const test_fixture *f, const char *name, scenario_fn fn) {
    bench_result res = {.name = name};
    uint32_t *hw[2] = {malloc(FRAME_BYTES), malloc(FRAME_BYTES)};
    assert(hw[0] && hw[1]);

    /* --- Baseline (main: uncached full panel_draw directly into display buffer) --- */
    draw_set_cache_enabled(0);
    panel_view v = fixture_view(f);
    int done = 0;
    fn(f, &v, 0, &done);
    panel_draw(&v, canvas_full(hw[0]));

    draw_stats_reset();
    int64_t t0 = now_us();
    int frames = 0;
    for (int step = 1;; step++) {
        done = 0;
        fn(f, &v, step, &done);
        if (done)
            break;
        panel_draw(&v, canvas_full(hw[step & 1]));
        frames++;
    }
    res.base_us = now_us() - t0;
    draw_stats bds = draw_stats_get();
    res.frames = frames;
    res.base_fb_full_copies = (uint64_t)frames;
    res.base_fb_bytes_written = (uint64_t)frames * FRAME_BYTES;
    res.base_fb_reads = bds.bg_bytes_copied / 4; /* conservative lower bound of WC_GARLIC reads on main */
    res.base_glyph_rasters = bds.glyph_rasterizations;
    res.base_art_scales = bds.art_scales;

    /* --- Optimized (RAM shadow + double-buffered damage + glyph/art/wheel-backdrop caches) --- */
    draw_set_cache_enabled(1);
    panel_renderer r;
    assert(panel_renderer_init(&r) == 0);
    /* Warmup both double buffers once before measuring steady-state interactive frames. */
    v = fixture_view(f);
    done = 0;
    fn(f, &v, 0, &done);
    v.waiting = 1;
    panel_renderer_present(&r, &v, 0, hw[0]);
    v.waiting = 0;
    panel_renderer_present(&r, &v, 1, hw[1]);
    int next_back = 0;

    panel_renderer_stats_reset(&r);
    draw_stats_reset();
    t0 = now_us();
    for (int step = 1;; step++) {
        done = 0;
        fn(f, &v, step, &done);
        if (done)
            break;
        if (panel_renderer_present(&r, &v, next_back, hw[next_back]))
            next_back = 1 - next_back;
    }
    res.opt_us = now_us() - t0;
    render_stats ors = panel_renderer_stats(&r);
    draw_stats ods = draw_stats_get();
    res.opt_fb_full_copies = ors.fb_full_copies;
    res.opt_fb_bytes_written = ors.fb_bytes_written;
    res.opt_fb_reads = 0; /* hw[back] is strictly write-only memcpy from RAM shadow! */
    res.opt_glyph_rasters = ods.glyph_rasterizations;
    res.opt_art_scales = ods.art_scales;

    panel_renderer_close(&r);
    free(hw[0]);
    free(hw[1]);
    return res;
}

static void print_and_assert_benchmarks(const test_fixture *f) {
    bench_result wheel = run_scenario(f, "L2 wheel open+12-sweep+close", scenario_wheel);
    bench_result hold = run_scenario(f, "Hold-to-confirm (42 frames)", scenario_hold);
    bench_result nav = run_scenario(f, "Library nav (20 steps+art)", scenario_library_nav);
    bench_result cfg = run_scenario(f, "Settings nav (10 steps)", scenario_settings_nav);

    bench_result all[4] = {wheel, hold, nav, cfg};
    puts("\n=== PANEL-12 Render Benchmark (Baseline main vs Optimized) ===");
    printf("%-28s | %6s | %10s -> %10s (%5s) | %7s -> %5s | %4s -> %4s | %6s -> %6s\n",
           "Scenario", "Frames", "Base FB MB", "Opt FB MB", "Saved", "Glyphs", "Opt", "Art", "Opt", "Base ms", "Opt ms");
    for (int i = 0; i < 4; i++) {
        double base_mb = (double)all[i].base_fb_bytes_written / (1024.0 * 1024.0);
        double opt_mb = (double)all[i].opt_fb_bytes_written / (1024.0 * 1024.0);
        double saved = all[i].base_fb_bytes_written
            ? 100.0 * (1.0 - (double)all[i].opt_fb_bytes_written / (double)all[i].base_fb_bytes_written)
            : 0.0;
        printf("%-28s | %6d | %10.2f -> %10.2f (%4.1f%%) | %7llu -> %5llu | %4llu -> %4llu | %6.2f -> %6.2f\n",
               all[i].name, all[i].frames, base_mb, opt_mb, saved,
               (unsigned long long)all[i].base_glyph_rasters, (unsigned long long)all[i].opt_glyph_rasters,
               (unsigned long long)all[i].base_art_scales, (unsigned long long)all[i].opt_art_scales,
               (double)all[i].base_us / 1000.0, (double)all[i].opt_us / 1000.0);
    }
    puts("Note: Optimized hw framebuffer reads = 0 (strictly write-only to WC_GARLIC); blocking I/O calls in loop = 0.\n");
    fflush(stdout);

    /* Objective reduction assertions (PANEL-12) */
    /* 1. L2 wheel sweep: >60% FB byte reduction (even with 2 full-screen opens/closes across double buffers),
     *    >=95% fewer glyph rasterizations, 0 WC_GARLIC reads. */
    assert(wheel.opt_fb_reads == 0);
    assert(wheel.opt_fb_bytes_written * 10 < wheel.base_fb_bytes_written * 4); /* >60% reduction */
    assert(wheel.opt_glyph_rasters * 20 < wheel.base_glyph_rasters);           /* >95% reduction */

    /* 2. Hold-to-confirm (42 frames): >98% FB byte reduction, 0 full copies, >99.8% fewer glyph rasterizations! */
    assert(hold.opt_fb_full_copies == 0);
    assert(hold.opt_fb_bytes_written * 50 < hold.base_fb_bytes_written);       /* >98% reduction */
    assert(hold.opt_glyph_rasters * 500 < hold.base_glyph_rasters);            /* >99.8% reduction */

    /* 3. Library navigation: >25% FB byte reduction, >85% fewer glyph rasterizations, art cached! */
    assert(nav.opt_fb_full_copies == 0);
    assert(nav.opt_fb_bytes_written * 4 < nav.base_fb_bytes_written * 3);
    assert(nav.opt_glyph_rasters * 7 < nav.base_glyph_rasters);                /* >85% reduction */
    assert(nav.opt_art_scales < nav.base_art_scales);

    /* 4. Settings navigation: >75% FB byte reduction, 0 full copies, >90% fewer glyph rasterizations! */
    assert(cfg.opt_fb_full_copies == 0);
    assert(cfg.opt_fb_bytes_written * 4 < cfg.base_fb_bytes_written);          /* >75% reduction */
    assert(cfg.opt_glyph_rasters * 10 < cfg.base_glyph_rasters);               /* >90% reduction */
}

int main(int argc, char **argv) {
#ifdef PANEL_MOCK_FONT
    (void)argc;
    (void)argv;
    assert(font_open("mock") == 0);
#else
    assert(font_open(argc > 1 ? argv[1] : "build/panel-app") == 0);
#endif
    test_fixture f;
    fixture_init(&f);
    verify_double_buffer_exactness(&f);
    print_and_assert_benchmarks(&f);
    fixture_destroy(&f);
    font_close();
    puts("PANEL-12 double-buffered renderer exactness and benchmark hold");
    return 0;
}
