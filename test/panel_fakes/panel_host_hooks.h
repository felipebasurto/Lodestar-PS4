/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

typedef struct {
    int page;
    int section;
    int selected;
    int visible_count;
    int wheel;
    int wheel_at;
    int held_action;
    int online;
    int spanish;
    int ime;
    int config_ready;
    int links_ready;
    int dest_ready;
    int dest;
    int link_writing;
    int active_task;
    int pending_task;
    int dirty;
    int quit;
    int art_cached;
    int category;
    int in_sidebar;
    int setting_row;
    int catalog_count;
    int setup;
    char focused_id[10];
    char query[256];
    char feedback[384];
    char entered_url[256];
    char local_url[48];
} panel_host_snapshot_t;

int64_t panel_host_clock_ms(void);
int panel_host_usleep(unsigned usec);
void panel_host_set_in_loop(int in_loop);
void panel_host_on_worker_reply(void);
void panel_host_on_loop_tick(const panel_host_snapshot_t *snap);
int panel_host_should_force_quit(void);

/* Exposed by panel-app/main.c when compiled with -DPANEL_HOST_TEST. */
int panel_app_main(void);
void panel_host_reset_app_globals(void);
void panel_host_wait_worker_reply(void);
