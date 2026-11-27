/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "applog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int64_t mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void write_all(int fd, const char *buf, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        buf += n;
        len -= (size_t)n;
    }
}

static void ensure_parent_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path)
        return;
    size_t len = (size_t)(slash - path);
    if (len >= 256)
        return;
    char dir[256];
    memcpy(dir, path, len);
    dir[len] = 0;
    mkdir(dir, 0777);
}

static void open_log_fd(applog *log, int trunc) {
    if (!log->path[0])
        return;
    ensure_parent_dir(log->path);
    int flags = O_WRONLY | O_CREAT | (trunc ? O_TRUNC : O_APPEND);
    log->fd = open(log->path, flags, 0666);
    if (log->fd >= 0) {
        struct stat st;
        log->written = (!trunc && fstat(log->fd, &st) == 0 && st.st_size > 0) ? (size_t)st.st_size : 0;
    }
}

void applog_open(applog *log, const char *path, size_t max_bytes) {
    if (!log)
        return;
    memset(log, 0, sizeof(*log));
    log->fd = -1;
    log->max_bytes = max_bytes ? max_bytes : APPLOG_DEFAULT_MAX;
    log->start_ms = mono_ms();
    if (pthread_mutex_init(&log->lock, NULL) == 0)
        log->lock_ready = 1;
    if (path && *path) {
        snprintf(log->path, sizeof(log->path), "%s", path);
        snprintf(log->old_path, sizeof(log->old_path), "%s.old", log->path);
        open_log_fd(log, 0);
    }
}

void applog_close(applog *log) {
    if (!log)
        return;
    if (log->lock_ready)
        pthread_mutex_lock(&log->lock);
    if (log->fd >= 0) {
        close(log->fd);
        log->fd = -1;
    }
    if (log->lock_ready) {
        pthread_mutex_unlock(&log->lock);
        pthread_mutex_destroy(&log->lock);
        log->lock_ready = 0;
    }
}

static void rotate_locked(applog *log) {
    if (log->fd < 0 || !log->path[0] || !log->old_path[0])
        return;
    int old_fd = log->fd;
    if (rename(log->path, log->old_path) == 0) {
        close(old_fd);
        log->fd = -1;
        open_log_fd(log, 1);
    }
}

void applog_write(applog *log, const char *tag, const char *fmt, ...) {
    if (!log || !tag || !fmt)
        return;
    char body[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    /* Strip control chars so every entry stays one line. */
    for (char *p = body; *p; p++)
        if ((unsigned char)*p < 32)
            *p = ' ';

    int64_t elapsed = mono_ms() - log->start_ms;
    if (elapsed < 0)
        elapsed = 0;

    char line[APPLOG_LINE_CAP];
    int n = snprintf(line, sizeof(line), "+%lld.%03lld [%s] %s\n",
                     (long long)(elapsed / 1000), (long long)(elapsed % 1000), tag, body);
    if (n <= 0)
        return;
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    if (line[len - 1] != '\n')
        line[len - 1] = '\n';

    if (log->lock_ready)
        pthread_mutex_lock(&log->lock);

    memcpy(log->ring[log->ring_head], line, len);
    log->ring[log->ring_head][len] = 0;
    log->ring_head = (log->ring_head + 1) % APPLOG_RING_LINES;
    if (log->ring_count < APPLOG_RING_LINES)
        log->ring_count++;

    if (log->fd >= 0) {
        if (log->written >= log->max_bytes)
            rotate_locked(log);
        if (log->fd >= 0) {
            write_all(log->fd, line, len);
            log->written += len;
        }
    }

    if (log->lock_ready)
        pthread_mutex_unlock(&log->lock);
}

static int safe_query_key(const char *k, size_t len) {
    return (len == 8 && !strncasecmp(k, "title_id", 8)) ||
           (len == 4 && !strncasecmp(k, "dest", 4));
}

void applog_safe_path(const char *path, char *out, size_t cap) {
    if (!out || !cap)
        return;
    if (!path || !*path) {
        out[0] = 0;
        return;
    }
    /* Never log full URLs with schemes. */
    if (strstr(path, "://")) {
        snprintf(out, cap, "<redacted-url>");
        return;
    }
    const char *q = strchr(path, '?');
    size_t base_len = q ? (size_t)(q - path) : strlen(path);
    size_t pos = 0;
    if (base_len >= cap)
        base_len = cap - 1;
    memcpy(out, path, base_len);
    pos = base_len;
    out[pos] = 0;
    if (!q)
        return;

    const char *p = q + 1;
    int first = 1;
    while (*p && pos + 2 < cap) {
        const char *amp = strchr(p, '&');
        size_t part_len = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', part_len);
        size_t klen = eq ? (size_t)(eq - p) : part_len;
        int n;
        if (eq && safe_query_key(p, klen)) {
            const char *val = eq + 1;
            size_t vlen = part_len - klen - 1;
            /* Redact even safe keys if their value looks like a URL. */
            int looks_url = 0;
            for (size_t i = 0; i + 2 < vlen; i++) {
                if (val[i] == ':' && val[i + 1] == '/' && val[i + 2] == '/') {
                    looks_url = 1;
                    break;
                }
            }
            if (looks_url)
                n = snprintf(out + pos, cap - pos, "%c%.*s=<redacted>", first ? '?' : '&', (int)klen, p);
            else
                n = snprintf(out + pos, cap - pos, "%c%.*s=%.*s", first ? '?' : '&', (int)klen, p, (int)vlen, val);
        } else if (klen > 0) {
            n = snprintf(out + pos, cap - pos, "%c%.*s=<redacted>", first ? '?' : '&', (int)klen, p);
        } else {
            n = 0;
        }
        if (n < 0 || (size_t)n >= cap - pos)
            break;
        pos += (size_t)n;
        first = 0;
        if (!amp)
            break;
        p = amp + 1;
    }
}

size_t applog_ring_copy(applog *log, char *out, size_t cap) {
    if (!log || !out || !cap)
        return 0;
    if (log->lock_ready)
        pthread_mutex_lock(&log->lock);
    size_t start = (log->ring_head + APPLOG_RING_LINES - log->ring_count) % APPLOG_RING_LINES;
    size_t pos = 0;
    for (size_t i = 0; i < log->ring_count; i++) {
        const char *line = log->ring[(start + i) % APPLOG_RING_LINES];
        size_t len = strlen(line);
        if (pos + len >= cap)
            break;
        memcpy(out + pos, line, len);
        pos += len;
    }
    out[pos] = 0;
    if (log->lock_ready)
        pthread_mutex_unlock(&log->lock);
    return pos;
}
