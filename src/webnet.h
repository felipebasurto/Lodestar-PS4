/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Which IPv4 peers may make write requests to the panel. This boundary is pure so a host
 * test can check the exact network ranges; the panel has no password. */
#pragma once

#include <stdint.h>

/* a: IPv4 address in host byte order. Private LANs, loopback and link-local may write;
 * every public address is refused. */
static inline int web_net_ok(uint32_t a) {
    return (a >> 24) == 10 ||    /* 10.0.0.0/8 */
           (a >> 24) == 127 ||   /* loopback */
           (a >> 20) == 0xac1 || /* 172.16.0.0/12 */
           (a >> 16) == 0xc0a8 || /* 192.168.0.0/16 */
           (a >> 16) == 0xa9fe || /* 169.254.0.0/16 link-local */
           (a >> 22) == 0x191;    /* 100.64.0.0/10 carrier-grade NAT */
}
