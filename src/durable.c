/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Atomic durable replacement for small state and metadata files (SYS-2). */
#include "durable.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int durable_replace(const char *path, const void *data, size_t len, mode_t mode) {
    if (!path || !*path || (len > 0 && !data)) {
        errno = EINVAL;
        return -1;
    }
    char tmp[PATH_MAX];
    int n = snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path);
    if (n <= 0 || (size_t)n >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    const uint8_t *p = (const uint8_t *)data;
    size_t left = len;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            int e = errno;
            close(fd);
            unlink(tmp);
            errno = e;
            return -1;
        }
        if (w == 0) {
            close(fd);
            unlink(tmp);
            errno = EIO;
            return -1;
        }
        p += (size_t)w;
        left -= (size_t)w;
    }
    if (fchmod(fd, mode) != 0) {
        int e = errno;
        close(fd);
        unlink(tmp);
        errno = e;
        return -1;
    }
    if (fsync(fd) != 0) {
        int e = errno;
        close(fd);
        unlink(tmp);
        errno = e;
        return -1;
    }
    if (close(fd) != 0) {
        int e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    if (rename(tmp, path) != 0) {
        int e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    return 0;
}
