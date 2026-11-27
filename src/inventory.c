/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Operational inventory persisted under /data/lodestar before registration
 * operations (specs/registration.md REG-10). Never reads or writes system DBs. */
#include "inventory.h"
#include "mem.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void sanitize_field(char *dst, size_t cap, const char *src) {
    if (!dst || cap == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\t' || c == '\r' || c == '\n')
            dst[j++] = ' ';
        else if (c >= 0x20 && c != 0x7f)
            dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

static int path_is_system_db(const char *path) {
    if (!path || !*path) return 1;
    /* Defense-in-depth: never allow inventory writes into the system DB folder. */
    if (strstr(path, "/priv/mms") != NULL) return 1;
    return 0;
}

inv_list_t *inv_alloc(void) {
    inv_list_t *list = mem_alloc(sizeof(*list));
    if (list) memset(list, 0, sizeof(*list));
    return list;
}

void inv_free(inv_list_t *list) {
    if (list) mem_free(list);
}

const inv_entry_t *inv_find(const inv_list_t *list, const char *title_id) {
    if (!list || !title_id || !*title_id) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].title_id, title_id) == 0)
            return &list->items[i];
    }
    return NULL;
}

int inv_upsert(inv_list_t *list, const inv_entry_t *entry) {
    if (!list || !entry || !entry->title_id[0]) return -1;
    inv_entry_t clean;
    memset(&clean, 0, sizeof(clean));
    sanitize_field(clean.title_id, sizeof(clean.title_id), entry->title_id);
    if (!clean.title_id[0]) return -1;
    sanitize_field(clean.source, sizeof(clean.source), entry->source);
    sanitize_field(clean.mount_mode, sizeof(clean.mount_mode),
                   entry->mount_mode[0] ? entry->mount_mode : "unknown");
    sanitize_field(clean.game_ver, sizeof(clean.game_ver), entry->game_ver);
    sanitize_field(clean.effective_ver, sizeof(clean.effective_ver), entry->effective_ver);
    clean.has_patch = entry->has_patch ? 1 : 0;
    clean.dlc_count = entry->dlc_count > 0 ? entry->dlc_count : 0;
    sanitize_field(clean.dlc_mode, sizeof(clean.dlc_mode),
                   entry->dlc_mode[0] ? entry->dlc_mode : "none");
    sanitize_field(clean.state, sizeof(clean.state),
                   entry->state[0] ? entry->state : "unknown");
    clean.updated_at = entry->updated_at;

    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].title_id, clean.title_id) == 0) {
            list->items[i] = clean;
            return 0;
        }
    }
    if (list->count >= INV_MAX) return -1;
    list->items[list->count++] = clean;
    return 0;
}

static char *next_tab(char **p) {
    if (!p || !*p) return NULL;
    char *start = *p;
    char *tab = strchr(start, '\t');
    if (tab) {
        *tab = '\0';
        *p = tab + 1;
    } else {
        *p = NULL;
    }
    return start;
}

int inv_load(inv_list_t *list, const char *path) {
    if (!list || !path || !*path) return -1;
    list->count = 0;
    FILE *f = fopen(path, "r");
    if (!f) return errno == ENOENT ? 0 : -1;

    char line[1536];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        char *p = line;
        char *f_tid = next_tab(&p);
        char *f_src = next_tab(&p);
        char *f_mode = next_tab(&p);
        char *f_gver = next_tab(&p);
        char *f_ever = next_tab(&p);
        char *f_patch = next_tab(&p);
        char *f_dlcn = next_tab(&p);
        char *f_dlcm = next_tab(&p);
        char *f_state = next_tab(&p);
        char *f_ts = next_tab(&p);
        if (!f_tid || !f_src || !f_mode || !f_state) continue;

        inv_entry_t e;
        memset(&e, 0, sizeof(e));
        snprintf(e.title_id, sizeof(e.title_id), "%s", f_tid);
        snprintf(e.source, sizeof(e.source), "%s", f_src);
        snprintf(e.mount_mode, sizeof(e.mount_mode), "%s", f_mode);
        if (f_gver) snprintf(e.game_ver, sizeof(e.game_ver), "%s", f_gver);
        if (f_ever) snprintf(e.effective_ver, sizeof(e.effective_ver), "%s", f_ever);
        if (f_patch) e.has_patch = atoi(f_patch) ? 1 : 0;
        if (f_dlcn) e.dlc_count = atoi(f_dlcn);
        if (f_dlcm) snprintf(e.dlc_mode, sizeof(e.dlc_mode), "%s", f_dlcm);
        snprintf(e.state, sizeof(e.state), "%s", f_state);
        if (f_ts) e.updated_at = strtoll(f_ts, NULL, 10);
        inv_upsert(list, &e);
    }
    fclose(f);
    return list->count;
}

int inv_save(const inv_list_t *list, const char *path) {
    if (!list || path_is_system_db(path)) return -1;
    char tmp[1100];
    int tn = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (tn <= 0 || (size_t)tn >= sizeof(tmp)) return -1;

    FILE *f = fopen(tmp, "w");
    if (!f) return -1;

    int ok = fprintf(f, "# lodestar-inventory v1\n"
                        "# title_id\tsource\tmount_mode\tgame_ver\teffective_ver\t"
                        "has_patch\tdlc_count\tdlc_mode\tstate\tupdated_at\n") > 0;
    for (int i = 0; ok && i < list->count; i++) {
        const inv_entry_t *e = &list->items[i];
        if (fprintf(f, "%s\t%s\t%s\t%s\t%s\t%d\t%d\t%s\t%s\t%lld\n",
                    e->title_id, e->source, e->mount_mode, e->game_ver, e->effective_ver,
                    e->has_patch ? 1 : 0, e->dlc_count, e->dlc_mode, e->state,
                    (long long)e->updated_at) <= 0)
            ok = 0;
    }
    if (fflush(f) != 0) ok = 0;
    if (ok) (void)fsync(fileno(f));
    if (fclose(f) != 0) ok = 0;

    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}
