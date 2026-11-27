/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

enum {
    APPLOG_DEFAULT_MAX = 256 * 1024,
    APPLOG_RING_LINES = 64,
    APPLOG_LINE_CAP = 192,
};

typedef struct {
    pthread_mutex_t lock;
    int lock_ready;
    int fd;
    char path[256];
    char old_path[264];
    size_t max_bytes;
    size_t written;
    int64_t start_ms;
    char ring[APPLOG_RING_LINES][APPLOG_LINE_CAP];
    size_t ring_head;
    size_t ring_count;
} applog;

/* Opens a bounded rotated log at path (may be NULL for ring-only mode). Never fails fatally. */
void applog_open(applog *log, const char *path, size_t max_bytes);
void applog_close(applog *log);
void applog_write(applog *log, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* Redacts query parameters (keeping only safe identifiers like title_id and dest) and URLs. */
void applog_safe_path(const char *path, char *out, size_t cap);

/* Copies the in-memory ring lines (oldest to newest) into out, returning bytes written. */
size_t applog_ring_copy(applog *log, char *out, size_t cap);
