/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Database integrity guardrails, registration circuit breaker, and staging/unmount
 * recovery helpers (specs/registration.md REG-7..REG-9). The user check is in bootplan.h. */
#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define DBGUARD_MMS_DIR "/system_data/priv/mms"

/* Kernel-log line classifier (REG-7).
 * Returns 1 when a kernel-log line reports PS4 system database corruption
 * (DbCorruptionOccured/Occurred, *.db-corrupt, missing tbl_appbrowse_*, or
 * LSDB/DbRecovery corrupt error codes). */
int dbguard_klog_corrupt(const char *line);

/* Installer / AppInstUtil return-code classifier (REG-9).
 * Returns 1 when rc belongs to SCE_LSDB_ERROR_* (0x80F5xxxx) or
 * SCE_DBRECOVERY_ERROR_* (0x809C11xx). */
int dbguard_rc_is_db_error(int rc);

/* Thread-safe latched corruption state for the current daemon run (REG-7).
 * Never writes, deletes, renames, or repairs files in mms_dir. */
typedef struct {
    pthread_mutex_t mu;
    int inited;
    int corrupt;
    int warned;
    time_t corrupt_at;
    char mms_dir[256];
    char reason[256];
} dbguard_t;

void dbguard_init(dbguard_t *g, const char *mms_dir);
void dbguard_destroy(dbguard_t *g);

/* Read-only stat() check for app.db-corrupt, appinfo.db-corrupt, addcont.db-corrupt.
 * Latches corrupt state if any marker exists. Returns 1 if corrupt, 0 if clean. */
int dbguard_check_markers(dbguard_t *g);

/* Inspects one kernel log line and latches corrupt state on a corruption event.
 * Returns 1 if this line matched corruption, 0 otherwise. */
int dbguard_observe_klog(dbguard_t *g, const char *line);

/* Latches corruption from an explicit database-layer error code (e.g. AppInstallAll). */
int dbguard_observe_rc(dbguard_t *g, int rc, const char *context);

/* Returns 1 if corruption has been latched. */
int dbguard_is_corrupt(dbguard_t *g);

/* Thread-safe snapshot for /api/status and /api/games. Returns 1 if corrupt and
 * writes reason (NUL-terminated) into reason_out when cap > 0. */
int dbguard_snapshot(dbguard_t *g, char *reason_out, size_t cap);

/* Returns 1 exactly once after corruption is latched so notices/logs fire once. */
int dbguard_take_warn(dbguard_t *g);

/* First-start & post-migration registration circuit breaker (REG-8). */
typedef enum {
    BREAKER_NORMAL = 0,
    BREAKER_PILOT_REQUIRED = 1,
    BREAKER_PILOT_HOLD = 2,
    BREAKER_TRIPPED = 3
} breaker_mode_t;

typedef struct {
    breaker_mode_t mode;
    int pilot_ok;
    char pilot_tid[10];
    char reason[128];
} reg_breaker_t;

void breaker_init(reg_breaker_t *b, int is_first_start, int migrated);

/* Maximum number of titles allowed in the next registration batch.
 * Returns 0 when db_corrupt is set, breaker is tripped, or pilot hold is active.
 * Returns at most 1 when BREAKER_PILOT_REQUIRED. */
int breaker_batch_limit(const reg_breaker_t *b, int pending_count, int normal_batch_size,
                        int db_corrupt);

/* Records the pilot title chosen for validation. */
void breaker_note_pilot(reg_breaker_t *b, const char *tid);

/* Validates the pilot title after a pilot registration attempt.
 * Returns:
 *   -1 if validation failed (breaker transitions to BREAKER_TRIPPED),
 *    1 if pilot succeeded and remaining titles are held for explicit continuation,
 *    0 if pilot succeeded and no more pending titles remain (returns to BREAKER_NORMAL). */
int breaker_after_pilot(reg_breaker_t *b, int db_corrupt, int pilot_registered,
                        int unmask_ok, int remaining_pending);

/* Explicit operator continuation (manual scan / explicit add) after a passing pilot.
 * Returns 1 if resumed to BREAKER_NORMAL, 0 if refused (corrupt, tripped, or not held). */
int breaker_resume(reg_breaker_t *b, int db_corrupt);

/* Staging / /user/app unmask recovery (REG-9). */
typedef struct {
    void *ctx;
    int (*is_masked)(void *ctx);
    int (*child_mounted)(void *ctx, const char *tid);
    int (*unmount_path)(void *ctx, const char *path, int force);
    void (*sleep_step)(void *ctx, int ms);
    void (*stage_clear)(void *ctx);
} stage_unmask_ops_t;

/* Restores the full /user/app view by unmounting staged children and /user/app
 * (gentle retries followed by force). Only clears *masked_flag and STAGE_DIR when
 * /user/app is proven unmasked; otherwise leaves *masked_flag = 1 and returns -1. */
int dbguard_unmask_user_app(const stage_unmask_ops_t *ops, char (*tids)[10], int n,
                            int *masked_flag);

typedef enum {
    INST_POLL_CONTINUE = 0,
    INST_POLL_DONE = 1,
    INST_POLL_ABORT_DB_ERROR = 2
} inst_poll_t;

/* Decision for one iteration of the AppInstallAll wait loop (REG-7 / REG-9). */
inst_poll_t dbguard_install_poll(int db_corrupt, int install_busy, int install_rc,
                                 int done, int total, int idle_sec);
