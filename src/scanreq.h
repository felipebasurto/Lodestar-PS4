/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Scan requests as an explicit intent boundary (WEBAPI-4 in specs/webapi.md). This is only
 * the ordering machine: callers protect one instance with their mutex. A request made while
 * a scan runs must survive that scan's end-of-scan scheduling. */
#pragma once

#include <time.h>

typedef struct {
    time_t next;   /* 0: due now */
    unsigned seq;  /* changes with every request */
    int manual;    /* a not-yet-consumed "scan for games" request */
    int hard;      /* with manual: owner invalidates cached titles/update markers in that epoch */
} scanreq_t;

static inline void scanreq_init(scanreq_t *q) {
    q->next = 0;
    q->seq = 0;
    q->manual = 0;
    q->hard = 0;
}

static inline void scanreq_request(scanreq_t *q, int manual) {
    q->next = 0;
    q->seq++;
    if (manual)
        q->manual = 1;
}

static inline void scanreq_request_hard(scanreq_t *q) {
    scanreq_request(q, 1);
    q->hard = 1;
}

static inline int scanreq_due(const scanreq_t *q, time_t now) { return now >= q->next; }

/* Consumes the pending manual/hard bits for this scan epoch and returns the request generation. */
static inline unsigned scanreq_begin(scanreq_t *q, int *manual, int *hard) {
    *manual = q->manual;
    *hard = q->hard;
    q->manual = 0;
    q->hard = 0;
    return q->seq;
}

/* A scan that never ran (the system database was not readable) keeps its manual meaning for
 * the next scheduled epoch; it does not become an immediate retry loop. */
static inline void scanreq_keep_request(scanreq_t *q, int manual, int hard) {
    if (manual)
        q->manual = 1;
    if (hard)
        q->hard = 1;
}

/* Only the generation observed at the scan start may schedule the routine next scan. A newer
 * request leaves next at 0, so work it arrived too late for is scanned again at once. */
static inline void scanreq_finish(scanreq_t *q, unsigned seq, time_t now, int period) {
    if (q->seq == seq)
        q->next = now + period;
}
