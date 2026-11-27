/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Pure startup, first-run setup, panel-app launch, DB gating, /user/app staging and
 * batch-registration decisions. Extracted from src/main.c so host tests can execute the
 * real decision logic without the PS4 SDK or kernel. */
#pragma once

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "fw.h"

typedef enum {
    BOOT_LOCK_ACQUIRED = 0,
    BOOT_LOCK_CONFLICT,
} boot_lock_state_t;

/* Shared loopback interlock (LOCK_PORT 9032, P4N-27): any other mount daemon holding the
 * port causes startup to fail closed without touching mounts or state. */
static inline boot_lock_state_t boot_lock_state(int lock_fd) {
    return lock_fd >= 0 ? BOOT_LOCK_ACQUIRED : BOOT_LOCK_CONFLICT;
}

/* First start: neither config.txt nor any prior catalog/source marker exists in STATE_DIR. */
static inline int boot_first_start(int has_config, int has_catalog_or_src) {
    return !has_config && !has_catalog_or_src;
}

/* The user a launch or a user-scoped install may name. The system's pseudo users end at
 * 0xFF (0xFE everyone, 0xFF system) and a real one is far above: user 0, -1 or a failed
 * sceUserServiceGetForegroundUser make ShellCore read tbl_appbrowse_0000000000, which
 * does not exist, and flag app.db as corrupt (REG-6, P4N-28). */
static inline int boot_fg_user_valid(int fg_rc, int32_t fg_user) {
    return fg_rc == 0 && fg_user > 0xFF;
}

/* Writes the user to launch or install for and returns 1 only when it is valid. */
static inline int boot_launch_user(int fg_rc, int32_t fg_user, uint32_t *out_user) {
    if (!boot_fg_user_valid(fg_rc, fg_user))
        return 0;
    if (out_user)
        *out_user = (uint32_t)fg_user;
    return 1;
}

typedef struct {
    int mark_done;
    int set_opened;
    int launch_app;
    int start_fetch;
} boot_panel_step_t;

/* Pure decision table for panel_app_step() in src/main.c. any_app_open is nonzero when an
 * app is open or the process table could not be read (fail closed). */
static inline boot_panel_step_t boot_panel_app_plan(int have_installed, int done_marker, int setup_active,
                                                    int already_opened, int db_registered, int any_app_open,
                                                    int fetching_state, long elapsed_sec, int delay_sec,
                                                    int upload_active, int install_waiting) {
    boot_panel_step_t plan = {0, 0, 0, 0};
    plan.mark_done = have_installed && !done_marker;
    plan.set_opened = have_installed && setup_active && !already_opened && db_registered;
    plan.launch_app = plan.set_opened && !any_app_open;
    plan.start_fetch = !have_installed && !done_marker && !fetching_state && elapsed_sec >= delay_sec &&
                       !upload_active && !install_waiting;
    return plan;
}

typedef enum {
    BOOT_SCAN_SKIP_DELETING = 0,
    BOOT_SCAN_HALT_DB_CORRUPT,
    BOOT_SCAN_WAIT_SETUP,
    BOOT_SCAN_UNSUPPORTED_FW,
    BOOT_SCAN_RUN,
} boot_scan_gate_t;

/* Main-loop scan gate. A first start waits for the explicit save (REG-8): no timeout and no
 * panel absence ever opens it. A database the system flagged as damaged (REG-7) stops every
 * registration whatever the setup says: with it every game would look unregistered. */
static inline boot_scan_gate_t boot_scan_gate(int deleting, int setup_active, int reg_mode, int db_corrupt) {
    if (deleting)
        return BOOT_SCAN_SKIP_DELETING;
    if (db_corrupt)
        return BOOT_SCAN_HALT_DB_CORRUPT;
    if (setup_active)
        return BOOT_SCAN_WAIT_SETUP;
    if (reg_mode == REG_NONE)
        return BOOT_SCAN_UNSUPPORTED_FW;
    return BOOT_SCAN_RUN;
}

/* /user/app staging mask gate: only used for AppInstallAll when no game holds /user/app
 * open and at least one title actually needs registration. */
static inline int boot_stage_mask_allowed(int reg_mode, int open_games_count, int need_count) {
    return reg_mode == REG_ALL && open_games_count == 0 && need_count > 0;
}

/* Group sizing for scan_root() registration batches (0, 1, 20, 768... pending games). */
static inline int boot_batch_plan(int pending, int batch_enabled, int batch_size, int *out_group_size,
                                  int *out_groups) {
    if (pending <= 0) {
        if (out_group_size)
            *out_group_size = 0;
        if (out_groups)
            *out_groups = 0;
        return 0;
    }
    int size = (batch_enabled && batch_size > 0) ? batch_size : pending;
    int groups = (pending + size - 1) / size;
    if (out_group_size)
        *out_group_size = size;
    if (out_groups)
        *out_groups = groups;
    return groups;
}

/* Who holds the lock port (P4N-31): bind_errno of the failed bind and what the local panel
 * answered to GET /api/agent. */
typedef enum {
    LOCK_CONFLICT_LODESTAR_RUNNING = 0,
    LOCK_CONFLICT_OTHER_OR_UNKNOWN = 1,
    LOCK_CONFLICT_SYS_ERROR = 2
} lock_conflict_t;

static inline lock_conflict_t lock_conflict_classify(int bind_errno, const char *agent_reply) {
    if (bind_errno != EADDRINUSE)
        return LOCK_CONFLICT_SYS_ERROR;
    if (agent_reply && strstr(agent_reply, "\"product\":\"Lodestar\""))
        return LOCK_CONFLICT_LODESTAR_RUNNING;
    return LOCK_CONFLICT_OTHER_OR_UNKNOWN;
}
