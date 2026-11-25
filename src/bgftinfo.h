/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Strict parser for the small parts of a PS4 BGFT game task PS4Mount reads. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BGFT_TITLE_ID_LEN 9
/* System pkg paths are at most 95 bytes. Longer records still expose their real length
 * and truncation state, but only the bytes that can ever be canonical are retained. */
#define BGFTINFO_URL_CAP 95

typedef struct {
    int records; /* records decoded; zero when the stream is malformed */
    int has_title;
    char title_id[BGFT_TITLE_ID_LEN + 1];
    int has_url;
    size_t url_len;      /* the record's real length, also when truncated */
    int url_truncated;
    char url[BGFTINFO_URL_CAP + 1];
    int error_present;
    uint32_t error;
} bgft_info_t;

static inline uint32_t bgft_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}


/* Tasks written by very old versions are only recognizable by the installed-path string
 * they carry. Extracts the title id in "/user/app/<TITLE_ID>/app.pkg" when that legacy
 * evidence is present; 0 otherwise. This is compatibility evidence, not a substitute
 * for canonical record 0x7c. */
static inline int bgft_legacy_title_id(const uint8_t *d, size_t len, char out[BGFT_TITLE_ID_LEN + 1]) {
    static const uint8_t prefix[] = "/user/app/";
    static const uint8_t suffix[] = "/app.pkg";
    for (size_t i = 0; i + sizeof(prefix) - 1 + BGFT_TITLE_ID_LEN + sizeof(suffix) - 1 <= len; i++) {
        if (memcmp(d + i, prefix, sizeof(prefix) - 1))
            continue;
        size_t t = i + sizeof(prefix) - 1;
        int ok = 1;
        for (size_t k = 0; k < BGFT_TITLE_ID_LEN; k++) {
            uint8_t c = d[t + k];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
                ok = 0;
                break;
            }
        }
        if (ok && !memcmp(d + t + BGFT_TITLE_ID_LEN, suffix, sizeof(suffix) - 1)) {
            memcpy(out, d + t, BGFT_TITLE_ID_LEN);
            out[BGFT_TITLE_ID_LEN] = 0;
            return 1;
        }
    }
    return 0;
}

/* A BGFT d0.pdb/d1.pdb is a little-endian u32 zero followed by records. Each record is
 * key, length, length again, then data, with no trailing bytes. Returns 0 and fills out
 * for a well-formed task, -1 for anything malformed. Missing individual keys are not
 * malformed: they only leave the matching field absent. Repeated keys use the last one. */
static inline int bgft_task_parse(const uint8_t *d, size_t len, bgft_info_t *out) {
    memset(out, 0, sizeof(*out));
    if (!d || len < 4 || bgft_le32(d))
        return -1;
    bgft_info_t tmp = {0};
    size_t off = 4;
    while (off < len) {
        if (len - off < 12)
            return -1;
        uint32_t key = bgft_le32(d + off);
        uint32_t n1 = bgft_le32(d + off + 4);
        uint32_t n2 = bgft_le32(d + off + 8);
        off += 12;
        if (n1 != n2 || n1 > len - off)
            return -1;
        const uint8_t *data = d + off;
        if (key == 0x7c) {
            if (n1 != BGFT_TITLE_ID_LEN)
                return -1; /* a canonical field of the wrong width is malformed, not absent */
            tmp.has_title = 1;
            memset(tmp.title_id, 0, sizeof(tmp.title_id));
            memcpy(tmp.title_id, data, BGFT_TITLE_ID_LEN);
        } else if (key == 0x1388) {
            size_t keep = n1 <= BGFTINFO_URL_CAP ? n1 : BGFTINFO_URL_CAP;
            tmp.has_url = 1;
            tmp.url_len = n1;
            tmp.url_truncated = n1 > BGFTINFO_URL_CAP;
            memcpy(tmp.url, data, keep);
            tmp.url[keep] = 0;
        } else if (key == 0x68) {
            if (n1 != 4)
                return -1;
            tmp.error_present = 1;
            tmp.error = bgft_le32(data);
        }
        off += n1;
        tmp.records++;
    }
    *out = tmp;
    return 0;
}

/* 1 when the canonical package URL record is exactly url. A longer record that merely
 * contains url is a different URL and cannot prove that the task is healthy. */
static inline int bgft_info_url_is(const bgft_info_t *info, const char *url) {
    if (!info || !url)
        return 0;
    size_t n = strlen(url);
    return info->has_url && !info->url_truncated && info->url_len == n && !memcmp(info->url, url, n);
}
