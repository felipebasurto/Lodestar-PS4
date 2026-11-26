/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Fuzz target: pkg header and entry table reader, and pkg_check (any file on a USB drive,
 * and the start of a pkg sent to the panel). */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "check.h"
#include "mem.h"
#include "pkg.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static pkg_t pkg;
#ifdef __linux__
    int fd = memfd_create("pkg", 0);
#else
    char tmp[] = "/tmp/lodestar-fuzz-pkg-XXXXXX";
    int fd = mkstemp(tmp);
    if (fd >= 0)
        unlink(tmp);
#endif
    if (fd < 0)
        return 0;
    if (size && write(fd, data, size) != (ssize_t)size) {
        close(fd);
        return 0;
    }
    char path[64];
#ifdef __linux__
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
#else
    snprintf(path, sizeof(path), "/dev/fd/%d", fd);
#endif
    if (!pkg_open(&pkg, path)) {
        static const uint32_t ids[] = {0x1000, 0x1200, 0x0400, 0x0401, 0x0402, 0x0403, 0x1001};
        for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
            size_t len;
            uint8_t *e = pkg_entry(&pkg, ids[i], &len);
            if (e && ids[i] == 0x1000) {
                static sfo_t sfo;
                sfo_parse(e, len, &sfo);
            }
            mem_free(e);
        }
        pkg_close(&pkg);
    }
    pkg_check_t c;
    pkg_check(path, 0x13520001, &c);
    close(fd);
    return 0;
}
