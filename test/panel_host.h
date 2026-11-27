/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host harness for executing panel-app state machines, rendering, artwork decoding,
 * IME, Pad, VideoOut, virtual clock and loopback HTTP without PS4 hardware. */
#pragma once

#include "../panel-app/view.h"
#include "panel_fakes/orbis/ImeDialog.h"
#include "panel_fakes/orbis/Pad.h"
#include "panel_fakes/panel_host_hooks.h"
#include "panel_fakes/panel_metrics.h"

#include <stddef.h>
#include <stdint.h>

enum {
    PANEL_NET_OK = 0,
    PANEL_NET_HTTP_500 = 1,
    PANEL_NET_TRUNCATED = 2,
    PANEL_NET_MALFORMED_JSON = 3,
    PANEL_NET_REFUSED = 4,
};

typedef struct {
    char method[8];
    char path[128];
    char body[256];
} panel_req_log_t;

typedef struct {
    int pad_opens, pad_closes;
    int video_opens, video_closes;
    int video_reg, video_unreg;
    int dm_allocs, dm_releases;
    int ime_inits, ime_terms;
    int splash_hidden;
    int sysmod_freetype, sysmod_ime, sysmod_netctl;
    int netctl_init_calls;
    int netctl_without_sysmodule;
} panel_lifecycle_t;

typedef void (*panel_tick_fn)(const panel_host_snapshot_t *snap, OrbisPadData *pad_out, int *force_quit, void *ctx);

/* Server control */
int panel_host_server_start(void);
void panel_host_server_stop(void);
void panel_host_server_reset(void);
void panel_host_server_set_games(const char *json);
void panel_host_server_set_status(int scanning);
void panel_host_server_set_config(const char *json);
void panel_host_server_set_links(const char *json);
void panel_host_server_set_dests(const char *json);
void panel_host_server_set_icon_png(const uint8_t *png, size_t len);
void panel_host_server_set_icon_status(int status);
void panel_host_server_set_net_mode(int mode);
void panel_host_server_set_action_response(int status, const char *json);
void panel_host_server_set_port_override(int port); /* 0 = use mock server port */

/* Barrier on GET /api/games for deterministic concurrent-refresh-mid-hold tests. */
void panel_host_server_arm_games_barrier(void);
void panel_host_server_wait_games_arrived(void);
void panel_host_server_release_games_barrier(const char *next_games_json);

int panel_host_server_req_count(void);
int panel_host_server_req_get(int idx, panel_req_log_t *out);
int panel_host_server_has_req(const char *method, const char *path_prefix, const char *body_substr);

/* Fake hardware configuration */
void panel_host_reset_fakes(void);
void panel_host_set_language(int lang_code);
void panel_host_set_ip(const char *ip);
void panel_host_set_flip_delay(int ticks_pending);
void panel_host_set_ime_script(int init_rc, int frames_running, int endstatus, const wchar_t *text);
void panel_host_set_clock_ms(int64_t ms);
void panel_host_advance_clock_ms(int64_t delta_ms);

/* Run full panel_app_main() driven by tick callback. */
int panel_host_run(panel_tick_fn fn, void *ctx, panel_host_snapshot_t *final_snap, panel_metrics_t *out_metrics,
                   panel_lifecycle_t *out_life);

/* Helper: load a real PNG fixture (panel-app/Brand.png) for artwork tests. */
uint8_t *panel_host_load_png_fixture(const char *path, size_t *out_len);
