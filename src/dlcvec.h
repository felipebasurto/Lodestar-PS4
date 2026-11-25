/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Bounded growth decisions for registration-time add-on arrays. */
#pragma once

/* The first capacity is ten because the current registration-time descriptor stride
 * (1600 bytes) still fits ten slots in one 16 KiB mapped page, header included. Growth
 * doubles from there and never passes max. Returns -1 when count is already at max. */
static inline int dlcvec_next_cap(int count, int cap, int max) {
    if (count < cap)
        return cap;
    if (count >= max)
        return -1;
    int n = cap ? cap * 2 : 10;
    return n > max ? max : n;
}
