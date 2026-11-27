/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

enum {
    ORBIS_NET_CTL_INFO_IP_ADDRESS = 14,
};

typedef union {
    char ip_address[16];
} OrbisNetCtlInfo;

int sceNetCtlInit(void);
int sceNetCtlGetInfo(int code, OrbisNetCtlInfo *info);
void sceNetCtlTerm(void);
