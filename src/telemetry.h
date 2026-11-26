/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Compact key=value telemetry for scan epochs and /api/games (P4N-22 in specs/ps4-native.md). */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "scanreq.h"

typedef enum {
    FP_STAT_APP = 0,
    FP_STAT_PATCH,
    FP_STAT_BADPATCH,
    FP_STAT_AC,
} fp_stat_kind_t;

typedef struct {
    uint64_t app;
    uint64_t patch;
    uint64_t badpatch;
    uint64_t ac;
    uint64_t usb_stat;
    uint64_t usb_open;
    uint64_t usb_read;
    uint64_t usb_bytes;
    uint32_t cache_miss;
} fp_counts_t;

typedef struct {
    int active;
    uint32_t seq;
    scan_trigger_t trigger;
    int gameplay_running;
    uint64_t wall_ms;
    uint64_t usb_stat;
    uint64_t usb_open;
    uint64_t usb_read;
    uint64_t usb_bytes;
    uint64_t adopt_cand;
    uint64_t adopt_appledouble;
    fp_counts_t fp;
    uint64_t hold_ready;
    uint64_t bgft_dirs;
    uint64_t bgft_pdb_reads;
    uint64_t bgft_pdb_bytes;
    uint64_t mount_reads;
} scan_metrics_t;

typedef struct {
    uint64_t wall_ms;
    uint32_t titles;
    fp_counts_t fp;
} api_games_metrics_t;

static inline int is_usb_storage_path(const char *path) {
    return path && (!strncmp(path, "/mnt/usb", 8) || !strncmp(path, "/mnt/ext", 8));
}

static inline void fp_counts_note_stat(fp_counts_t *fc, fp_stat_kind_t kind, const char *path) {
    if (!fc)
        return;
    switch (kind) {
    case FP_STAT_APP:
        fc->app++;
        break;
    case FP_STAT_PATCH:
        fc->patch++;
        break;
    case FP_STAT_BADPATCH:
        fc->badpatch++;
        break;
    case FP_STAT_AC:
        fc->ac++;
        break;
    }
    if (path && path[0] && is_usb_storage_path(path))
        fc->usb_stat++;
}

static inline void fp_counts_note_usb_io(fp_counts_t *fc, const char *path, uint32_t read_ops, uint64_t bytes_read) {
    if (!fc || !is_usb_storage_path(path))
        return;
    fc->usb_open++;
    fc->usb_read += read_ops;
    fc->usb_bytes += bytes_read;
}

static inline int scan_metrics_format(const scan_metrics_t *m, char *out, size_t cap) {
    return snprintf(
        out, cap,
        "[scan] epoch seq=%u trigger=%s gameplay=%d wall_ms=%llu usb_stat=%llu usb_open=%llu usb_read=%llu "
        "usb_bytes=%llu adopt_cand=%llu adopt_appledouble=%llu fp_app=%llu fp_patch=%llu fp_badpatch=%llu "
        "fp_ac=%llu hold_ready=%llu bgft_dirs=%llu bgft_pdb_reads=%llu bgft_pdb_bytes=%llu mount_reads=%llu",
        m->seq, scanreq_trigger_name(m->trigger), m->gameplay_running ? 1 : 0, (unsigned long long)m->wall_ms,
        (unsigned long long)(m->usb_stat + m->fp.usb_stat), (unsigned long long)(m->usb_open + m->fp.usb_open),
        (unsigned long long)(m->usb_read + m->fp.usb_read), (unsigned long long)(m->usb_bytes + m->fp.usb_bytes),
        (unsigned long long)m->adopt_cand, (unsigned long long)m->adopt_appledouble, (unsigned long long)m->fp.app,
        (unsigned long long)m->fp.patch, (unsigned long long)m->fp.badpatch, (unsigned long long)m->fp.ac,
        (unsigned long long)m->hold_ready, (unsigned long long)m->bgft_dirs, (unsigned long long)m->bgft_pdb_reads,
        (unsigned long long)m->bgft_pdb_bytes, (unsigned long long)m->mount_reads);
}

static inline int api_games_metrics_format(const api_games_metrics_t *m, char *out, size_t cap) {
    return snprintf(out, cap,
                    "[api] games wall_ms=%llu titles=%u usb_stats=%llu fp_app=%llu fp_patch=%llu fp_ac=%llu "
                    "cache_miss=%u",
                    (unsigned long long)m->wall_ms, m->titles, (unsigned long long)m->fp.usb_stat,
                    (unsigned long long)m->fp.app, (unsigned long long)m->fp.patch, (unsigned long long)m->fp.ac,
                    m->fp.cache_miss);
}
