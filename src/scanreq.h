/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Scan requests as an explicit intent boundary (WEBAPI-4 in specs/webapi.md). This is only
 * the ordering machine: callers protect one instance with their mutex. A request made while
 * a scan runs must survive that scan's end-of-scan scheduling. */
#pragma once

#include <time.h>

typedef enum {
    SCAN_TRIG_ROUTINE = 0,
    SCAN_TRIG_STARTUP,
    SCAN_TRIG_MANUAL,
    SCAN_TRIG_HARD,
    SCAN_TRIG_UPLOAD,
    SCAN_TRIG_LINKS,
    SCAN_TRIG_CONFIG,
    SCAN_TRIG_ADD,
    SCAN_TRIG_RESTORE,
} scan_trigger_t;

static inline const char *scanreq_trigger_name(scan_trigger_t t) {
    switch (t) {
    case SCAN_TRIG_STARTUP:
        return "startup";
    case SCAN_TRIG_MANUAL:
        return "manual";
    case SCAN_TRIG_HARD:
        return "hard";
    case SCAN_TRIG_UPLOAD:
        return "upload";
    case SCAN_TRIG_LINKS:
        return "links";
    case SCAN_TRIG_CONFIG:
        return "config";
    case SCAN_TRIG_ADD:
        return "add";
    case SCAN_TRIG_RESTORE:
        return "restore";
    case SCAN_TRIG_ROUTINE:
    default:
        return "routine";
    }
}

typedef struct {
    time_t next;            /* 0: due now */
    unsigned seq;           /* changes with every request */
    int manual;             /* a not-yet-consumed "scan for games" request */
    int hard;               /* with manual: owner invalidates cached titles/update markers in that epoch */
    scan_trigger_t trigger; /* why the pending scan was requested (P4N-22) */
    int started;            /* 0 before the first scan epoch begins */
} scanreq_t;

static inline void scanreq_init(scanreq_t *q) {
    q->next = 0;
    q->seq = 0;
    q->manual = 0;
    q->hard = 0;
    q->trigger = SCAN_TRIG_STARTUP;
    q->started = 0;
}

static inline void scanreq_request_why(scanreq_t *q, int manual, scan_trigger_t trig) {
    q->next = 0;
    q->seq++;
    if (manual)
        q->manual = 1;
    if (q->trigger != SCAN_TRIG_HARD)
        q->trigger = trig;
}

static inline void scanreq_request(scanreq_t *q, int manual) {
    scanreq_request_why(q, manual, manual ? SCAN_TRIG_MANUAL : SCAN_TRIG_CONFIG);
}

static inline void scanreq_request_hard(scanreq_t *q) {
    scanreq_request_why(q, 1, SCAN_TRIG_HARD);
    q->hard = 1;
}

static inline int scanreq_due(const scanreq_t *q, time_t now) { return now >= q->next; }

/* Consumes the pending manual/hard bits and trigger reason for this scan epoch. */
static inline unsigned scanreq_begin_why(scanreq_t *q, int *manual, int *hard, scan_trigger_t *trig) {
    *manual = q->manual;
    *hard = q->hard;
    scan_trigger_t reason = q->trigger;
    if (reason == SCAN_TRIG_ROUTINE && !q->started)
        reason = SCAN_TRIG_STARTUP;
    if (trig)
        *trig = reason;
    q->manual = 0;
    q->hard = 0;
    q->trigger = SCAN_TRIG_ROUTINE;
    q->started = 1;
    return q->seq;
}

static inline unsigned scanreq_begin(scanreq_t *q, int *manual, int *hard) {
    return scanreq_begin_why(q, manual, hard, NULL);
}

/* A scan that never ran (the system database was not readable) keeps its manual meaning for
 * the next scheduled epoch; it does not become an immediate retry loop. */
static inline void scanreq_keep_request_why(scanreq_t *q, int manual, int hard, scan_trigger_t trig) {
    if (manual)
        q->manual = 1;
    if (hard)
        q->hard = 1;
    if (q->trigger == SCAN_TRIG_ROUTINE && trig != SCAN_TRIG_ROUTINE)
        q->trigger = trig;
}

static inline void scanreq_keep_request(scanreq_t *q, int manual, int hard) {
    scanreq_keep_request_why(q, manual, hard, hard ? SCAN_TRIG_HARD : manual ? SCAN_TRIG_MANUAL : SCAN_TRIG_ROUTINE);
}

/* Only the generation observed at the scan start may schedule the routine next scan. A newer
 * request leaves next at 0, so work it arrived too late for is scanned again at once. */
static inline void scanreq_finish(scanreq_t *q, unsigned seq, time_t now, int period) {
    if (q->seq == seq)
        q->next = now + period;
}

#define SCANREQ_IDLE_SECONDS 10
#define SCANREQ_GAMEPLAY_SECONDS 120

static inline int scanreq_period(int any_title_running) {
    return any_title_running ? SCANREQ_GAMEPLAY_SECONDS : SCANREQ_IDLE_SECONDS;
}
