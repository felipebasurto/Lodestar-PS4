/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "lane.h"

#include <string.h>

int lane_in_section(const game *g, enum section section) {
    if (section == SECTION_RUNNING)
        return g->state == RUNNING;
    if (section == SECTION_NEEDS)
        return g->state == RESTART || g->state == PENDING || g->state == OFFLINE || g->state == DUPLICATE ||
               g->state == UNKNOWN;
    return 1;
}

int available_actions(const game *g, int online, enum action out[2]) {
    int count = 0;
    if (!g || !online)
        return 0;
    if (g->state == REMOVED)
        out[count++] = ADD;
    if (g->state == READY || g->state == RESTART)
        out[count++] = REMOVE;
    if (!g->in_place && (g->state == READY || g->state == RESTART || g->state == REMOVED || g->state == PENDING))
        out[count++] = RESTORE;
    return count;
}

/* The first byte of the title, capitals folded; bytes of other scripts as they are. */
static unsigned initial(const library *catalog, int index) {
    const game *g = &catalog->games[index];
    unsigned c = (unsigned char)*(*g->name ? g->name : g->id);
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

int lane_jump(const library *catalog, const int *indices, int count, int at, int dir) {
    if (!count)
        return 0;
    unsigned here = initial(catalog, indices[at]);
    if (dir > 0) {
        for (int i = at + 1; i < count; i++)
            if (initial(catalog, indices[i]) != here)
                return i;
        return at;
    }
    /* Backwards: the first of this initial, or of the one before when already there. */
    int i = at;
    if (i && initial(catalog, indices[i - 1]) != here)
        here = initial(catalog, indices[--i]);
    while (i && initial(catalog, indices[i - 1]) == here)
        i--;
    return i;
}

int wheel_point(int x, int y) {
    /* Six directions clockwise from the top, as thousandths. */
    static const int dir[WHEEL_ITEMS][2] = {{0, -1000}, {866, -500}, {866, 500}, {0, 1000}, {-866, 500}, {-866, -500}};
    if (x * x + y * y < 64 * 64)
        return -1;
    int best = 0;
    for (int i = 1; i < WHEEL_ITEMS; i++)
        if (x * dir[i][0] + y * dir[i][1] > x * dir[best][0] + y * dir[best][1])
            best = i;
    return best;
}

static int allowed(const game *g, int online, enum action action) {
    enum action out[2];
    for (int i = 0, n = available_actions(g, online, out); i < n; i++)
        if (out[i] == action)
            return 1;
    return 0;
}

void hold_press(hold *h, enum action action, const game *focused, int64_t now) {
    *h = (hold){.action = action, .since = now};
    memcpy(h->id, focused->id, sizeof(h->id));
}

enum action hold_tick(hold *h, const game *focused, int online, int down, int64_t now) {
    enum action action = h->action;
    if (!action)
        return NONE;
    if (!down || !focused || strcmp(h->id, focused->id) || !allowed(focused, online, action)) {
        h->action = NONE;
        return NONE;
    }
    if (now - h->since < HOLD_MS)
        return NONE;
    h->action = NONE;
    return action;
}

int hold_progress(const hold *h, int64_t now) {
    if (!h->action)
        return 0;
    int64_t passed = now - h->since;
    return passed >= HOLD_MS ? 1000 : (int)(passed * 1000 / HOLD_MS);
}
