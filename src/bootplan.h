/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Pure startup, first-run setup, panel-app launch, DB gating, /user/app staging and
 * batch-registration decisions. Extracted from src/main.c so host tests can execute the
 * real decision logic without the PS4 SDK or kernel. */
#pragma once

#include <stdint.h>
#include <time.h>

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

/* First-run setup timeout: expires wait_sec seconds after the later of startup and the
 * panel's last GET /api/games observation. */
static inline int boot_setup_timeout_expired(int setup_active, time_t now, time_t start, time_t last_seen,
                                             int wait_sec) {
    if (!setup_active)
        return 0;
    time_t base = last_seen > start ? last_seen : start;
    return now >= base + wait_sec;
}

/* Pure risk classifier: true when an expired setup timeout would auto-adopt pending games. */
static inline int boot_setup_would_auto_adopt(int setup_expired, int pending_games) {
    return setup_expired && pending_games > 0;
}

/* Pure foreground-user safety predicate: PS4 user IDs are positive non-zero values returned
 * when sceUserServiceGetForegroundUser succeeds (rc == 0). */
static inline int boot_fg_user_valid(int fg_rc, int32_t fg_user) {
    return fg_rc == 0 && fg_user > 0;
}

/* Current production policy for launching the panel app on first start: preserves main's
 * behavior verbatim (passes fg_user unchecked). fix/first-run-registration-safety tightens
 * this single decision point to require boot_fg_user_valid(fg_rc, fg_user). */
static inline int boot_panel_launch_user(int fg_rc, int32_t fg_user, uint32_t *out_user) {
    (void)fg_rc;
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

/* Pure decision table for panel_app_step() in src/main.c. */
static inline boot_panel_step_t boot_panel_app_plan(int have_installed, int done_marker, int setup_active,
                                                    int already_opened, int db_registered, int any_game_open,
                                                    int fetching_state, long elapsed_sec, int delay_sec,
                                                    int upload_active, int install_waiting) {
    boot_panel_step_t plan = {0, 0, 0, 0};
    plan.mark_done = have_installed && !done_marker;
    plan.set_opened = have_installed && setup_active && !already_opened && db_registered;
    plan.launch_app = plan.set_opened && !any_game_open;
    plan.start_fetch = !have_installed && !done_marker && !fetching_state && elapsed_sec >= delay_sec &&
                       !upload_active && !install_waiting;
    return plan;
}

/* Current production DB gate in scan_all(): checks only db_ready(). Marker/schema checks
 * are parameters here so fix/first-run-registration-safety can tighten one boundary. */
static inline int boot_db_usable(int db_ready, int db_corrupt_marker, int db_schema_consistent) {
    (void)db_corrupt_marker;
    (void)db_schema_consistent;
    return db_ready != 0;
}

/* Strict DB safety predicate for future corruption/schema guards. */
static inline int boot_db_safe_for_registration(int db_ready, int db_corrupt_marker, int db_schema_consistent) {
    return db_ready != 0 && !db_corrupt_marker && db_schema_consistent != 0;
}

typedef enum {
    BOOT_SCAN_SKIP_DELETING = 0,
    BOOT_SCAN_WAIT_SETUP,
    BOOT_SCAN_UNSUPPORTED_FW,
    BOOT_SCAN_RUN,
} boot_scan_gate_t;

/* Main-loop scan gate matching src/main.c. */
static inline boot_scan_gate_t boot_scan_gate(int deleting, int setup_active, int reg_mode) {
    if (deleting)
        return BOOT_SCAN_SKIP_DELETING;
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

/* Postcondition of unmask_user_app(): 1 only when /user/app is no longer masked. */
static inline int boot_stage_unmask_ok(int still_masked_after_force) {
    return !still_masked_after_force;
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

typedef enum {
    BOOT_REG_OK = 0,
    BOOT_REG_UNSUPPORTED,
    BOOT_REG_STUCK_MASKED,
    BOOT_REG_BUSY_TIMEOUT,
    BOOT_REG_FAILED,
    BOOT_REG_PARTIAL,
} boot_reg_outcome_t;

/* Classifies the observable result of an install_all() registration pass. */
static inline boot_reg_outcome_t boot_install_all_outcome(int reg_mode, int install_busy, int install_rc,
                                                          int registered, int total, int unmask_still_masked) {
    if (reg_mode == REG_NONE)
        return BOOT_REG_UNSUPPORTED;
    if (unmask_still_masked)
        return BOOT_REG_STUCK_MASKED;
    if (install_busy)
        return BOOT_REG_BUSY_TIMEOUT;
    if (install_rc != 0)
        return BOOT_REG_FAILED;
    if (registered < total)
        return BOOT_REG_PARTIAL;
    return BOOT_REG_OK;
}
