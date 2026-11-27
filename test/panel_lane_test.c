/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PANEL-8: removing and restoring need the whole hold, over the same game, once. */
#include "../panel-app/lane.h"

#include <assert.h>
#include <stdio.h>

static const game ready = {.id = "DEM000001", .state = READY};
static const game other = {.id = "DEM000002", .state = READY};
static const game running = {.id = "DEM000001", .state = RUNNING};

int main(void) {
    hold h;

    /* Released early: nothing, and the same press cannot finish later. */
    hold_press(&h, REMOVE, &ready, 1000);
    assert(hold_tick(&h, &ready, 1, 1, 1000 + HOLD_MS - 1) == NONE);
    assert(hold_tick(&h, &ready, 1, 0, 1000 + HOLD_MS - 1) == NONE);
    assert(hold_tick(&h, &ready, 1, 1, 1000 + 5 * HOLD_MS) == NONE);

    /* Held: once, however long the button stays down. */
    hold_press(&h, REMOVE, &ready, 1000);
    assert(hold_tick(&h, &ready, 1, 1, 1000 + HOLD_MS) == REMOVE);
    assert(hold_tick(&h, &ready, 1, 1, 1000 + 3 * HOLD_MS) == NONE);

    /* What happens mid-hold, then the original game again: the hold stays dropped. */
    const struct {
        const char *what;
        const game *focused;
        int online;
    } interrupted[] = {
        {"a refresh moved another game under the button", &other, 1},
        {"the library is empty", NULL, 1},
        {"the game started", &running, 1},
        {"the connection was lost", &ready, 0},
    };
    for (size_t i = 0; i < sizeof(interrupted) / sizeof(*interrupted); i++) {
        hold_press(&h, RESTORE, &ready, 1000);
        enum action during = hold_tick(&h, interrupted[i].focused, interrupted[i].online, 1, 1100);
        enum action after = hold_tick(&h, &ready, 1, 1, 1000 + HOLD_MS);
        if (during != NONE || after != NONE) {
            fprintf(stderr, "acted although %s\n", interrupted[i].what);
            return 1;
        }
    }

    puts("PANEL-8 a hold acts only on the same game, in full, once");
    return 0;
}
