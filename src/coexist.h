/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Whether the upstream PS4Mount port is installed or set to start with the console.
 * Lodestar began as a port of gasb07/PS4Mount and keeps that project's license. */
#pragma once

#include <stddef.h>

/* Ports and title ids of the two products. The 9032 lock is shared on purpose
 * (P4N-27). The panel and relay ports are not. */
enum {
    COEXIST_LOCK_PORT = 9032,
    COEXIST_PANEL_PORT = 8780,
    COEXIST_RELAY_PORT = 8781,
    COEXIST_KLOG_PORT = 3232,
    COEXIST_PS4MOUNT_PANEL_PORT = 8770,
    COEXIST_PS4MOUNT_RELAY_PORT = 8771
};

#define COEXIST_APP_ID "PSMT00001"
#define COEXIST_SHORTCUT_ID "PMNT00001"
#define COEXIST_PANEL_ID "LODS00001"

/* Shown on the console and, in the same words, in both web panels. Short enough
 * for the 256-byte notification buffer, and it says what to do. */
#define COEXIST_NOTICE_EN \
    "PS4Mount is installed or set to start with the console. Lodestar is not adding games on its own. " \
    "Disable PS4Mount in GoldHEN AutoRun, uninstall the PS4Mount app, then restart and load only Lodestar."
#define COEXIST_NOTICE_ES \
    "PS4Mount está instalado o arranca con la consola. Lodestar no añade juegos por su cuenta. " \
    "Desactiva PS4Mount en el AutoRun de GoldHEN, desinstala la app de PS4Mount, reinicia y carga solo Lodestar."

/* What a probe found. state_dir is recorded and does not by itself block. */
typedef struct {
    int payload_file;  /* a real ps4mount-ps4.bin or .elf (ELF or UH, at least 4 KiB) */
    int app_installed; /* the PSMT00001 app directory */
    int shortcut;      /* the PMNT00001 shortcut directory */
    int state_dir;     /* leftover /data/ps4mount or /system_data/ps4mount */
    int autorun;       /* an active payloads.ini line names that payload */
} coexist_evidence_t;

/* NULL-terminated path lists. A NULL list skips that check. */
typedef struct {
    const char *const *payloads;
    const char *const *apps;
    const char *const *shortcuts;
    const char *const *states;
    const char *const *inis;
} coexist_probe_paths_t;

/* Why title_state (src/main.c) would queue a title that is already in app.db.
 * The order is the order of that function's checks. */
typedef enum {
    COEXIST_WHY_NONE = 0,
    COEXIST_WHY_NOT_REGISTERED,
    COEXIST_WHY_BGFT_URL,
    COEXIST_WHY_BGFT_ERROR,
    COEXIST_WHY_MARKER,
    COEXIST_WHY_GAME_LINK,
    COEXIST_WHY_UPDATE_LINK,
    COEXIST_WHY_ADDONS
} coexist_why_t;

int coexist_ps4mount_installed(const coexist_evidence_t *e);
int coexist_block_auto_register(const coexist_evidence_t *e);

/* 1 when an active (not commented) line's basename is ps4mount-ps4.bin or .elf. */
int coexist_ini_has_ps4mount(const char *text, size_t len);

void coexist_probe(const coexist_probe_paths_t *paths, coexist_evidence_t *out);
const coexist_probe_paths_t *coexist_console_paths(void);

/* The same priority as title_state. has_patch 0 skips the update-link check. */
coexist_why_t coexist_reregister_why(int db_registered, int bgft_names_pkg, int bgft_error,
                                      int marker_matches, int game_link, int has_patch,
                                      int update_link, int addons_ready);

/* 0 when the guard is on, or when the title needs nothing. 1 when a scan would
 * register it (the behavior on main when PS4Mount is not in the way). */
int coexist_scan_registers(const coexist_evidence_t *e, coexist_why_t why);

/* An explicit add from the panel stays the user's action. Always 1. */
int coexist_manual_register_allowed(const coexist_evidence_t *e);

/* main.c's first-start wait: now >= max(start, seen) + wait. */
int coexist_setup_timeout_due(long start, long seen, long now, long wait);

/* 1 when that wait would save the defaults and the next scan could register.
 * The guard forces 0 while PS4Mount is installed or in AutoRun. */
int coexist_timeout_saves(int setup, long start, long seen, long now, long wait, int block);

/* ver < 2 forces organize on (config_load). Otherwise the file's value. */
int coexist_upgrade_organize(int ver, int file_organize);

/* ver < 3 and no dlc_mode key: dlc on becomes external. 1 means that mapping. */
int coexist_upgrade_forces_external(int ver, int dlc_mode_seen, int dlc_on);

/* first_start looks only at Lodestar's config and catalog or .src files. */
int coexist_first_start(int config_exists, int lodestar_catalog_or_src);

/* boot_time is wall minus monotonic seconds. 0 when the monotonic clock failed. */
long coexist_boot_time(long wall, long mono_sec);
int coexist_restart_mark_fresh(long mark, long boot);

/* main() does not refuse a second entry. 0: the current code is not safe. */
int coexist_same_process_reentry_safe(int log_already_running);

/* Lock-failure path calls log_stop. In another process that only stops the
 * loser. In this process it also stops the copy that is still running. */
int coexist_second_load_stops_log(int same_process);

/* organize off and not yet migrated: main still calls migrate_organized, and
 * the guard does not stop it. block is ignored, which is the open gap. */
int coexist_migrate_runs(int organize, int already_migrated, int block);

/* Lost the 9032 socket in rest and could not bind it again: main stops. */
int coexist_rest_lost_lock_stops(int still_listening, int rebind_ok);

/* The additive object on /api/games and /api/status. Writes a NUL-terminated
 * fragment, truncated if n is short. */
void coexist_format_json(int installed, int autorun, int state, int block, char *out, size_t n);
