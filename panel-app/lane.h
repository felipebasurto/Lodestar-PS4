/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "api.h"

/* The rules of the one-column library (PANEL-1, PANEL-3, PANEL-8), apart from drawing and the pad. */
enum section { SECTION_NEEDS, SECTION_ALL, SECTION_RUNNING, SECTIONS };
enum action { NONE, ADD, REMOVE, RESTORE };
enum { HOLD_MS = 700, WHEEL_ITEMS = 6 };

int lane_in_section(const game *g, enum section section);
int available_actions(const game *g, int online, enum action out[2]);
/* Position in indices (title order) after jumping to the previous (-1) or next (1) initial. */
int lane_jump(const library *catalog, const int *indices, int count, int at, int dir);
/* The wheel item a stick at (x, y), centre 0 and up negative, points at; -1 near the centre. */
int wheel_point(int x, int y);

/* A button being held to do something to one game. */
typedef struct {
    enum action action;
    char id[10];
    int64_t since;
} hold;

void hold_press(hold *h, enum action action, const game *focused, int64_t now);
/* focused: what the button is over now; down: the button is still held.
 * Returns the action once, when it is due; otherwise NONE. */
enum action hold_tick(hold *h, const game *focused, int online, int down, int64_t now);
/* How much of the hold has passed, in thousandths. */
int hold_progress(const hold *h, int64_t now);
