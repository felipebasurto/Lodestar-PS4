/* SPDX-License-Identifier: GPL-3.0-or-later
 * Host-testable fail-closed decisions for first-run consent, panel auto-launch,
 * and instance lock conflict classification (P4N-28, P4N-29, P4N-31). */
#pragma once

#include <errno.h>
#include <stdint.h>
#include <string.h>

/* Reserved PS4 UserService IDs (OpenOrbis / libSceUserService):
 *   <= 0  : 0 (no user, triggers tbl_appbrowse_0000000000 in app.db), -1 (INVALID),
 *           and any negative value / error code
 *   0x01..0xFF : reserved range including 0xFE (EVERYONE) and 0xFF (SYSTEM)
 * Real local PS4 user IDs are positive 32-bit integers above 0xFF (typically >= 0x10000000). */
static inline int user_id_valid(int32_t user_id) {
    return user_id > 0xFF;
}

typedef enum {
    PANEL_LAUNCH_WAIT = 0,
    PANEL_LAUNCH_SKIP_BUSY = 1,
    PANEL_LAUNCH_SKIP_NO_USER = 2,
    PANEL_LAUNCH_OPEN = 3
} panel_launch_action_t;

/* Decides whether the first-run loop may call sceLncUtilLaunchApp for LODS00001.
 * Fails closed on any open game, already-open panel app, unreadable process table
 * (< 0), unconfirmed app.db registration, UserService error, or invalid user ID. */
static inline panel_launch_action_t panel_launch_decide(int have_app, int setup_active, int already_tried,
                                                        int app_registered, int open_games_rc, int open_panel_rc,
                                                        int fg_user_rc, int32_t fg_user_id) {
    if (!have_app || !setup_active || already_tried || !app_registered)
        return PANEL_LAUNCH_WAIT;
    if (open_games_rc != 0 || open_panel_rc != 0)
        return PANEL_LAUNCH_SKIP_BUSY;
    if (fg_user_rc != 0 || !user_id_valid(fg_user_id))
        return PANEL_LAUNCH_SKIP_NO_USER;
    return PANEL_LAUNCH_OPEN;
}

/* First-run consent state machine (P4N-29).
 * Initial library adoption requires explicit settings confirmation (POST /api/config).
 * No UI timeout, panel absence, panel close, or app crash may auto-save settings or
 * trigger mass registration of preexisting games. Once settings are saved (or on an
 * existing installation), subsequent scans run normally for newly added packages. */
typedef enum {
    FIRSTRUN_AWAITING_CONSENT = 0,
    FIRSTRUN_READY = 1
} firstrun_phase_t;

typedef enum {
    FIRSTRUN_SCAN_BLOCK = 0,
    FIRSTRUN_SCAN_ALLOW = 1
} firstrun_scan_action_t;

typedef struct {
    firstrun_phase_t phase;
    int panel_seen;
    int64_t started_at;
    int64_t last_seen_at;
} firstrun_state_t;

static inline void firstrun_init(firstrun_state_t *s, int is_first_start, int64_t now) {
    s->phase = is_first_start ? FIRSTRUN_AWAITING_CONSENT : FIRSTRUN_READY;
    s->panel_seen = 0;
    s->started_at = now;
    s->last_seen_at = 0;
}

static inline void firstrun_on_panel_seen(firstrun_state_t *s, int64_t now) {
    s->panel_seen = 1;
    s->last_seen_at = now;
}

static inline void firstrun_on_panel_closed(firstrun_state_t *s) {
    (void)s;
}

static inline void firstrun_on_settings_saved(firstrun_state_t *s) {
    s->phase = FIRSTRUN_READY;
}

static inline int firstrun_awaiting_consent(const firstrun_state_t *s) {
    return s->phase == FIRSTRUN_AWAITING_CONSENT;
}

static inline firstrun_scan_action_t firstrun_scan_decision(const firstrun_state_t *s, int64_t now,
                                                            int preexisting_titles) {
    (void)now;
    (void)preexisting_titles;
    return s->phase == FIRSTRUN_READY ? FIRSTRUN_SCAN_ALLOW : FIRSTRUN_SCAN_BLOCK;
}

/* Instance lock conflict classification (P4N-31). */
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
