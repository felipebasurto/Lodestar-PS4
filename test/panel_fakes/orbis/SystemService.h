/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

enum {
    ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG = 1,
};

int sceSystemServiceParamGetInt(int32_t paramId, int32_t *value);
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
int sceSystemServiceNavigateToGoHome(void);
