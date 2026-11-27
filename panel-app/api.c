/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "api.h"
#include "../src/json.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifdef PANEL_HOST_TEST
extern int panel_host_api_port(void);
#define API_CONNECT_PORT panel_host_api_port()
#else
#define API_CONNECT_PORT API_PORT
#endif

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int wait_io(int fd, short events, int64_t until) {
    struct pollfd p = {.fd = fd, .events = events};
    int64_t left;
    while ((left = until - now_ms()) > 0) {
        int n = poll(&p, 1, (int)left);
        if (n >= 0)
            return n > 0 && (p.revents & (events | POLLHUP | POLLERR));
        if (errno != EINTR)
            break;
    }
    return 0;
}

static int send_all(int fd, const char *p, size_t size, int64_t until) {
    while (size && wait_io(fd, POLLOUT, until)) {
        ssize_t n = send(fd, p, size, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (n <= 0)
            break;
        p += n;
        size -= (size_t)n;
    }
    return size == 0;
}

static ssize_t receive(int fd, char *buf, size_t size, int64_t until) {
    while (wait_io(fd, POLLIN, until)) {
        ssize_t n = recv(fd, buf, size, 0);
        if (n >= 0 || (errno != EAGAIN && errno != EINTR))
            return n;
    }
    return -1;
}

response api_exchange(int fd, const char *path, const char *form, size_t limit, int timeout_ms) {
    int64_t until = now_ms() + timeout_ms;
    char header[2048];
    size_t form_size = form ? strlen(form) : 0;
    int len = snprintf(header, sizeof(header),
                       "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n"
                       "%sContent-Length: %zu\r\n\r\n",
                       form ? "POST" : "GET", path, API_CONNECT_PORT,
                       form ? "X-Lodestar: 1\r\nContent-Type: application/x-www-form-urlencoded\r\n" : "",
                       form_size);
    if (len < 0 || (size_t)len >= sizeof(header) || !send_all(fd, header, (size_t)len, until) ||
        (form_size && !send_all(fd, form, form_size, until)))
        return (response){0};

    size_t used = 0;
    char *start = NULL;
    while (!start && used < sizeof(header) - 1) {
        ssize_t n = receive(fd, header + used, sizeof(header) - 1 - used, until);
        if (n <= 0)
            return (response){0};
        used += (size_t)n;
        header[used] = 0;
        start = strstr(header, "\r\n\r\n");
    }
    int status;
    if (!start || sscanf(header, "HTTP/1.%*1[01] %3d", &status) != 1 || status < 100 || status > 599)
        return (response){0};
    long long length = -1;
    for (const char *p = strchr(header, '\n') + 1; p < start; p = strchr(p, '\n') + 1) {
        if (!strncasecmp(p, "Transfer-Encoding:", 18))
            return (response){0};
        if (!strncasecmp(p, "Content-Length:", 15)) {
            const char *v = p + 15;
            while (*v == ' ' || *v == '\t')
                v++;
            if (length != -1 || *v < '0' || *v > '9')
                return (response){0};
            char *end;
            errno = 0;
            length = strtoll(v, &end, 10);
            while (*end == ' ' || *end == '\t')
                end++;
            if (errno || *end != '\r')
                return (response){0};
        }
    }
    if (length < 0 || (unsigned long long)length > limit)
        return (response){0};
    start += 4;
    size_t have = used - (size_t)(start - header);
    if (have > (size_t)length)
        return (response){0};
    char *body = malloc((size_t)length + 1);
    if (!body)
        return (response){0};
    memcpy(body, start, have);
    while (have < (size_t)length) {
        ssize_t n = receive(fd, body + have, (size_t)length - have, until);
        if (n <= 0) {
            free(body);
            return (response){0};
        }
        have += (size_t)n;
    }
    body[have] = 0;
    return (response){.status = status, .size = have, .body = body};
}

response api_request(const char *path, const char *form, size_t limit, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return (response){0};
#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    struct sockaddr_in loopback = {
        .sin_family = AF_INET, .sin_port = htons((uint16_t)API_CONNECT_PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    response res = {0};
    if (fcntl(fd, F_SETFL, O_NONBLOCK) == 0) {
        int rc = connect(fd, (struct sockaddr *)&loopback, sizeof(loopback));
        if (rc < 0 && errno == EINPROGRESS && wait_io(fd, POLLOUT, now_ms() + 1000)) {
            int error = 0;
            socklen_t len = sizeof(error);
            rc = getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) || error ? -1 : 0;
        }
        if (!rc)
            res = api_exchange(fd, path, form, limit, timeout_ms);
    }
    close(fd);
    return res;
}

static int object(const response *res, int status, jv_t *root) {
    if (res->status != status || res->size < 2 || res->body[0] != '{' ||
        res->body[res->size - 1] != '}' || memchr(res->body, 0, res->size))
        return -1;
    *root = jv_of(res->body, res->size);
    return 0;
}

static int boolean(jv_t obj, const char *key, int fallback) {
    jv_t v;
    if (!jv_get(obj, key, &v)) {
        if (v.n == 4 && !memcmp(v.p, "true", 4))
            return 1;
        if (v.n == 5 && !memcmp(v.p, "false", 5))
            return 0;
    }
    return fallback;
}

static int valid_id(const char *id) {
    if (strlen(id) != 9)
        return 0;
    for (size_t i = 0; i < 9; ++i)
        if ((id[i] < 'A' || id[i] > 'Z') && (id[i] < '0' || id[i] > '9'))
            return 0;
    return 1;
}

static enum game_state parse_state(const char *value) {
    const char *names[] = {"", "ready", "restart", "running", "removed",
                           "offline", "internal", "pending", "duplicate"};
    for (int i = READY; i <= DUPLICATE; i++)
        if (!strcmp(value, names[i]))
            return (enum game_state)i;
    return UNKNOWN;
}

int api_library(const response *res, library *out) {
    jv_t root, games, drives, row;
    if (object(res, 200, &root) || jv_get(root, "games", &games) || games.n < 2 ||
        games.p[0] != '[' || games.p[games.n - 1] != ']')
        return -1;
    char lang[8];
    jv_get_string(root, "lang", lang, sizeof(lang));
    out->spanish = !strcmp(lang, "es");
    out->setup = boolean(root, "setup", 0);
    jv_get_string(root, "version", out->version, sizeof(out->version));

    if (!jv_get(root, "drives", &drives)) {
        for (; out->drive_count < DRIVE_LIMIT && !jv_array_next(&drives, &row); out->drive_count++) {
            drive *d = &out->drives[out->drive_count];
            jv_get_string(row, "label", d->label, sizeof(d->label));
            d->free_bytes = jv_get_int(row, "free", 0);
        }
    }
    while (!jv_array_next(&games, &row)) {
        if (out->count == GAME_LIMIT) {
            out->overflow = 1;
            break;
        }
        game *g = &out->games[out->count];
        char state[24], id[24];
        jv_get_string(row, "title_id", id, sizeof(id));
        if (!valid_id(id))
            continue;
        memcpy(g->id, id, sizeof(g->id));
        jv_get_string(row, "state", state, sizeof(state));
        g->state = parse_state(state);
        jv_get_string(row, "name", g->name, sizeof(g->name));
        jv_get_string(row, "source", g->source, sizeof(g->source));
        jv_get_string(row, "storage", g->storage, sizeof(g->storage));
        jv_get_string(row, "version", g->version, sizeof(g->version));
        jv_get_string(row, "system_ver", g->firmware, sizeof(g->firmware));
        jv_get_string(row, "update_issue", g->issue, sizeof(g->issue));
        g->bytes = jv_get_int(row, "size", 0);
        g->dlc = jv_get_int(row, "dlc", 0);
        g->in_place = boolean(row, "in_place", 1);
        out->count++;
    }
    return 0;
}

int api_settings(const response *res, settings *out) {
    jv_t root;
    char mode[24], lang[8];
    if (object(res, 200, &root) || jv_get_string(root, "dlc_mode", mode, sizeof(mode)) ||
        jv_get_string(root, "lang", lang, sizeof(lang)))
        return -1;
    int batch_size = (int)jv_get_int(root, "batch_size", 5);
    *out = (settings){
        .organize = boolean(root, "organize", 0),
        .internal = boolean(root, "internal", 0),
        .dlc_big = boolean(root, "dlc_big", 0),
        .dlc_mode = !strcmp(mode, "internal") ? 1 : !strcmp(mode, "off") ? 2 : 0,
        .language = !strcmp(lang, "en") ? 1 : !strcmp(lang, "es") ? 2 : 0,
        .autorun = boolean(root, "autorun", 0),
        .klog = boolean(root, "klog", 0),
        .batch = boolean(root, "batch", 0),
        .batch_size = batch_size >= 1 && batch_size <= 50 ? batch_size : 5,
    };
    return 0;
}

int api_result(const response *res, char *message, size_t size) {
    jv_t root;
    if (!object(res, 400, &root)) {
        jv_get_string(root, "error", message, size);
        return -1;
    }
    if (object(res, 200, &root) || !boolean(root, "ok", 0))
        return -1;
    jv_get_string(root, "message", message, size);
    return 0;
}

int api_scan_active(const response *res) {
    jv_t root, activity;
    char phase[24];
    if (object(res, 200, &root) || jv_get(root, "activity", &activity) ||
        jv_get_string(activity, "phase", phase, sizeof(phase)))
        return -1;
    return *phase != 0;
}

int api_downloads(const response *res, downloads *out) {
    jv_t root, jobs, job, row;
    if (object(res, 200, &root) || jv_get(root, "jobs", &jobs) || jv_get(root, "job", &job))
        return -1;
    out->configured = boolean(root, "configured", 0);
    out->active = boolean(job, "busy", 0);
    jv_get_string(job, "name", out->name, sizeof(out->name));
    jv_get_string(job, "phase", out->phase, sizeof(out->phase));
    jv_get_string(root, "result", out->message, sizeof(out->message));
    out->done = jv_get_int(job, "done", 0);
    out->total = jv_get_int(job, "total", 0);
    for (out->count = 0; out->count < DOWNLOAD_LIMIT && !jv_array_next(&jobs, &row); out->count++) {
        download *item = &out->jobs[out->count];
        jv_get_string(row, "name", item->name, sizeof(item->name));
        jv_get_string(row, "state", item->state, sizeof(item->state));
        jv_get_string(row, "error", item->error, sizeof(item->error));
    }
    return 0;
}

int api_destinations(const response *res, destinations *out) {
    jv_t root, list, row;
    if (object(res, 200, &root) || jv_get(root, "dests", &list))
        return -1;
    out->count = 0;
    while (out->count < DRIVE_LIMIT && !jv_array_next(&list, &row)) {
        char id[32];
        jv_get_string(row, "id", id, sizeof(id));
        size_t n = strlen(id);
        if (strcmp(id, "internal") && (n < 4 || n > 5 || strncmp(id, "usb", 3) ||
                                       strspn(id + 3, "0123456789") != n - 3))
            continue;
        memcpy(out->items[out->count].id, id, n + 1);
        jv_get_string(row, "label", out->items[out->count].label, sizeof(out->items[out->count].label));
        out->count++;
    }
    return 0;
}
