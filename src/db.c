/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Read-only lookups in the system databases. */
/* The system databases are written by the system alone: ShellCore keeps them open
 * with its own cache, and rows written from outside make it flag them as corrupted. */
#include "db.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "dbsnap.h"
#include "sqlite3.h"

#ifndef APP_DB
#define APP_DB "/system_data/priv/mms/app.db"
#endif

/* Titles registered in app.db, read in one query at most once a second. The file is
 * opened without locks (nolock=1): a shared lock held here, even for a moment, can make
 * a write of ShellCore fail, and ShellCore then flags the database as corrupted. */
typedef struct {
    char tid[10];
    char ver[8]; /* APP_VER the system registered ("" when it has none) */
} title_row_t;

static title_row_t *g_titles, *g_next;
static const title_row_t **g_index;
static int g_ntitles, g_cap, g_next_cap, g_index_cap, g_index_valid;
static time_t g_read_at;
static int g_read_ok, g_read_valid;
static int g_have_trusted, g_trusted_n, g_shrink_logged;
/* the panel thread and the scan ask: one reader at a time */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int title_index_cmp(const void *a, const void *b) {
    const title_row_t *aa = *(const title_row_t *const *)a;
    const title_row_t *bb = *(const title_row_t *const *)b;
    return strcmp(aa->tid, bb->tid);
}

/* The cached list is scanned once per title by scans and panel refreshes. Sort pointers to
 * it when it changes; if the small index cannot grow, the linear scan remains as fallback. */
static void index_titles(void) {
    g_index_valid = 0;
    if (g_ntitles > g_index_cap) {
        const title_row_t **grown = realloc(g_index, sizeof(*grown) * (size_t)g_ntitles);
        if (!grown)
            return;
        g_index = grown;
        g_index_cap = g_ntitles;
    }
    for (int i = 0; i < g_ntitles; i++)
        g_index[i] = &g_titles[i];
    qsort(g_index, (size_t)g_ntitles, sizeof(*g_index), title_index_cmp);
    g_index_valid = 1;
}

static void refresh(void) {
    if (g_read_ok && time(NULL) == g_read_at)
        return;
    g_read_at = time(NULL);
    g_read_valid = 0;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2("file:" APP_DB "?mode=ro&nolock=1", &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return; /* keep the last list */
    }
    sqlite3_stmt *st;
    int n = 0, ok = 0;
    /* the PS4 keeps one row per title and key in tbl_appinfo (the PS5 has tbl_contentinfo) */
    if (sqlite3_prepare_v2(db, "SELECT a.titleId, b.val FROM tbl_appinfo a LEFT JOIN tbl_appinfo b "
                               "ON b.titleId = a.titleId AND b.key = 'APP_VER' WHERE a.key = 'TITLE_ID'",
                           -1, &st, NULL) == SQLITE_OK) {
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(st, 0), *v = (const char *)sqlite3_column_text(st, 1);
            if (!t)
                continue;
            if (n == g_next_cap) {
                int cap = g_next_cap ? g_next_cap * 2 : 256;
                title_row_t *grown = realloc(g_next, sizeof(*grown) * (size_t)cap);
                if (!grown)
                    break;
                g_next = grown;
                g_next_cap = cap;
            }
            snprintf(g_next[n].tid, sizeof(g_next[n].tid), "%s", t);
            snprintf(g_next[n].ver, sizeof(g_next[n].ver), "%s", v ? v : "");
            n++;
        }
        ok = rc == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    if (!ok) {
        /* say which schema the database has: a firmware whose app.db keeps its rows in
         * another table makes every scan wait forever, and the reason belongs in the log */
        static int schema_logged;
        if (!schema_logged) {
            schema_logged = 1;
            sqlite3_stmt *ms = NULL;
            if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type = 'table' "
                                        "AND name IN ('tbl_appinfo','tbl_contentinfo')",
                                   -1, &ms, NULL) == SQLITE_OK) {
                char names[64] = "";
                while (sqlite3_step(ms) == SQLITE_ROW) {
                    const char *nm = (const char *)sqlite3_column_text(ms, 0);
                    if (nm) {
                        if (*names)
                            strncat(names, ",", sizeof(names) - strlen(names) - 1);
                        strncat(names, nm, sizeof(names) - strlen(names) - 1);
                    }
                }
                printf("[db] %s: title table query failed; tables found: %s\n",
                       APP_DB, *names ? names : "none of tbl_appinfo/tbl_contentinfo");
                sqlite3_finalize(ms);
            }
        }
    }
    sqlite3_close(db);
    if (!ok)
        return; /* a failed read keeps the last list and does not prove absence */
    struct stat jst;
    int journal = stat(APP_DB "-journal", &jst) == 0;
    /* SYS-1: a hot journal plus a smaller (or first) list is not the catalog. */
    if (!db_snapshot_publish(g_have_trusted, g_trusted_n, n, journal)) {
        if (!g_shrink_logged) {
            g_shrink_logged = 1;
            printf("[db] %s: journal present; not using a smaller or first title list as the catalog\n", APP_DB);
        }
        return;
    }
    g_shrink_logged = 0;
    title_row_t *prev = g_titles;
    int prev_cap = g_cap;
    g_titles = g_next;
    g_cap = g_next_cap;
    g_next = prev;
    g_next_cap = prev_cap;
    g_ntitles = n;
    g_trusted_n = n;
    g_have_trusted = 1;
    index_titles();
    g_read_ok = g_read_valid = 1;
}

static const title_row_t *find_title(const char *title_id) {
    if (g_index_valid) {
        int lo = 0, hi = g_ntitles;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            int cmp = strcmp(g_index[mid]->tid, title_id);
            if (cmp < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo < g_ntitles && !strcmp(g_index[lo]->tid, title_id))
            return g_index[lo];
        return NULL;
    }
    for (int i = 0; i < g_ntitles; i++)
        if (!strcmp(g_titles[i].tid, title_id))
            return &g_titles[i];
    return NULL;
}

int db_ready(void) {
    pthread_mutex_lock(&g_mu);
    refresh();
    int ok = g_read_ok;
    pthread_mutex_unlock(&g_mu);
    return ok;
}

int db_is_registered(const char *title_id) {
    return db_app_ver(title_id) != NULL;
}

int db_confirms_registered(const char *title_id) {
    pthread_mutex_lock(&g_mu);
    refresh();
    int present = g_read_valid && find_title(title_id) != NULL;
    pthread_mutex_unlock(&g_mu);
    return present;
}

int db_confirms_unregistered(const char *title_id) {
    pthread_mutex_lock(&g_mu);
    refresh();
    int absent = g_read_valid && !find_title(title_id);
    pthread_mutex_unlock(&g_mu);
    return absent;
}

const char *db_app_ver(const char *title_id) {
    static _Thread_local char ver_buf[8];
    pthread_mutex_lock(&g_mu);
    refresh();
    const title_row_t *row = find_title(title_id);
    int found = row != NULL;
    if (found)
        snprintf(ver_buf, sizeof(ver_buf), "%s", row->ver);
    pthread_mutex_unlock(&g_mu);
    return found ? ver_buf : NULL;
}
