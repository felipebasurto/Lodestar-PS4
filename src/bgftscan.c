/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Scan-scoped BGFT snapshot: small canonical fields, not cached PDB files. */
#include "bgftscan.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bgftinfo.h"
#include "mem.h"

void bgftscan_init(bgftscan_t *s) { memset(s, 0, sizeof(*s)); }

void bgftscan_free(bgftscan_t *s) {
    mem_free(s->entries);
    bgftscan_init(s);
}

static int task_dir_id(const char *name, uint32_t first, uint32_t last, uint32_t *id) {
    char *end;
    unsigned long v = strtoul(name, &end, 16);
    if (*end || v < first || v > last)
        return 0;
    *id = (uint32_t)v;
    return 1;
}

/* Same 1 MiB cap as the daemon's task reads: a PDB larger than that is not a game task
 * PS4Mount is willing to trust. */
static uint8_t *read_task(const char *path, size_t *len) {
    struct stat st;
    *len = 0;
    if (stat(path, &st) || st.st_size <= 0 || st.st_size > (1 << 20))
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    uint8_t *d = mem_alloc((size_t)st.st_size);
    if (d && read(fd, d, (size_t)st.st_size) != st.st_size) {
        mem_free(d);
        d = NULL;
    }
    close(fd);
    if (d)
        *len = (size_t)st.st_size;
    return d;
}

void bgftscan_load(bgftscan_t *s, const char *dir, uint32_t first, uint32_t last) {
    s->count = 0;
    s->usable = 0;
    DIR *d = opendir(dir);
    if (!d) {
        s->usable = 1; /* the system has no task directory: a valid empty snapshot */
        return;
    }
    struct dirent *e;
    uint32_t id;
    size_t dirs = 0;
    while ((e = readdir(d)))
        dirs += task_dir_id(e->d_name, first, last, &id) != 0;
    rewinddir(d);
    if (!dirs) {
        closedir(d);
        s->usable = 1;
        return;
    }
    if (dirs > SIZE_MAX / sizeof(*s->entries)) {
        closedir(d);
        return;
    }
    bgftscan_entry_t *entries = mem_realloc(s->entries, dirs * sizeof(*entries));
    if (!entries) {
        closedir(d);
        return;
    }
    s->entries = entries;
    char path[512];
    size_t n = 0;
    while ((e = readdir(d))) {
        if (!task_dir_id(e->d_name, first, last, &id))
            continue;
        int npath = snprintf(path, sizeof(path), "%s/%s/d0.pdb", dir, e->d_name);
        if (npath < 0 || (size_t)npath >= sizeof(path))
            continue;
        size_t len;
        uint8_t *data = read_task(path, &len);
        if (!data)
            continue;
        bgft_info_t info;
        int ok = bgft_task_parse(data, len, &info) == 0;
        char tid[10] = "";
        if (ok && info.title_id && info.title_id_len == BGFT_TITLE_ID_LEN) {
            memcpy(tid, info.title_id, BGFT_TITLE_ID_LEN);
            tid[BGFT_TITLE_ID_LEN] = 0;
        } else {
            bgft_legacy_title_id(data, len, tid);
        }
        /* The directory is external mutable state. The first count is capacity, not a
         * proof that no task appeared since; writes remain bounded either way. */
        if (*tid && n < dirs) {
            bgftscan_entry_t *x = &s->entries[n++];
            memset(x, 0, sizeof(*x));
            x->id = id;
            x->malformed = !ok;
            x->error = ok && info.error_present ? info.error : !ok;
            snprintf(x->tid, sizeof(x->tid), "%s", tid);
            if (ok && info.url && info.url_len <= SYS_PATH_MAX) {
                memcpy(x->url, info.url, info.url_len);
                x->url_len = (uint8_t)info.url_len;
            } else if (ok && info.url) {
                x->malformed = 1;
                x->error = 1;
            }
        }
        mem_free(data);
    }
    closedir(d);
    s->count = n;
    s->usable = 1;
}

const bgftscan_entry_t *bgftscan_find(const bgftscan_t *s, const char *tid) {
    if (!s->usable)
        return NULL;
    for (size_t i = 0; i < s->count; i++)
        if (!strcmp(s->entries[i].tid, tid))
            return &s->entries[i];
    return NULL;
}

int bgftscan_names(const bgftscan_entry_t *e, const char *url) {
    return e && !e->malformed && e->url_len &&
           bgft_bytes_contains((const uint8_t *)e->url, e->url_len, (const uint8_t *)url, strlen(url));
}
