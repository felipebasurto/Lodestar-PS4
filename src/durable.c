/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Replace one file so a crash cannot leave a short prefix. SYS-2. */
#include "durable.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int (*durable_fail_after_temp)(void);

int durable_replace(const char *path, const void *data, size_t len, mode_t mode) {
    char tmp[PATH_MAX];
    if (!path || (!data && len))
        return -1;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.XXXXXX", path) >= (int)sizeof(tmp))
        return -1;
    int fd = mkstemp(tmp);
    if (fd < 0)
        return -1;
    const uint8_t *p = data;
    size_t left = len;
    int rc = 0;
    while (!rc && left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            rc = -1;
            break;
        }
        p += (size_t)n;
        left -= (size_t)n;
    }
    if (!rc) {
        mode_t use = mode & 0777;
        struct stat st;
        if (lstat(path, &st) == 0 && S_ISREG(st.st_mode))
            use = st.st_mode & 0777;
        if (fchmod(fd, use))
            rc = -1;
    }
    /* A filesystem that cannot fsync is not fatal; a failed fsync is. */
    if (!rc && fsync(fd) && errno != EINVAL && errno != ENOTSUP)
        rc = -1;
    if (close(fd))
        rc = -1;
    if (!rc && durable_fail_after_temp && durable_fail_after_temp())
        rc = -1;
    if (!rc && rename(tmp, path))
        rc = -1;
    if (rc)
        unlink(tmp);
    return rc;
}
