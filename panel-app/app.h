/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "api.h"
#include "applog.h"
#include "lane.h"
#include "view.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

enum {
    PANEL_BTN_UP       = 1u << 0,
    PANEL_BTN_DOWN     = 1u << 1,
    PANEL_BTN_LEFT     = 1u << 2,
    PANEL_BTN_RIGHT    = 1u << 3,
    PANEL_BTN_CROSS    = 1u << 4,
    PANEL_BTN_CIRCLE   = 1u << 5,
    PANEL_BTN_SQUARE   = 1u << 6,
    PANEL_BTN_TRIANGLE = 1u << 7,
    PANEL_BTN_L1       = 1u << 8,
    PANEL_BTN_R1       = 1u << 9,
    PANEL_BTN_L2       = 1u << 10,
};

enum panel_task {
    TASK_IDLE = 0,
    TASK_LOAD,
    TASK_STATUS,
    TASK_SCAN,
    TASK_CONFIG,
    TASK_SAVE,
    TASK_CHANGE,
    TASK_ART,
    TASK_LINKS,
    TASK_DESTS,
    TASK_ENQUEUE,
    TASK_ADDR,
};

enum ime_target { IME_NONE = 0, IME_SEARCH, IME_URL };
enum ime_status { IME_RUNNING = 0, IME_OK = 1, IME_CANCELED = 2 };

typedef struct {
    int connected;
    uint32_t buttons;
    int lx, ly; /* -128..127 */
    int rx, ry; /* -128..127 */
} panel_pad;

typedef struct {
    enum panel_task kind;
    char id[10];
    char path[96];
    char form[2048];
} panel_request;

typedef struct {
    enum panel_task kind;
    int ok;
    int http_status;
    int64_t elapsed_ms;
    char message[384];
    library *catalog;
    settings config;
    artwork cover;
    downloads links;
    destinations dests;
    char local_url[48];
} panel_result;

typedef struct {
    void *ctx;
    /* Called strictly on the worker thread, never on the input/render thread. */
    panel_result (*perform)(void *ctx, const panel_request *req, const int *stop);
    /* Called on the main thread for system IME dialog integration (may be NULL). */
    int (*ime_open)(void *ctx, enum ime_target target, const char *initial_utf8, size_t max_chars);
    enum ime_status (*ime_poll)(void *ctx, char *out_utf8, size_t out_cap);
    void (*ime_close)(void *ctx);
} panel_platform;

typedef struct {
    panel_platform plat;
    applog *log;

    pthread_mutex_t mutex;
    pthread_cond_t wake;
    pthread_t worker;
    int sync_ready;
    int worker_running;
    int quit_worker;
    int stop_flag; /* accessed with __atomic_* for cooperative HTTP abort */
    panel_request mailbox;
    panel_result reply;

    library *catalog;
    settings config, original_config;
    int spanish, online, config_ready, selected, in_sidebar, visible_count;
    int indices[GAME_LIMIT], counts[SECTIONS], category, setting_row;
    enum section section;
    enum page page;
    hold held;
    uint32_t held_button;
    int wheel, wheel_at;

    enum panel_task active;
    panel_request pending_write;
    panel_request pending_read;

    artwork artwork_cache[ART_SLOTS];
    unsigned next_art;
    uint32_t art_slot_gen;
    uint32_t catalog_gen, art_gen, links_gen, config_gen, dests_gen;
    char art_last_id[10];

    char query[256], feedback[384];
    char entered_url[2048], local_url[48];
    downloads links;
    destinations dests;
    int links_ready, dest_ready, dest, link_writing;
    int64_t links_at;

    int ime;
    enum ime_target ime_target;
    int64_t ime_cooldown_until;

    uint32_t last_buttons;
    uint32_t stick_dirs;
    int64_t repeat_at;
    int repeat_steps;
    char section_focus[SECTIONS][10];
    char pre_search_focus[10];
    int settings_visited;
    int dirty, quit;
    const char *quit_reason;
    int scan_seen;
    int64_t refresh_at, library_at, scan_until, feedback_until;
} panel_app;

void panel_result_free(panel_result *r);
const char *panel_task_name(enum panel_task kind);

int panel_app_init(panel_app *app, panel_platform plat, applog *log, int spanish);
int panel_app_start_worker(panel_app *app);
void panel_app_stop_worker(panel_app *app);
void panel_app_destroy(panel_app *app);

/* Processes completed worker replies, schedules non-blocking background tasks, handles IME,
 * processes controller input, wheel, holds and timers. Never performs blocking I/O. */
void panel_app_tick(panel_app *app, const panel_pad *pad, int64_t now_ms);

/* Fills a view snapshot for panel_renderer_present / panel_draw. */
void panel_app_build_view(const panel_app *app, int64_t now_ms, panel_view *out);

/* Helpers for direct testing/inspection. */
const game *panel_app_focused(const panel_app *app);
void panel_app_request_quit(panel_app *app, const char *reason);
