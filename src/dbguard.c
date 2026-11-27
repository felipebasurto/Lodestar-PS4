/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Database integrity guardrails, registration circuit breaker, and staging/unmount
 * recovery helpers (specs/registration.md REG-7..REG-9). */
#include "dbguard.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nlen) return 1;
    }
    return 0;
}

static void copy_clean(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r' || c == '\n' || c == '\t')
            dst[j++] = ' ';
        else if (c >= 0x20 && c < 0x7f)
            dst[j++] = (char)c;
    }
    while (j > 0 && dst[j - 1] == ' ') j--;
    dst[j] = '\0';
}

int dbguard_klog_corrupt(const char *line) {
    if (!line || !*line) return 0;
    /* ShellCore logs "DbCorruptionOccured" (sic) and occasionally "DbCorruptionOccurred". */
    if (contains_ci(line, "DbCorruptionOccur")) return 1;
    if (contains_ci(line, "app.db-corrupt")) return 1;
    if (contains_ci(line, "appinfo.db-corrupt")) return 1;
    if (contains_ci(line, "addcont.db-corrupt")) return 1;
    if (contains_ci(line, "no such table: tbl_appbrowse_")) return 1;
    if (contains_ci(line, "0x80f50061")) return 1;
    if (contains_ci(line, "0x809c1110")) return 1;
    return 0;
}

int dbguard_rc_is_db_error(int rc) {
    uint32_t u = (uint32_t)rc;
    if ((u & 0xFFFF0000u) == 0x80F50000u) return 1; /* SCE_LSDB_ERROR_* */
    if ((u & 0xFFFFFF00u) == 0x809C1100u) return 1; /* SCE_DBRECOVERY_ERROR_* */
    return 0;
}

void dbguard_init(dbguard_t *g, const char *mms_dir) {
    if (!g) return;
    memset(g, 0, sizeof(*g));
    pthread_mutex_init(&g->mu, NULL);
    g->inited = 1;
    const char *dir = (mms_dir && *mms_dir) ? mms_dir : DBGUARD_MMS_DIR;
    snprintf(g->mms_dir, sizeof(g->mms_dir), "%s", dir);
}

void dbguard_destroy(dbguard_t *g) {
    if (!g || !g->inited) return;
    pthread_mutex_destroy(&g->mu);
    g->inited = 0;
}

static void latch_locked(dbguard_t *g, const char *reason) {
    if (!g->corrupt) {
        g->corrupt = 1;
        g->corrupt_at = time(NULL);
        copy_clean(g->reason, sizeof(g->reason), reason);
    }
}

int dbguard_check_markers(dbguard_t *g) {
    if (!g) return 0;
    static const char *const markers[] = {
        "app.db-corrupt",
        "appinfo.db-corrupt",
        "addcont.db-corrupt",
    };
    char dir[sizeof(g->mms_dir)];
    if (g->inited) pthread_mutex_lock(&g->mu);
    snprintf(dir, sizeof(dir), "%s", g->mms_dir[0] ? g->mms_dir : DBGUARD_MMS_DIR);
    int already = g->corrupt;
    if (g->inited) pthread_mutex_unlock(&g->mu);
    if (already) return 1;

    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        char p[320];
        struct stat st;
        int n = snprintf(p, sizeof(p), "%s/%s", dir, markers[i]);
        if (n <= 0 || (size_t)n >= sizeof(p)) continue;
        if (stat(p, &st) == 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "marker present: %s", markers[i]);
            if (g->inited) pthread_mutex_lock(&g->mu);
            latch_locked(g, msg);
            if (g->inited) pthread_mutex_unlock(&g->mu);
            return 1;
        }
    }
    return 0;
}

int dbguard_observe_klog(dbguard_t *g, const char *line) {
    if (!g || !dbguard_klog_corrupt(line)) return 0;
    char msg[256];
    char clean[220];
    copy_clean(clean, sizeof(clean), line);
    snprintf(msg, sizeof(msg), "klog: %s", clean);
    if (g->inited) pthread_mutex_lock(&g->mu);
    latch_locked(g, msg);
    if (g->inited) pthread_mutex_unlock(&g->mu);
    return 1;
}

int dbguard_observe_rc(dbguard_t *g, int rc, const char *context) {
    if (!g || !dbguard_rc_is_db_error(rc)) return 0;
    char msg[256];
    snprintf(msg, sizeof(msg), "%s returned DB error 0x%08x",
             (context && *context) ? context : "installer", (unsigned)rc);
    if (g->inited) pthread_mutex_lock(&g->mu);
    latch_locked(g, msg);
    if (g->inited) pthread_mutex_unlock(&g->mu);
    return 1;
}

int dbguard_is_corrupt(dbguard_t *g) {
    if (!g) return 0;
    if (g->inited) pthread_mutex_lock(&g->mu);
    int v = g->corrupt;
    if (g->inited) pthread_mutex_unlock(&g->mu);
    return v;
}

int dbguard_snapshot(dbguard_t *g, char *reason_out, size_t cap) {
    if (reason_out && cap > 0) reason_out[0] = '\0';
    if (!g) return 0;
    if (g->inited) pthread_mutex_lock(&g->mu);
    int v = g->corrupt;
    if (v && reason_out && cap > 0) snprintf(reason_out, cap, "%s", g->reason);
    if (g->inited) pthread_mutex_unlock(&g->mu);
    return v;
}

int dbguard_take_warn(dbguard_t *g) {
    if (!g) return 0;
    int fire = 0;
    if (g->inited) pthread_mutex_lock(&g->mu);
    if (g->corrupt && !g->warned) {
        g->warned = 1;
        fire = 1;
    }
    if (g->inited) pthread_mutex_unlock(&g->mu);
    return fire;
}

void breaker_init(reg_breaker_t *b, int is_first_start, int migrated) {
    if (!b) return;
    memset(b, 0, sizeof(*b));
    if (is_first_start || migrated) {
        b->mode = BREAKER_PILOT_REQUIRED;
        snprintf(b->reason, sizeof(b->reason), "%s",
                 is_first_start ? "first start pilot" : "post-migration pilot");
    } else {
        b->mode = BREAKER_NORMAL;
    }
}

int breaker_batch_limit(const reg_breaker_t *b, int pending_count, int normal_batch_size,
                        int db_corrupt) {
    if (db_corrupt || pending_count <= 0) return 0;
    if (!b) {
        return (normal_batch_size > 0 && normal_batch_size < pending_count)
                   ? normal_batch_size
                   : pending_count;
    }
    switch (b->mode) {
    case BREAKER_TRIPPED:
    case BREAKER_PILOT_HOLD:
        return 0;
    case BREAKER_PILOT_REQUIRED:
        return 1;
    case BREAKER_NORMAL:
    default:
        return (normal_batch_size > 0 && normal_batch_size < pending_count)
                   ? normal_batch_size
                   : pending_count;
    }
}

void breaker_note_pilot(reg_breaker_t *b, const char *tid) {
    if (!b || !tid) return;
    snprintf(b->pilot_tid, sizeof(b->pilot_tid), "%s", tid);
}

int breaker_after_pilot(reg_breaker_t *b, int db_corrupt, int pilot_registered,
                        int unmask_ok, int remaining_pending) {
    if (!b) return -1;
    if (db_corrupt) {
        b->mode = BREAKER_TRIPPED;
        snprintf(b->reason, sizeof(b->reason), "system database flagged as corrupt");
        return -1;
    }
    if (!unmask_ok) {
        b->mode = BREAKER_TRIPPED;
        snprintf(b->reason, sizeof(b->reason), "/user/app unmask failed after pilot");
        return -1;
    }
    if (!pilot_registered) {
        b->mode = BREAKER_TRIPPED;
        snprintf(b->reason, sizeof(b->reason), "pilot title not confirmed in app.db");
        return -1;
    }
    b->pilot_ok = 1;
    if (remaining_pending > 0) {
        b->mode = BREAKER_PILOT_HOLD;
        snprintf(b->reason, sizeof(b->reason), "pilot validated; waiting for confirmation");
        return 1;
    }
    b->mode = BREAKER_NORMAL;
    b->reason[0] = '\0';
    return 0;
}

int breaker_resume(reg_breaker_t *b, int db_corrupt) {
    if (!b || db_corrupt) return 0;
    if (b->mode != BREAKER_PILOT_HOLD || !b->pilot_ok) return 0;
    b->mode = BREAKER_NORMAL;
    b->reason[0] = '\0';
    return 1;
}

int dbguard_unmask_user_app(const stage_unmask_ops_t *ops, char (*tids)[10], int n,
                            int *masked_flag) {
    if (!ops || !ops->is_masked || !ops->unmount_path) return -1;

    /* Unmount staged children under /user/app/<tid> first so /user/app is not busy. */
    for (int i = 0; i < n; i++) {
        if (!tids || !tids[i][0]) continue;
        char dst[64];
        snprintf(dst, sizeof(dst), "/user/app/%s", tids[i]);
        if (ops->unmount_path(ops->ctx, dst, 0) != 0) {
            if (!ops->child_mounted || ops->child_mounted(ops->ctx, tids[i]))
                ops->unmount_path(ops->ctx, dst, 1);
        }
    }

    /* Prioritize restoring the full /user/app view: gentle retries, then force. */
    for (int i = 0; i < 12 && ops->is_masked(ops->ctx); i++) {
        if (ops->unmount_path(ops->ctx, "/user/app", 0) == 0) break;
        if (ops->sleep_step) ops->sleep_step(ops->ctx, 250);
    }
    if (ops->is_masked(ops->ctx))
        ops->unmount_path(ops->ctx, "/user/app", 1);

    if (ops->is_masked(ops->ctx)) {
        if (masked_flag) *masked_flag = 1;
        return -1;
    }

    if (masked_flag) *masked_flag = 0;
    if (ops->stage_clear) ops->stage_clear(ops->ctx);
    return 0;
}

inst_poll_t dbguard_install_poll(int db_corrupt, int install_busy, int install_rc,
                                 int done, int total, int idle_sec) {
    if (db_corrupt) return INST_POLL_ABORT_DB_ERROR;
    if (!install_busy && dbguard_rc_is_db_error(install_rc))
        return INST_POLL_ABORT_DB_ERROR;
    if (done >= total) return INST_POLL_DONE;
    if (!install_busy && install_rc != 0) return INST_POLL_DONE;
    if (idle_sec >= 30) return INST_POLL_DONE;
    return INST_POLL_CONTINUE;
}
