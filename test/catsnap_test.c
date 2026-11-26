/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Snapshot layout and reference ownership. Run with: bash test/run.sh */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "catsnap.h"

static int failed, checks;
#define CHECK(x) do { checks++; if (!(x)) { failed++; printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

static catsnap_t *shared;

static void *ref_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        catsnap_ref(shared);
        const catsnap_game_t *g = catsnap_find(shared, "CUSA00001");
        if (!g)
            return (void *)1;
        catsnap_unref(shared);
    }
    return NULL;
}

int main(void) {
    catsnap_t *s = catsnap_alloc(1, 1, 32);
    CHECK(s != NULL);
    catsnap_game_t *g = catsnap_games(s);
    catsnap_dlc_t *d = catsnap_dlcs(s);
    char *str = catsnap_strings(s);
    snprintf(g->title_id, sizeof(g->title_id), "CUSA00001");
    g->app_off = 0;
    g->patch_off = CATSNAP_NO_PATH;
    g->first_dlc = 0;
    g->ndlc = 1;
    snprintf(str, 32, "/usb0/game.pkg");
    d->path_off = 0;
    snprintf(d->content_id, sizeof(d->content_id), "EP0001-CUSA00001_00-ADDON00000000000");
    CHECK(catsnap_ngames(s) == 1);
    CHECK(catsnap_find(s, "CUSA00001") == g);
    CHECK(!strcmp(catsnap_path(s, g->app_off), "/usb0/game.pkg"));
    CHECK(catsnap_path(s, g->patch_off)[0] == 0);
    CHECK(catsnap_find(s, "CUSA00002") == NULL);

    shared = s;
    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        CHECK(!pthread_create(&th[i], NULL, ref_worker, NULL));
    int bad = 0;
    for (int i = 0; i < 4; i++) {
        void *rc = NULL;
        pthread_join(th[i], &rc);
        bad |= rc != NULL;
    }
    CHECK(!bad);
    catsnap_unref(s); /* publisher slot: readers already dropped theirs */
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
