/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

enum {
    ORBIS_KERNEL_PRIO_FIFO_LOWEST = 767,
};

typedef struct {
    int32_t priority;
} OrbisUserServiceInitializeParams;

int sceUserServiceInitialize(const OrbisUserServiceInitializeParams *params);
int sceUserServiceGetInitialUser(int32_t *userId);
