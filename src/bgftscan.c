/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Scan-scoped BGFT snapshot: small canonical fields, not cached PDB files. */
#include "bgftscan.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mem.h"

void bgftscan_init(bgftscan_t *s) { memset(s, 0, sizeof(*s)); }

void bgftscan_free(bgftscan_t *s) {
    mem_free(s->entries);
    mem_free(s->used);
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

static int bgftscan_reserve(bgftscan_t *s, size_t need) {
    if (need <= s->capacity)
        return 1;
    if (need > SIZE_MAX / sizeof(*s->entries))
        return 0;
    size_t cap = s->capacity ? s->capacity : 16;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(*s->entries))
        return 0;
    bgftscan_entry_t *entries = mem_realloc(s->entries, cap * sizeof(*entries));
    if (!entries)
        return 0;
    s->entries = entries;
    s->capacity = cap;
    return 1;
}


static void mark_used(bgftscan_t *s, uint32_t id) {
    if (!s->used || id < s->used_first || id - s->used_first >= s->used_count)
        return;
    size_t bit = id - s->used_first;
    s->used[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void clear_used(bgftscan_t *s, uint32_t id) {
    if (!s->used || id < s->used_first || id - s->used_first >= s->used_count)
        return;
    size_t bit = id - s->used_first;
    s->used[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

int bgftscan_id_used(const bgftscan_t *s, uint32_t id) {
    if (!s->usable || !s->used || id < s->used_first || id - s->used_first >= s->used_count)
        return 0;
    size_t bit = id - s->used_first;
    return !!(s->used[bit / 8] & (uint8_t)(1u << (bit % 8)));
}

static int fill_entry(bgftscan_entry_t *x, uint32_t id, const uint8_t *data, size_t len) {
    bgft_info_t info;
    int parse_ok = bgft_task_parse(data, len, &info) == 0;
    int canonical = parse_ok && info.has_title;
    char legacy[BGFT_TITLE_ID_LEN + 1];
    int legacy_id = !canonical && bgft_legacy_title_id(data, len, legacy);
    if (legacy_id) {
        if (!parse_ok)
            memset(&info, 0, sizeof(info));
        info.has_title = 1;
        snprintf(info.title_id, sizeof(info.title_id), "%s", legacy);
    }
    if (!info.has_title)
        return 0;
    x->id = id;
    x->malformed = !canonical;
    x->error = canonical && info.error_present ? info.error : !canonical;
    x->info = info;
    return 1;
}

void bgftscan_remove_id(bgftscan_t *s, uint32_t id) {
    size_t out = 0;
    for (size_t i = 0; i < s->count; i++)
        if (s->entries[i].id != id)
            s->entries[out++] = s->entries[i];
    s->count = out;
    clear_used(s, id);
}

int bgftscan_update(bgftscan_t *s, uint32_t id, const uint8_t *data, size_t len) {
    if (!s->usable)
        return 0;
    bgftscan_entry_t x;
    if (!fill_entry(&x, id, data, len)) {
        s->usable = 0; /* bytes PS4Mount wrote should identify their title canonically */
        return 0;
    }
    for (size_t i = 0; i < s->count; i++)
        if (s->entries[i].id == id) {
            s->entries[i] = x;
            mark_used(s, id);
            return 1;
        }
    if (!bgftscan_reserve(s, s->count + 1)) {
        s->usable = 0;
        return 0;
    }
    s->entries[s->count++] = x;
    mark_used(s, id);
    return 1;
}

void bgftscan_load(bgftscan_t *s, const char *dir, uint32_t first, uint32_t last) {
    s->count = 0;
    s->usable = 0;
    mem_free(s->used);
    s->used = NULL;
    s->used_first = 0;
    s->used_count = 0;
    if (first > last)
        return;
    /* Allocation needs the even install id below each odd game id as well as the game id.
     * That occupancy fact is independent of whether the even directory is a game task. */
    uint32_t used_first = (first & 1) && first ? first - 1 : first;
    uint32_t used_count32 = last - used_first + 1;
    size_t used_bytes = ((size_t)used_count32 + 7) / 8;
    s->used = mem_alloc(used_bytes);
    if (!s->used)
        return;
    memset(s->used, 0, used_bytes);
    s->used_first = used_first;
    s->used_count = used_count32;
    DIR *d = opendir(dir);
    if (!d) {
        /* Only a proven absent directory is an empty snapshot. A failed open is unknown:
         * the caller falls back to fresh direct reads instead of inventing absence. */
        s->usable = errno == ENOENT || errno == ENOTDIR;
        return;
    }
    struct dirent *e;
    uint32_t id;
    char path[512];
    size_t n = 0;
    while ((e = readdir(d))) {
        if (!task_dir_id(e->d_name, used_first, last, &id))
            continue;
        mark_used(s, id);
        if (id < first)
            continue; /* install task of the first game id: occupancy only, never a game entry */
        int npath = snprintf(path, sizeof(path), "%s/%s/d0.pdb", dir, e->d_name);
        if (npath < 0 || (size_t)npath >= sizeof(path))
            continue;
        size_t len;
        uint8_t *data = read_task(path, &len);
        if (!data)
            continue;
        if (!bgftscan_reserve(s, n + 1)) {
            mem_free(data);
            closedir(d);
            s->count = 0;
            return; /* not usable: the caller falls back to fresh direct reads */
        }
        if (!fill_entry(&s->entries[n], id, data, len)) {
            mem_free(data);
            continue;
        }
        n++;
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
        if (!strcmp(s->entries[i].info.title_id, tid))
            return &s->entries[i];
    return NULL;
}

int bgftscan_names(const bgftscan_entry_t *e, const char *url) {
    return e && !e->malformed && bgft_info_url_is(&e->info, url);
}
