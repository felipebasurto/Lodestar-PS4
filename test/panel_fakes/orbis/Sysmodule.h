/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

enum {
    ORBIS_SYSMODULE_NET_CTL = 0x0009,
    ORBIS_SYSMODULE_IME_DIALOG = 0x0096,
    ORBIS_SYSMODULE_FREETYPE_OL = 0x009a,
};

int sceSysmoduleLoadModule(uint16_t id);
