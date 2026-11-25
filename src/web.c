/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Minimal HTTP server for the PS4Mount panel. */
/* Minimal HTTP server for the PS4Mount panel (one request per connection). */
#include "web.h"
#include "webnet.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "drive.h"
#include "links.h"
#include "page_data.h"
#include "lang.h"
#include "mem.h"
#include "pkg.h"

#define LOG_PATH "/data/ps4mount/log.txt"
#define CHECK_MAX (16u << 20) /* most a pkg check is given */
#define REQ_MS 30000          /* headers and the start of a body: this long, all told */

static void web_pool_start(void);

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int web_listen(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 8)) {
        if (s >= 0)
            close(s);
        return -1;
    }
    web_pool_start();
    return s;
}

/* Owner responses are captured per thread while the owner decides them. Worker replies keep
 * writing directly because no capture is installed on those threads. */
typedef struct {
    buf_t out;
} web_capture_t;
static pthread_key_t g_capture_key;
static pthread_once_t g_capture_once = PTHREAD_ONCE_INIT;
static int g_capture_key_ok;
static void capture_key_make(void) { g_capture_key_ok = !pthread_key_create(&g_capture_key, NULL); }
static int capture_key_ready(void) {
    pthread_once(&g_capture_once, capture_key_make);
    return g_capture_key_ok;
}
static web_capture_t *capture_current(void) {
    if (!capture_key_ready())
        return NULL;
    return pthread_getspecific(g_capture_key);
}

static void send_all(int fd, const void *p, size_t n) {
    web_capture_t *cap = capture_current();
    if (cap) {
        buf_put(&cap->out, p, n);
        return;
    }
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w <= 0)
            return;
        c += w;
        n -= (size_t)w;
    }
}

static void reply(int fd, const char *status, const char *type, const void *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, type, len);
    send_all(fd, head, (size_t)n);
    send_all(fd, body, len);
}

static void json_msg(int fd, int ok, const char *msg) {
    buf_t b = {0};
    buf_printf(&b, "{\"ok\":%s,\"%s\":\"", ok ? "true" : "false", ok ? "message" : "error");
    for (const char *p = msg; *p; p++) {
        if (*p == '"' || *p == '\\')
            buf_put(&b, "\\", 1);
        if ((unsigned char)*p >= 0x20)
            buf_put(&b, p, 1);
    }
    buf_printf(&b, "\"}");
    reply(fd, ok ? "200 OK" : "400 Bad Request", "application/json; charset=utf-8", b.data, b.len);
    buf_free(&b);
}

/* A title id (CUSA00000) at p, copied to tid. 0 when valid. */
static int title_at(const char *p, char *tid) {
    for (int i = 0; i < 9; i++) {
        char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return -1;
        tid[i] = c;
    }
    tid[9] = 0;
    return is_game_id(tid) ? 0 : -1;
}

static int title_param(const char *path, char *tid) {
    const char *p = strstr(path, "title_id=");
    return p ? title_at(p + 9, tid) : -1;
}

static void send_log(int fd) {
    buf_t b = {0}, j = {0};
    int f = open(LOG_PATH, O_RDONLY);
    if (f >= 0) {
        off_t end = lseek(f, 0, SEEK_END), start = end > 16384 ? end - 16384 : 0;
        lseek(f, start, SEEK_SET);
        char tmp[4096];
        ssize_t n;
        while ((n = read(f, tmp, sizeof(tmp))) > 0)
            buf_put(&b, tmp, (size_t)n);
        close(f);
    }
    buf_printf(&j, "{\"log\":\"");
    for (size_t i = 0; i < b.len; i++) {
        char c = (char)b.data[i];
        if (c == '\n')
            buf_put(&j, "\\n", 2);
        else if (c == '"' || c == '\\') {
            buf_put(&j, "\\", 1);
            buf_put(&j, &c, 1);
        } else if ((unsigned char)c >= 0x20)
            buf_put(&j, &c, 1);
    }
    buf_printf(&j, "\"}");
    reply(fd, "200 OK", "application/json; charset=utf-8", j.data, j.len);
    buf_free(&b);
    buf_free(&j);
}

/* Requests that write files: from the local network only, and with the panel's own
 * header, which a page of another site cannot send without the panel agreeing (CORS
 * preflight): no site the browser opens can push files to the console. */
static int may_write(int fd, const char *req) {
    const char *h = req;
    int header = 0;
    /* the header block only: req also holds body bytes already read, and a
     * "X-PS4Mount:" line in them must not count as the panel's header */
    while ((h = strchr(h, '\n'))) {
        h++;
        if (*h == '\r' || *h == '\n')
            break; /* the empty line that ends the headers */
        if (!strncasecmp(h, "X-PS4Mount:", 11)) {
            header = 1;
            break;
        }
    }
    if (!header)
        return 0;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) || ss.ss_family != AF_INET)
        return 1; /* a local socket (the host tests) */
    uint32_t a = ntohl(((struct sockaddr_in *)&ss)->sin_addr.s_addr);
    return web_net_ok(a);
}

/* The body after what was read with the headers, up to len bytes. NULL on a short
 * read or when deadline_ms passes first (a trickle must not hold the one thread). */
static uint8_t *read_body(int fd, const char *pre, size_t pre_len, size_t len, long long deadline_ms) {
    uint8_t *b = mem_alloc(len ? len : 1);
    if (!b)
        return NULL;
    size_t have = pre_len < len ? pre_len : len;
    memcpy(b, pre, have);
    while (have < len) {
        if (now_ms() >= deadline_ms) {
            mem_free(b);
            return NULL;
        }
        ssize_t m = read(fd, b + have, len - have);
        if (m <= 0) {
            mem_free(b);
            return NULL;
        }
        have += (size_t)m;
    }
    return b;
}


/* WEBAPI-1 boundary: one parsed request owns the bytes that application dispatch uses.
 * raw is the header block plus whatever body bytes arrived with it. Large streaming bodies
 * remain outside raw and are read only by their explicit streaming route. */
typedef struct {
    char raw[8192];
    char method[8], path[1024];
    char *body;
    size_t have;
    unsigned long long length;
    long long deadline;
    int post;
    uint8_t *body_owned;    /* worker-read check body; owned by this request object */
    size_t body_owned_len;  /* bytes and owner-body quota units while body_owned is set */
    int body_incomplete;    /* worker finished waiting: owner must not read the socket */
} web_req_t;


/* WEBAPI-6: conservative route ownership. Worker routes either have no mutable state or are
 * already owned by a subsystem with its own locks. Everything else waits for the main loop. */
int web_route_worker_safe(const char *path, int post) {
    if (!strncmp(path, "/icon/", 6) || !strncmp(path, "/pic/", 5))
        return 0;
    if (!strncmp(path, "/api/", 5)) {
        if (!strcmp(path, "/api/log") || !strcmp(path, "/api/drive") || !strcmp(path, "/api/links") ||
            !strcmp(path, "/api/uploads") || !strcmp(path, "/api/nas/scan"))
            return 1;
        if (post && !strcmp(path, "/api/nas/shares"))
            return 1;
        if (post && (!strncmp(path, "/api/drive/", 11) || !strncmp(path, "/api/links/", 11)))
            return 1;
        if (post && !strncmp(path, "/api/upload", 11) && (path[11] == 0 || path[11] == '?'))
            return 1;
        return 0;
    }
    return 1; /* pages and unknown paths: static data or the fixed 404 */
}

#define WEB_POOL_THREADS 2
#define WEB_POOL_QUEUE 8
#define WEB_OWNER_QUEUE 4
#define WEB_OWNER_BODY_BYTES (16u << 20)
#define WEB_STACK_SIZE (256u << 10)

typedef struct web_owner_job {
    int fd;
    web_req_t req;
    struct web_owner_job *next;
} web_owner_job_t;

typedef struct web_write_job {
    int fd;
    uint8_t *data;
    size_t len;
    struct web_write_job *next;
} web_write_job_t;

static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pool_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_pool_idle_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_pool_threads[WEB_POOL_THREADS];
static int g_pool_nthreads, g_pool_started, g_pool_quit;
static int g_pool_fds[WEB_POOL_QUEUE], g_pool_head, g_pool_count;
#define WEB_WRITE_QUEUE 8
#define WEB_WRITE_BYTES (1u << 20)
static web_write_job_t *g_write_head, *g_write_tail;
static unsigned g_write_count, g_write_bytes, g_write_active;
static int g_pool_active[WEB_POOL_THREADS];
static unsigned g_pool_inflight;
static int g_wake_r = -1, g_wake_w = -1;

static pthread_mutex_t g_drive_action_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_links_action_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_owner_mu = PTHREAD_MUTEX_INITIALIZER;
static web_owner_job_t *g_owner_head, *g_owner_tail;
static int g_owner_count;
static size_t g_owner_body_bytes;

static void web_handle_connection(int fd, int queue_owner);
static void web_dispatch(int fd, web_req_t *r);

static void nonblock_fd(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void web_owner_wake(void) {
    if (g_wake_w >= 0) {
        char b = 1;
        (void)write(g_wake_w, &b, 1); /* nonblocking: a full pipe is already a wake */
    }
}

int web_owner_wake_fd(void) { return g_wake_r; }

static void web_owner_drain_wake(void) {
    char tmp[64];
    while (g_wake_r >= 0 && read(g_wake_r, tmp, sizeof(tmp)) > 0)
        ;
}

static int owner_body_reserve(size_t len) {
    pthread_mutex_lock(&g_owner_mu);
    int ok = len <= WEB_OWNER_BODY_BYTES - g_owner_body_bytes;
    if (ok)
        g_owner_body_bytes += len;
    pthread_mutex_unlock(&g_owner_mu);
    return ok ? 0 : -1;
}

static void owner_body_release(size_t len) {
    pthread_mutex_lock(&g_owner_mu);
    g_owner_body_bytes -= len;
    pthread_mutex_unlock(&g_owner_mu);
}

static void owner_body_free(web_req_t *req) {
    if (!req->body_owned)
        return;
    mem_free(req->body_owned);
    owner_body_release(req->body_owned_len);
    req->body_owned = NULL;
    req->body_owned_len = 0;
}

/* Copies req into a node owned by the main-loop queue. body points into raw, so it must be
 * relocated after the copy; leaving it unchanged would hand the owner a pointer to worker stack. */
static int web_owner_enqueue(int fd, const web_req_t *req) {
    web_owner_job_t *job = mem_alloc(sizeof(*job));
    if (!job)
        return -1;
    size_t body_at = req->body ? (size_t)(req->body - req->raw) : 0;
    job->fd = fd;
    job->req = *req;
    job->req.body = job->req.raw + body_at;
    job->next = NULL;
    pthread_mutex_lock(&g_owner_mu);
    if (g_owner_count >= WEB_OWNER_QUEUE) {
        pthread_mutex_unlock(&g_owner_mu);
        mem_free(job);
        return -1;
    }
    if (g_owner_tail)
        g_owner_tail->next = job;
    else
        g_owner_head = job;
    g_owner_tail = job;
    g_owner_count++;
    pthread_mutex_unlock(&g_owner_mu);
    web_owner_wake();
    return 0;
}

int web_owner_pending(void) {
    pthread_mutex_lock(&g_owner_mu);
    int n = g_owner_count;
    pthread_mutex_unlock(&g_owner_mu);
    return n;
}

/* fd and captured bytes move together. Failure closes the connection rather than sending a
 * response whose completion would depend on unbounded memory or on the owner blocking. */
static int web_write_enqueue(int fd, buf_t *out) {
    if (!out->data || !out->len)
        return -1;
    web_write_job_t *job = mem_alloc(sizeof(*job));
    if (!job)
        return -1;
    job->fd = fd;
    job->data = out->data;
    job->len = out->len;
    job->next = NULL;
    pthread_mutex_lock(&g_pool_mu);
    if (!g_pool_started || g_pool_quit || g_write_count >= WEB_WRITE_QUEUE ||
        out->len > WEB_WRITE_BYTES - g_write_bytes) {
        pthread_mutex_unlock(&g_pool_mu);
        mem_free(job);
        return -1;
    }
    if (g_write_tail)
        g_write_tail->next = job;
    else
        g_write_head = job;
    g_write_tail = job;
    g_write_count++;
    g_write_bytes += out->len;
    g_pool_inflight++;
    pthread_cond_signal(&g_pool_cv);
    pthread_mutex_unlock(&g_pool_mu);
    out->data = NULL;
    out->len = out->cap = 0;
    return 0;
}

/* Main-loop owner only: executes every queued parsed request through the same dispatcher as
 * the historical synchronous path. Response bytes are captured whole and handed to the writer
 * queue; direct worker dispatch retains the historical close unless it explicitly transfers fd. */
void web_owner_pump(void) {
    static int pumping; /* main-loop owner only: registration may pump events recursively */
    if (pumping)
        return;
    pumping = 1;
    web_owner_drain_wake();
    for (;;) {
        pthread_mutex_lock(&g_owner_mu);
        web_owner_job_t *job = g_owner_head;
        if (job) {
            g_owner_head = job->next;
            if (!g_owner_head)
                g_owner_tail = NULL;
            g_owner_count--;
        }
        pthread_mutex_unlock(&g_owner_mu);
        if (!job)
            break;
        if (!capture_key_ready()) {
            web_dispatch(job->fd, &job->req); /* direct historical fallback: dispatch closes fd */
            owner_body_free(&job->req);
            mem_free(job);
            continue;
        }
        web_capture_t cap = {0};
        pthread_setspecific(g_capture_key, &cap);
        web_dispatch(job->fd, &job->req);
        pthread_setspecific(g_capture_key, NULL);
        if (web_write_enqueue(job->fd, &cap.out))
            close(job->fd);
        buf_free(&cap.out);
        owner_body_free(&job->req);
        mem_free(job);
    }
    web_owner_drain_wake();
    pumping = 0;
}

static void *web_pool_worker(void *arg) {
    int slot = (int)(intptr_t)arg;
    for (;;) {
        pthread_mutex_lock(&g_pool_mu);
        while (!g_pool_count && !g_write_count && !g_pool_quit)
            pthread_cond_wait(&g_pool_cv, &g_pool_mu);
        if (!g_pool_count && !g_write_count && g_pool_quit) {
            pthread_mutex_unlock(&g_pool_mu);
            break;
        }
        web_write_job_t *write_job = NULL;
        int fd;
        if (g_write_head) {
            write_job = g_write_head;
            g_write_head = write_job->next;
            if (!g_write_head)
                g_write_tail = NULL;
            g_write_count--;
            g_write_active++;
            fd = write_job->fd;
        } else {
            fd = g_pool_fds[g_pool_head];
            g_pool_head = (g_pool_head + 1) % WEB_POOL_QUEUE;
            g_pool_count--;
        }
        g_pool_active[slot] = fd;
        pthread_mutex_unlock(&g_pool_mu);

        if (write_job) {
            send_all(write_job->fd, write_job->data, write_job->len);
            close(write_job->fd);
            size_t len = write_job->len;
            mem_free(write_job->data);
            mem_free(write_job);
            pthread_mutex_lock(&g_pool_mu);
            g_write_bytes -= len;
            g_write_active--;
            pthread_mutex_unlock(&g_pool_mu);
        } else
            web_handle_connection(fd, 1);

        pthread_mutex_lock(&g_pool_mu);
        g_pool_active[slot] = -1;
        if (g_pool_inflight)
            g_pool_inflight--;
        if (!g_pool_inflight)
            pthread_cond_broadcast(&g_pool_idle_cv);
        pthread_mutex_unlock(&g_pool_mu);
    }
    return NULL;
}

static void web_pool_start(void) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_pool_started) {
        pthread_mutex_unlock(&g_pool_mu);
        return;
    }
    for (int i = 0; i < WEB_POOL_THREADS; i++)
        g_pool_active[i] = -1;
    int fds[2] = {-1, -1};
    int wake_ok = !pipe(fds);
    if (wake_ok) {
        nonblock_fd(fds[0]);
        nonblock_fd(fds[1]);
        int mode0 = fcntl(fds[0], F_GETFL, 0), mode1 = fcntl(fds[1], F_GETFL, 0);
        wake_ok = mode0 >= 0 && mode1 >= 0 && (mode0 & O_NONBLOCK) && (mode1 & O_NONBLOCK);
        if (wake_ok) {
            g_wake_r = fds[0];
            g_wake_w = fds[1];
        } else {
            close(fds[0]);
            close(fds[1]);
        }
    }
    if (!wake_ok) { /* without the owner wake, threaded parsing would add latency: keep the synchronous fallback */
        pthread_mutex_unlock(&g_pool_mu);
        return;
    }
    g_pool_quit = 0;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, WEB_STACK_SIZE);
    int made = 0;
    for (; made < WEB_POOL_THREADS; made++) {
        if (pthread_create(&g_pool_threads[made], &at, web_pool_worker, (void *)(intptr_t)made))
            break;
    }
    pthread_attr_destroy(&at);
    if (made) {
        g_pool_nthreads = made;
        g_pool_started = 1;
    } else if (g_wake_r >= 0) {
        close(g_wake_r);
        close(g_wake_w);
        g_wake_r = g_wake_w = -1;
    }
    pthread_mutex_unlock(&g_pool_mu);
}

/* 0 queued; 1 no worker pool exists (caller keeps the synchronous fallback); -1 full/stopping. */
static int web_pool_offer(int fd) {
    int rc = 1;
    pthread_mutex_lock(&g_pool_mu);
    if (g_pool_started && !g_pool_quit) {
        if (g_pool_count >= WEB_POOL_QUEUE)
            rc = -1;
        else {
            size_t at = (size_t)(g_pool_head + g_pool_count) % WEB_POOL_QUEUE;
            g_pool_fds[at] = fd;
            g_pool_count++;
            g_pool_inflight++;
            pthread_cond_signal(&g_pool_cv);
            rc = 0;
        }
    }
    pthread_mutex_unlock(&g_pool_mu);
    return rc;
}

int web_write_busy(void) {
    pthread_mutex_lock(&g_pool_mu);
    int busy = g_write_count || g_write_active;
    pthread_mutex_unlock(&g_pool_mu);
    return busy;
}

void web_pool_idle_wait(void) {
    pthread_mutex_lock(&g_pool_mu);
    while (g_pool_inflight)
        pthread_cond_wait(&g_pool_idle_cv, &g_pool_mu);
    pthread_mutex_unlock(&g_pool_mu);
}

void web_stop(void) {
    pthread_t threads[WEB_POOL_THREADS];
    int nthreads = 0;
    pthread_mutex_lock(&g_pool_mu);
    if (g_pool_started) {
        g_pool_quit = 1;
        while (g_pool_count) {
            int fd = g_pool_fds[g_pool_head];
            g_pool_head = (g_pool_head + 1) % WEB_POOL_QUEUE;
            g_pool_count--;
            if (g_pool_inflight)
                g_pool_inflight--;
            close(fd); /* never parsed: no application intent is lost */
        }
        while (g_write_head) {
            web_write_job_t *job = g_write_head;
            g_write_head = job->next;
            g_write_count--;
            g_write_bytes -= job->len;
            if (g_pool_inflight)
                g_pool_inflight--;
            close(job->fd); /* never transmitted: no partial owner response is sent by policy */
            mem_free(job->data);
            mem_free(job);
        }
        g_write_tail = NULL;
        for (int i = 0; i < WEB_POOL_THREADS; i++)
            if (g_pool_active[i] >= 0)
                shutdown(g_pool_active[i], SHUT_RDWR); /* wake the read/write; the worker still owns close */
        nthreads = g_pool_nthreads;
        for (int i = 0; i < nthreads; i++)
            threads[i] = g_pool_threads[i];
        pthread_cond_broadcast(&g_pool_cv);
        pthread_cond_broadcast(&g_pool_idle_cv);
    }
    pthread_mutex_unlock(&g_pool_mu);
    for (int i = 0; i < nthreads; i++)
        pthread_join(threads[i], NULL);

    pthread_mutex_lock(&g_pool_mu);
    g_pool_started = g_pool_nthreads = 0;
    g_pool_quit = 0;
    g_pool_head = g_pool_count = 0;
    g_write_count = g_write_bytes = g_write_active = 0;
    g_pool_inflight = 0;
    for (int i = 0; i < WEB_POOL_THREADS; i++)
        g_pool_active[i] = -1;
    if (g_wake_r >= 0) {
        close(g_wake_r);
        close(g_wake_w);
        g_wake_r = g_wake_w = -1;
    }
    pthread_cond_broadcast(&g_pool_idle_cv);
    pthread_mutex_unlock(&g_pool_mu);

    pthread_mutex_lock(&g_owner_mu);
    web_owner_job_t *job = g_owner_head;
    g_owner_head = g_owner_tail = NULL;
    g_owner_count = 0;
    pthread_mutex_unlock(&g_owner_mu);
    while (job) {
        web_owner_job_t *next = job->next;
        close(job->fd);
        owner_body_free(&job->req);
        mem_free(job);
        job = next;
    }
}

/* dest=<usb0..usb7, ext0, ext1, internal> of a query: letters and digits only. */
static int dest_param(const char *path, char *dest, size_t n) {
    const char *p = strstr(path, "dest=");
    size_t o = 0;
    for (p = p ? p + 5 : ""; ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')) && o + 1 < n; p++)
        dest[o++] = *p;
    dest[o] = 0;
    return o ? 0 : -1;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return c - 'A' + 10;
}

/* The form field key of body, decoded (+ and %XX) into out. 0 when present. */
static int form_field(const char *body, const char *key, char *out, size_t n) {
    size_t kl = strlen(key), o = 0;
    for (const char *p = body; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, kl) || p[kl] != '=')
            continue;
        for (p += kl + 1; *p && *p != '&' && o + 1 < n; p++) {
            if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
                out[o++] = (char)((hexv(p[1]) << 4) | hexv(p[2]));
                p += 2;
            } else
                out[o++] = *p == '+' ? ' ' : *p;
        }
        out[o] = 0;
        return 0;
    }
    out[0] = 0;
    return -1;
}

/* POST /api/drive/<action>[?title_id=..&dest=..]: the Drive thread does the work, the
 * panel follows it in GET /api/drive. The user's client comes in the body (client_id,
 * client_secret), never in the address. */
static void drive_action(int fd, const char *path, const char *body) {
    const char *a = path + 11;
    size_t al = strcspn(a, "?");
    char tid[10], dest[16], msg[512];
    int has_tid = !title_param(path, tid), rc;
#define IS(name) (al == sizeof(name) - 1 && !strncmp(a, name, al))
    if (IS("client")) {
        char id[256], secret[128];
        form_field(body, "client_id", id, sizeof(id));
        form_field(body, "client_secret", secret, sizeof(secret));
        rc = drive_set_client(id, secret, msg, sizeof(msg));
        memset(secret, 0, sizeof(secret));
    } else if (IS("forget_client"))
        rc = drive_forget_client(msg, sizeof(msg));
    else if (IS("connect"))
        rc = drive_connect(msg, sizeof(msg));
    else if (IS("disconnect"))
        rc = drive_disconnect(msg, sizeof(msg));
    else if (IS("upload"))
        rc = strstr(path, "title_id=") && !has_tid ? (snprintf(msg, sizeof(msg), "%s", TR("invalid title_id", "title_id no válido")), -1)
                                                   : drive_upload(has_tid ? tid : NULL, msg, sizeof(msg));
    else if (IS("download"))
        rc = !has_tid || dest_param(path, dest, sizeof(dest))
                 ? (snprintf(msg, sizeof(msg), "%s", TR("invalid title_id or dest", "title_id o dest no válidos")), -1)
                 : drive_download(tid, dest, msg, sizeof(msg));
    else if (IS("trash"))
        rc = !has_tid ? (snprintf(msg, sizeof(msg), "%s", TR("invalid title_id", "title_id no válido")), -1)
                      : drive_trash(tid, msg, sizeof(msg));
    else if (IS("refresh"))
        rc = drive_refresh(msg, sizeof(msg));
    else if (IS("cancel"))
        rc = drive_cancel(msg, sizeof(msg));
    else {
        reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
        return;
    }
#undef IS
    json_msg(fd, !rc, msg);
}

/* The panel's X-PS4Mount-Password header (percent-encoded: any password is plain ASCII
   there, and none is ever in a path the log could show), decoded into out; "" without it. */
static void password_header(const char *req, char *out, size_t n) {
    out[0] = 0;
    for (const char *h = req; (h = strchr(h, '\n'));) {
        h++;
        if (*h == '\r' || *h == '\n')
            return; /* the end of the headers */
        if (strncasecmp(h, "X-PS4Mount-Password:", 20))
            continue;
        const char *v = h + 20;
        while (*v == ' ')
            v++;
        size_t l = strcspn(v, "\r\n");
        char *f = mem_alloc(l + 3);
        if (f) {
            memcpy(f, "p=", 2);
            memcpy(f + 2, v, l);
            f[l + 2] = 0;
            form_field(f, "p", out, n);
            mem_free(f);
        }
        return;
    }
}

/* The pasted links of /api/links/add: at most this much text. */
#define LINKS_TEXT_MAX (1u << 20)

/* POST /api/links/<action>: add?dest=.. (the links are the body, one a line), cancel?id=..,
 * retry?id=..; add and retry take an archive's password in X-PS4Mount-Password. The links
 * thread does the work; the panel follows it in GET /api/links. */
static void links_action(int fd, const char *req, const char *path, const char *body, size_t have, unsigned long long length,
                         long long deadline) {
    const char *a = path + 11;
    size_t al = strcspn(a, "?");
    const char *idp = strstr(path, "id=");
    int id = idp ? atoi(idp + 3) : 0, rc;
    char msg[512], pw[256];
    password_header(req, pw, sizeof(pw));
#define IS(name) (al == sizeof(name) - 1 && !strncmp(a, name, al))
    if (IS("add")) {
        char dest[16];
        uint8_t *data = NULL;
        char *text = NULL;
        if (dest_param(path, dest, sizeof(dest)))
            rc = (snprintf(msg, sizeof(msg), "%s", TR("choose where to save it", "elige dónde guardarlo")), -1);
        else if (!length || length > LINKS_TEXT_MAX)
            rc = (snprintf(msg, sizeof(msg), "%s", TR("paste at least one link", "pega al menos un enlace")), -1);
        else if (!(data = read_body(fd, body, have, (size_t)length, deadline)) || !(text = mem_alloc((size_t)length + 1)))
            rc = (snprintf(msg, sizeof(msg), "%s", TR("the links did not arrive whole", "los enlaces no llegaron enteros")), -1);
        else {
            memcpy(text, data, (size_t)length);
            text[length] = 0;
            rc = links_add(text, dest, pw, msg, sizeof(msg));
        }
        mem_free(data);
        mem_free(text);
    } else if (IS("cancel"))
        rc = id > 0 ? links_cancel(id, msg, sizeof(msg)) : (snprintf(msg, sizeof(msg), "invalid id"), -1);
    else if (IS("retry"))
        rc = id > 0 ? links_retry(id, pw, msg, sizeof(msg)) : (snprintf(msg, sizeof(msg), "invalid id"), -1);
    else {
        reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
        return;
    }
#undef IS
    memset(pw, 0, sizeof(pw));
    json_msg(fd, !rc, msg);
}

void web_serve(int listen_fd) {
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    int offered = web_pool_offer(fd);
    if (offered > 0)
        web_handle(fd); /* no worker could be created: the historical synchronous path */
    else if (offered < 0)
        close(fd); /* bounded overload: kernel memory is never allowed to grow without a cap */
}

static void web_dispatch(int fd, web_req_t *r);

/* 0: r is ready for dispatch; 1: close without more work; -1: malformed request. */
static int web_read_request(int fd, web_req_t *r) {
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    long long deadline = now_ms() + REQ_MS;
    char *req = r->raw;
    ssize_t n = read(fd, req, sizeof(r->raw) - 1);
    if (n <= 0) {
        return 1;
    }
    req[n] = 0;
    /* https://: a TLS handshake. There is no certificate to answer it (a made-up one would
     * show a security warning), so it is closed at once: browsers that tried https first
     * go back to http:// quickly. */
    if ((unsigned char)req[0] == 0x16) {
        printf("[web] an https:// request: the panel is http:// only\n");
        return 1;
    }
    char *body;
    while (!(body = strstr(req, "\r\n\r\n")) && (size_t)n < sizeof(r->raw) - 1 && now_ms() < deadline) {
        ssize_t m = read(fd, req + n, sizeof(r->raw) - 1 - (size_t)n);
        if (m <= 0)
            break;
        n += m;
        req[n] = 0;
    }
    if (!body)
        return -1;
    unsigned long long length = 0;
    int have_length = 0;
    for (const char *h = strchr(req, '\n'); h && h < body;) {
        h++;
        if (!strncasecmp(h, "Transfer-Encoding:", 18))
            return -1; /* this server only reads bodies with a declared length */
        if (!strncasecmp(h, "Content-Length:", 15)) {
            const char *v = h + 15;
            while (*v == ' ' || *v == '\t')
                v++;
            if (have_length || *v < '0' || *v > '9')
                return -1;
            char *end;
            errno = 0;
            length = strtoull(v, &end, 10);
            while (*end == ' ' || *end == '\t')
                end++;
            if (errno == ERANGE || *end != '\r' || end[1] != '\n')
                return -1;
            have_length = 1;
        }
        h = strchr(h, '\n');
    }
    body += 4;
    size_t have = (size_t)(req + n - body);
    if (length < have)
        have = (size_t)length;
    char method[8] = "", path[1024] = "";
    sscanf(req, "%7s %1023s", method, path);
    int post = !strcmp(method, "POST");
    /* every POST changes something (a mount, a setting, the payload itself): the panel's
     * own requests only, so a page of another site cannot drive the console */
    if (post && !may_write(fd, req)) {
        reply(fd, "403 Forbidden", "text/plain", "forbidden\n", 10);
        return 1;
    }

    int form = post && (!strcmp(path, "/api/config") || !strncmp(path, "/api/restore", 12) ||
                        !strcmp(path, "/api/drive/client") || !strncmp(path, "/api/drive/client?", 18) ||
                        !strcmp(path, "/api/nas/shares"));
    if (form) {
        if (length >= (size_t)(req + sizeof(r->raw) - body))
            return -1;
        while (have < length && now_ms() < deadline) {
            ssize_t m = read(fd, body + have, (size_t)length - have);
            if (m <= 0)
                break;
            have += (size_t)m;
        }
        if (have != length || memchr(body, 0, have))
            return -1;
        body[have] = 0;
    }


    snprintf(r->method, sizeof(r->method), "%s", method);
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->body = body;
    r->have = have;
    r->length = length;
    r->deadline = deadline;
    r->post = post;
    r->body_owned = NULL;
    r->body_owned_len = 0;
    r->body_incomplete = 0;
    return 0;
}

/* Reads a bounded check body in the network worker. On success body_owned, its quota and fd
 * move as one owner intent; on a short read only the proven fact of incompleteness is queued. */
static int web_prepare_check_body(int fd, web_req_t *r) {
    if (!r->post || strncmp(r->path, "/api/check", 10) || (r->path[10] && r->path[10] != '?') ||
        !r->length || r->length > CHECK_MAX)
        return 0;
    size_t len = (size_t)r->length;
    if (owner_body_reserve(len))
        return -1;
    uint8_t *data = mem_alloc(len);
    size_t got = 0;
    if (data) {
        size_t pre = r->have < len ? r->have : len;
        memcpy(data, r->body, pre);
        got = pre;
        while (got < len && now_ms() < r->deadline) {
            ssize_t n = read(fd, data + got, len - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
    }
    if (!data || got != len) {
        if (data)
            mem_free(data);
        owner_body_release(len);
        r->body_incomplete = 1;
        return 0;
    }
    r->body_owned = data;
    r->body_owned_len = len;
    return 0;
}

static void web_handle_connection(int fd, int queue_owner) {
    web_req_t r;
    int rr = web_read_request(fd, &r);
    if (rr > 0) {
        close(fd);
        return;
    }
    if (rr < 0) {
        reply(fd, "400 Bad Request", "text/plain", "bad request\n", 12);
        close(fd);
        return;
    }
    if (!queue_owner || web_route_worker_safe(r.path, r.post)) {
        web_dispatch(fd, &r);
        return;
    }
    if (web_prepare_check_body(fd, &r) || web_owner_enqueue(fd, &r)) {
        owner_body_free(&r);
        reply(fd, "503 Service Unavailable", "text/plain", "panel busy\n", 11);
        close(fd);
    }
}

void web_handle(int fd) { web_handle_connection(fd, 0); }

static void web_dispatch(int fd, web_req_t *r) {
    const char *req = r->raw, *path = r->path;
    char *body = r->body;
    size_t have = r->have;
    unsigned long long length = r->length;
    long long deadline = r->deadline;
    int post = r->post;
    char tid[10], msg[256];
    if (!strcmp(path, "/") || !strcmp(path, "/index.html") || !strncmp(path, "/?", 2)) { /* /?app=1: the panel app */
        reply(fd, "200 OK", "text/html; charset=utf-8", index_html, sizeof(index_html));
    } else if (!strcmp(path, "/legacy") || !strcmp(path, "/legacy.html")) {
        reply(fd, "200 OK", "text/html; charset=utf-8", legacy_html, sizeof(legacy_html));
    } else if (!strcmp(path, "/api/games")) {
        buf_t b = {0};
        app_games_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (!strncmp(path, "/icon/", 6) || !strncmp(path, "/pic/", 5)) {
        buf_t b = {0};
        int pic = path[1] == 'p';
        const char *id = path + (pic ? 5 : 6);
        /* the id becomes part of file paths: nothing but a title id */
        if (strlen(id) == 9 && !title_at(id, tid) && !(pic ? app_pic(tid, &b) : app_icon(tid, &b)))
            reply(fd, "200 OK", "image/png", b.data, b.len);
        else
            reply(fd, "404 Not Found", "text/plain", "no icon\n", 8);
        buf_free(&b);
    } else if (post && !strncmp(path, "/api/mount", 10)) {
        if (title_param(path, tid))
            json_msg(fd, 0, TR("invalid title_id", "title_id no válido"));
        else {
            int rc = app_add(tid, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/unmount", 12)) {
        if (title_param(path, tid))
            json_msg(fd, 0, TR("invalid title_id", "title_id no válido"));
        else {
            int rc = app_remove(tid, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strcmp(path, "/api/rescan")) {
        app_rescan_hard();
        json_msg(fd, 1, TR("Looking for games on the drives and the share", "Buscando juegos en los discos y el NAS"));
    } else if (post && !strncmp(path, "/api/restore", 12) && (path[12] == 0 || path[12] == '?')) {
        /* ?title_id=<one game>, body title_ids=<games, comma separated>, or neither for all */
        int rc;
        const char *list = strstr(body, "title_ids=");
        if (strstr(path, "title_id="))
            rc = title_param(path, tid) ? (snprintf(msg, sizeof(msg), "invalid title_id"), -1)
                                        : app_restore(tid, msg, sizeof(msg));
        else if (list) {
            char tids[4096];
            size_t o = 0;
            for (const char *c = list + 10; *c && *c != '&' && o + 1 < sizeof(tids); c++) {
                if (!strncmp(c, "%2C", 3) || !strncmp(c, "%2c", 3)) {
                    tids[o++] = ',';
                    c += 2;
                } else
                    tids[o++] = *c;
            }
            tids[o] = 0;
            rc = app_restore(tids, msg, sizeof(msg));
        } else
            rc = app_restore(NULL, msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (post && !strcmp(path, "/api/restored/forget")) {
        int rc = app_restored_forget(msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (!strcmp(path, "/api/config") && !post) {
        buf_t b = {0};
        app_config_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (post && !strcmp(path, "/api/config")) {
        int rc = app_config_set(body, msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (post && !strncmp(path, "/api/check", 10) && (path[10] == 0 || path[10] == '?')) {
        const char *sz = strstr(path, "size=");
        uint8_t *data = r->body_owned;
        if (!length || length > CHECK_MAX)
            json_msg(fd, 0, TR("send the first MB of the pkg (at most 16 MB)", "envía el primer MB del pkg (como máximo 16 MB)"));
        else if (r->body_incomplete)
            json_msg(fd, 0, TR("the pkg start did not arrive whole", "el comienzo del pkg no llegó entero"));
        else if (!data && !(data = read_body(fd, body, have, (size_t)length, deadline)))
            json_msg(fd, 0, TR("the pkg start did not arrive whole", "el comienzo del pkg no llegó entero"));
        else {
            buf_t b = {0};
            app_check(data, (size_t)length, sz ? strtoull(sz + 5, NULL, 10) : 0, &b);
            reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
            buf_free(&b);
        }
        if (data != r->body_owned)
            mem_free(data);
    } else if (post && !strncmp(path, "/api/upload", 11) && (path[11] == 0 || path[11] == '?')) {
        if (!length)
            json_msg(fd, 0, TR("nothing was sent", "no se envió nada"));
        else if (!app_upload(fd, path, body, have, length, msg, sizeof(msg)))
            return; /* the upload answers and closes it */
        else
            json_msg(fd, 0, msg);
    } else if (!strcmp(path, "/api/uploads")) {
        buf_t b = {0};
        app_uploads_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (!strcmp(path, "/api/storage")) {
        buf_t b = {0};
        app_storage_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (!strcmp(path, "/api/dests")) {
        buf_t b = {0};
        app_dests_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (!strcmp(path, "/api/drive")) {
        buf_t b = {0};
        drive_status_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (post && !strncmp(path, "/api/drive/", 11)) {
        pthread_mutex_lock(&g_drive_action_mu);
        drive_action(fd, path, body);
        pthread_mutex_unlock(&g_drive_action_mu);
    } else if (!strcmp(path, "/api/links")) {
        buf_t b = {0};
        links_status_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (post && !strncmp(path, "/api/links/", 11)) {
        pthread_mutex_lock(&g_links_action_mu);
        links_action(fd, req, path, body, have, length, deadline);
        pthread_mutex_unlock(&g_links_action_mu);
    } else if (!strcmp(path, "/api/log")) {
        send_log(fd);
    } else if (!strcmp(path, "/api/nas/scan") && !may_write(fd, req)) {
        /* it connects to other machines: the panel's own requests only */
        reply(fd, "403 Forbidden", "text/plain", "forbidden\n", 10);
    } else if (!strcmp(path, "/api/nas/scan")) {
        buf_t b = {0};
        app_nas_scan_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (post && !strcmp(path, "/api/nas/shares")) {
        buf_t b = {0};
        app_nas_shares_json(body, &b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (post && !strcmp(path, "/api/nas/reload")) {
        int rc = app_nas_reload(msg, sizeof(msg));
        json_msg(fd, !rc, msg);
    } else if (post && !strcmp(path, "/api/app/close")) {
        json_msg(fd, 1, "closing");
        app_panel_close();
    } else if (post && !strcmp(path, "/api/stop")) {
        json_msg(fd, 1, "PS4Mount is stopping");
        app_stop();
    } else {
        reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
    }
    if (!capture_current())
        close(fd);
}
