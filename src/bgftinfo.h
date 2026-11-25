/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Strict parser for the small parts of a PS4 BGFT game task PS4Mount reads. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BGFT_TITLE_ID_LEN 9

typedef struct {
    int records;              /* records decoded, when valid */
    const uint8_t *title_id;  /* record 0x7c, normally 9 bytes */
    size_t title_id_len;
    const uint8_t *url;       /* record 0x1388, the game pkg path handed to BGFT */
    size_t url_len;
    uint32_t error;           /* record 0x68 when present */
    int error_present;
} bgft_info_t;

static inline uint32_t bgft_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* 1 when needle (nlen bytes) occurs in hay (hlen bytes). Unlike a C-string search this is
 * safe for PDB values, which are length-delimited and need not end in NUL. */
static inline int bgft_bytes_contains(const uint8_t *hay, size_t hlen, const uint8_t *needle, size_t nlen) {
    if (!nlen)
        return 1;
    if (nlen > hlen)
        return 0;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (!memcmp(hay + i, needle, nlen))
            return 1;
    return 0;
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
 * malformed: they only leave the matching field empty. */
static inline int bgft_task_parse(const uint8_t *d, size_t len, bgft_info_t *out) {
    memset(out, 0, sizeof(*out));
    if (!d || len < 4 || bgft_le32(d))
        return -1;
    size_t off = 4;
    int records = 0;
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
            out->title_id = data;
            out->title_id_len = n1;
        } else if (key == 0x1388) {
            out->url = data;
            out->url_len = n1;
        } else if (key == 0x68 && n1 == 4) {
            out->error = bgft_le32(data);
            out->error_present = 1;
        }
        off += n1;
        records++;
    }
    out->records = records;
    return 0;
}
