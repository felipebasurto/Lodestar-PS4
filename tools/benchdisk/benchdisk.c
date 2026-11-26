/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * One-shot read-only disk diagnostic. It is deliberately separate from the daemon:
 * inputs are opened O_RDONLY, results are streamed to stdout and, when reachable,
 * to a reverse TCP listener on the operator's Mac. Nothing is written persistently. */
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <ps4/kernel.h>

#include "mem.h"

#ifndef BENCH_BUILD
#define BENCH_BUILD "unknown"
#endif
#ifndef BENCH_SDK
#define BENCH_SDK "573b4a0d"
#endif

#define BENCH_VER 2
#define BENCH_SEED 0xC0DE000000023827ULL
#define USB_PATCH "/mnt/usb0/ps4/CUSA23827/patch.pkg"
#define MNT_PATCH "/user/patch/CUSA23827/patch.pkg"
#define USB_SCAN_DIR "/mnt/usb0/PS4Mount"
#define USB_LIB_DIR "/mnt/usb0/ps4"
#define USB_AC_003 "/mnt/usb0/ps4/CUSA23827/addcont/CUSA23827/CODMW3CONTENT003/ac.pkg"
#define USB_AC_004 "/mnt/usb0/ps4/CUSA23827/addcont/CUSA23827/CODMW3CONTENT004/ac.pkg"
#define USB_AC_005 "/mnt/usb0/ps4/CUSA23827/addcont/CUSA23827/CODMW3CONTENT005/ac.pkg"
#define USB_AC_006 "/mnt/usb0/ps4/CUSA23827/addcont/CUSA23827/CODMW3CONTENT006/ac.pkg"
#define INTERNAL_PREFERRED "/user/app/CUSA36843/app.pkg"
#define APPDB_FALLBACK "/system_data/priv/mms/app.db"
#define RESULT_HOST "192.168.0.41"
#define RESULT_PORT 19191

#define KIB 1024ULL
#define MIB (1024ULL * 1024ULL)
#define GRANULE (16ULL * KIB)
#define BYTE_CAP (2ULL * 1024ULL * 1024ULL * 1024ULL)
#define DEADLINE_NS (240ULL * 1000ULL * 1000ULL * 1000ULL)
#define THREAD_STACK (256U * 1024U)
#define PATH_CAP 512
#define MAX_LAT_SAMPLES 512

#ifndef BENCH_AC_USB
#define BENCH_AC_USB NULL
#endif
#ifndef BENCH_AC_MNT
#define BENCH_AC_MNT NULL
#endif

enum {
    INC_NONE = 0,
    INC_DEADLINE,
    INC_BYTE_CAP,
    INC_ERROR,
    INC_SHORT_READ,
    INC_UNIQUE_EXHAUSTED,
    INC_THREAD_ERROR
};

static const char *inc_name(int inc) {
    switch (inc) {
    case INC_DEADLINE:
        return "deadline";
    case INC_BYTE_CAP:
        return "byte_cap";
    case INC_ERROR:
        return "error";
    case INC_SHORT_READ:
        return "short_read";
    case INC_UNIQUE_EXHAUSTED:
        return "unique_exhausted";
    case INC_THREAD_ERROR:
        return "thread_error";
    default:
        return "none";
    }
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t elapsed_since(uint64_t start) {
    uint64_t now = mono_ns();
    return now >= start ? now - start : 0;
}

/* --- result output: stdout mirror plus bounded reverse TCP, never a file --- */

static int g_out_fd = -1;
static int g_net_on = 0;
static pthread_mutex_t g_out_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_run[80] = "0-0";

static int net_write_all(const char *p, size_t n, uint64_t deadline) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(g_out_fd, p + done, n - done);
        if (w > 0) {
            done += (size_t)w;
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return -1;
        if (mono_ns() >= deadline)
            return -1;
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(g_out_fd, &fs);
        struct timeval tv = {0, 100000};
        int r = select(g_out_fd + 1, NULL, &fs, NULL, &tv);
        if (r < 0 && errno != EINTR)
            return -1;
    }
    return 0;
}

static void out_line(const char *line) {
    pthread_mutex_lock(&g_out_lock);
    printf("%s\n", line);
    fflush(stdout);
    if (g_net_on && g_out_fd >= 0) {
        uint64_t deadline = mono_ns() + 100000000ULL;
        if (net_write_all(line, strlen(line), deadline) || net_write_all("\n", 1, deadline)) {
            close(g_out_fd);
            g_out_fd = -1;
            g_net_on = 0;
            printf("[bench] run=%s net_lost=1\n", g_run);
            fflush(stdout);
        }
    }
    pthread_mutex_unlock(&g_out_lock);
}

static void benchf(const char *fmt, ...) {
    char line[2048];
    int n = snprintf(line, sizeof(line), "[bench] run=%s ", g_run);
    if (n < 0 || (size_t)n >= sizeof(line))
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);
    out_line(line);
}

static int net_connect_result(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(RESULT_PORT);
    sa.sin_addr.s_addr = inet_addr(RESULT_HOST);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    for (int attempt = 0; rc && errno == EINPROGRESS && attempt < 10; attempt++) {
        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(fd, &fs);
        struct timeval tv = {0, 200000};
        int r = select(fd + 1, NULL, &fs, NULL, &tv);
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (r == 1) {
            if (!getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) && !soerr) {
                rc = 0;
                break;
            }
            break;
        }
        if (r < 0 && errno != EINTR)
            break;
    }
    if (rc) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    g_out_fd = fd;
    g_net_on = 1;
    return 0;
}

/* --- escaping and small statistics helpers (no libc heap use) --- */

static void esc(const char *in, char *out, size_t n) {
    size_t o = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n; p++) {
        unsigned char c = *p;
        int plain = c > 32 && c < 127 && c != '=' && c != '%';
        if (plain) {
            out[o++] = (char)c;
        } else {
            if (o + 3 >= n)
                break;
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = 0;
}

typedef struct {
    uint64_t n;
    double mean;
    double m2;
    double min;
    double max;
} dstat_t;

static void dstat_add(dstat_t *s, double x) {
    s->n++;
    if (s->n == 1) {
        s->mean = x;
        s->m2 = 0;
        s->min = x;
        s->max = x;
        return;
    }
    double delta = x - s->mean;
    s->mean += delta / (double)s->n;
    s->m2 += delta * (x - s->mean);
    if (x < s->min)
        s->min = x;
    if (x > s->max)
        s->max = x;
}

static double bench_sqrt(double x) {
    if (x <= 0)
        return 0;
    double lo = 0;
    double hi = x < 1 ? 1 : x;
    for (int i = 0; i < 90; i++) {
        double mid = (lo + hi) / 2;
        if (mid * mid > x)
            hi = mid;
        else
            lo = mid;
    }
    return (lo + hi) / 2;
}

static double dstat_sd(const dstat_t *s) {
    return s->n ? bench_sqrt(s->m2 / (double)s->n) : 0;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static double nearest_percentile(double *v, uint64_t n, unsigned pct) {
    if (!n)
        return 0;
    uint64_t idx = ((uint64_t)pct * n + 99) / 100;
    if (!idx)
        idx = 1;
    return v[idx - 1];
}

static double mib_s(uint64_t bytes, uint64_t ns) {
    return ns ? (double)bytes / (double)ns * 1000000000.0 / (double)MIB : 0;
}

/* --- global byte and wall-clock limits, safe for worker threads --- */

static pthread_mutex_t g_limit_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_reserved_bytes;
static uint64_t g_bytes_read;
static uint64_t g_total_reads;
static uint64_t g_errors;
static uint64_t g_short_reads;
static uint64_t g_start_mono;
static unsigned g_abnormal;

static void mark_abnormal(void) {
    pthread_mutex_lock(&g_limit_lock);
    g_abnormal++;
    pthread_mutex_unlock(&g_limit_lock);
}

static int limit_hit(void) {
    if (elapsed_since(g_start_mono) >= DEADLINE_NS)
        return INC_DEADLINE;
    pthread_mutex_lock(&g_limit_lock);
    int cap = g_reserved_bytes >= BYTE_CAP;
    pthread_mutex_unlock(&g_limit_lock);
    return cap ? INC_BYTE_CAP : INC_NONE;
}

static int reserve_read(uint64_t want) {
    pthread_mutex_lock(&g_limit_lock);
    int rc = INC_NONE;
    if (elapsed_since(g_start_mono) >= DEADLINE_NS)
        rc = INC_DEADLINE;
    else if (want > BYTE_CAP || g_reserved_bytes > BYTE_CAP - want)
        rc = INC_BYTE_CAP;
    else
        g_reserved_bytes += want;
    pthread_mutex_unlock(&g_limit_lock);
    return rc;
}

static void account_read(uint64_t bytes, int error, int short_read) {
    pthread_mutex_lock(&g_limit_lock);
    g_bytes_read += bytes;
    g_total_reads++;
    g_errors += error ? 1 : 0;
    g_short_reads += short_read ? 1 : 0;
    pthread_mutex_unlock(&g_limit_lock);
}

/* --- 16 KiB granule uniqueness bitmap: normal cache is never purged --- */

typedef struct {
    uint8_t *bits;
    uint64_t granules;
    uint64_t bytes;
    int enabled;
} bitmap_t;

static void bitmap_init(bitmap_t *bm, uint64_t size) {
    memset(bm, 0, sizeof(*bm));
    bm->granules = (size + GRANULE - 1) / GRANULE;
    bm->bytes = (bm->granules + 7) / 8;
    if (!bm->bytes)
        return;
    bm->bits = mem_alloc(bm->bytes);
    bm->enabled = bm->bits != NULL;
}

static void bitmap_free(bitmap_t *bm) {
    if (bm->bits)
        mem_free(bm->bits);
    memset(bm, 0, sizeof(*bm));
}

static int bitmap_seen(const bitmap_t *bm, uint64_t off, uint64_t len) {
    if (!bm->enabled || !len)
        return 0;
    if (off > UINT64_MAX - (len - 1))
        return 1;
    uint64_t first = off / GRANULE;
    uint64_t last = (off + len - 1) / GRANULE;
    if (last >= bm->granules)
        return 1;
    for (uint64_t g = first; g <= last; g++)
        if ((bm->bits[g >> 3] >> (g & 7)) & 1)
            return 1;
    return 0;
}

static void bitmap_mark(bitmap_t *bm, uint64_t off, uint64_t len) {
    if (!bm->enabled || !len || off > UINT64_MAX - (len - 1))
        return;
    uint64_t first = off / GRANULE;
    uint64_t last = (off + len - 1) / GRANULE;
    if (last >= bm->granules)
        return;
    for (uint64_t g = first; g <= last; g++)
        bm->bits[g >> 3] |= (uint8_t)(1U << (g & 7));
}

static int bitmap_reserve(bitmap_t *bm, uint64_t off, uint64_t len) {
    if (!bm->enabled)
        return 1;
    if (bitmap_seen(bm, off, len))
        return 0;
    bitmap_mark(bm, off, len);
    return 1;
}

/* --- files and statfs metadata, all O_RDONLY --- */

typedef struct {
    char path[PATH_CAP];
    int fd;
    int symlink;
    int regular;
    int statfs_ok;
    struct stat st;
    struct statfs sfs;
} input_t;

static int open_input(const char *path, const char *role, input_t *in, int reject_symlink) {
    memset(in, 0, sizeof(*in));
    in->fd = -1;
    snprintf(in->path, sizeof(in->path), "%s", path);
    struct stat lst;
    memset(&lst, 0, sizeof(lst));
    int have_lstat = lstat(path, &lst) == 0;
    in->symlink = have_lstat && S_ISLNK(lst.st_mode);
    int err = 0;
    if (!(reject_symlink && in->symlink)) {
        in->fd = open(path, O_RDONLY);
        err = in->fd < 0 ? errno : 0;
    } else {
        err = EMLINK;
    }
    if (in->fd >= 0) {
        if (fstat(in->fd, &in->st))
            err = errno;
        in->regular = S_ISREG(in->st.st_mode);
        in->statfs_ok = statfs(path, &in->sfs) == 0;
    }
    char ep[1024], efs[128], emnton[160], emntfrom[160];
    esc(path, ep, sizeof(ep));
    esc(in->statfs_ok ? in->sfs.f_fstypename : "unknown", efs, sizeof(efs));
    esc(in->statfs_ok ? in->sfs.f_mntonname : "unknown", emnton, sizeof(emnton));
    esc(in->statfs_ok ? in->sfs.f_mntfromname : "unknown", emntfrom, sizeof(emntfrom));
    benchf("file role=%s path=%s open=%d regular=%d symlink=%d errno=%d size=%llu dev=%llu ino=%llu statfs=%d fs=%s mnton=%s mntfrom=%s",
           role, ep, in->fd >= 0, in->regular, in->symlink, err,
           (unsigned long long)(in->fd >= 0 ? in->st.st_size : 0),
           (unsigned long long)(in->fd >= 0 ? in->st.st_dev : 0),
           (unsigned long long)(in->fd >= 0 ? in->st.st_ino : 0),
           in->statfs_ok, efs, emnton, emntfrom);
    if (in->fd >= 0 && !in->regular) {
        close(in->fd);
        in->fd = -1;
    }
    return in->fd >= 0 ? 0 : -1;
}

static void close_input(input_t *in) {
    if (in->fd >= 0)
        close(in->fd);
    in->fd = -1;
}

/* --- exact logical pread timing; errors are never retried --- */

typedef struct {
    uint64_t bytes;
    uint64_t ns;
    int error;
    int errnum;
    int short_read;
} readres_t;

static readres_t timed_pread(int fd, void *buf, size_t len, uint64_t off) {
    readres_t rr;
    memset(&rr, 0, sizeof(rr));
    uint64_t start = mono_ns();
    size_t done = 0;
    while (done < len) {
        ssize_t r = pread(fd, (uint8_t *)buf + done, len - done, (off_t)(off + done));
        if (r < 0) {
            rr.error = 1;
            rr.errnum = errno;
            break;
        }
        if (!r)
            break;
        done += (size_t)r;
    }
    rr.bytes = done;
    rr.short_read = done < len;
    rr.ns = elapsed_since(start);
    account_read(rr.bytes, rr.error, rr.short_read);
    return rr;
}

typedef struct {
    uint64_t bytes;
    uint64_t ns;
    unsigned reads;
    unsigned errors;
    unsigned short_reads;
    int incomplete;
    int errnum;
} seqres_t;

static void measure_seq(input_t *in, void *buf, uint64_t off, uint64_t len, size_t block, seqres_t *res) {
    memset(res, 0, sizeof(*res));
    uint64_t start = mono_ns();
    while (res->bytes < len) {
        uint64_t left = len - res->bytes;
        size_t want = left < block ? (size_t)left : block;
        int lim = reserve_read(want);
        if (lim) {
            res->incomplete = lim;
            break;
        }
        readres_t rr = timed_pread(in->fd, buf, want, off + res->bytes);
        res->reads++;
        res->bytes += rr.bytes;
        res->errors += rr.error ? 1 : 0;
        res->short_reads += rr.short_read ? 1 : 0;
        res->errnum = rr.errnum;
        if (rr.error) {
            res->incomplete = INC_ERROR;
            break;
        }
        if (rr.short_read) {
            res->incomplete = INC_SHORT_READ;
            break;
        }
    }
    res->ns = elapsed_since(start);
}

/* --- deterministic splitmix64 random offsets --- */

typedef struct {
    uint64_t s;
} rng_t;

static uint64_t rng_next(rng_t *r) {
    uint64_t z = (r->s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static int random_offset(input_t *in, bitmap_t *bm, rng_t *rng, uint64_t block, int require_unique,
                         uint64_t *out, unsigned *collisions) {
    if ((uint64_t)in->st.st_size < block)
        return -1;
    uint64_t slots = ((uint64_t)in->st.st_size - block) / GRANULE + 1;
    for (unsigned attempt = 0; attempt < 4096; attempt++) {
        uint64_t off = (rng_next(rng) % slots) * GRANULE;
        if (!require_unique || !bm->enabled || !bitmap_seen(bm, off, block)) {
            if (require_unique && bm->enabled)
                bitmap_mark(bm, off, block);
            *out = off;
            return 0;
        }
        (*collisions)++;
    }
    return -1;
}

/* --- USB layout: disjoint windows are reserved before any random reads --- */

typedef struct {
    uint64_t seq_base[3];
    uint64_t conc_base;
    uint64_t cmp_base;
    int seq_ok;
    int conc_ok;
    int cmp_ok;
} usb_layout_t;

static int find_free_span(bitmap_t *bm, uint64_t size, uint64_t span, uint64_t align, unsigned pct, uint64_t *out) {
    if (size < span)
        return 0;
    uint64_t last = size - span;
    uint64_t hint = (last * pct / 100) & ~(align - 1);
    for (int pass = 0; pass < 2; pass++) {
        uint64_t pos = pass ? 0 : hint;
        while (pos <= last) {
            if (!bm->enabled || !bitmap_seen(bm, pos, span)) {
                *out = pos;
                return 1;
            }
            if (pos > last - align)
                break;
            pos += align;
        }
    }
    return 0;
}

static void plan_usb_layout(input_t *usb, bitmap_t *bm, usb_layout_t *lay) {
    memset(lay, 0, sizeof(*lay));
    uint64_t size = (uint64_t)usb->st.st_size;
    const uint64_t seq_span = 4 * 32 * MIB;
    static const unsigned pct[3] = {5, 50, 85};
    int seq_size_ok = size >= seq_span + MIB;
    lay->seq_ok = seq_size_ok;
    if (seq_size_ok) {
        for (int i = 0; i < 3; i++) {
            uint64_t avail = size - seq_span - MIB;
            lay->seq_base[i] = ((avail * pct[i] / 100) & ~(MIB - 1));
            if (!bitmap_reserve(bm, lay->seq_base[i], seq_span))
                lay->seq_ok = 0;
        }
    }
    const uint64_t conc_span = 9 * 64 * MIB;
    lay->conc_ok = find_free_span(bm, size, conc_span, MIB, 20, &lay->conc_base);
    if (lay->conc_ok && bm->enabled) {
        if (!bitmap_reserve(bm, lay->conc_base, conc_span))
            lay->conc_ok = 0;
    }
    const uint64_t cmp_span = 8 * 32 * MIB;
    lay->cmp_ok = find_free_span(bm, size, cmp_span, MIB, 65, &lay->cmp_base);
    if (lay->cmp_ok && bm->enabled) {
        if (!bitmap_reserve(bm, lay->cmp_base, cmp_span))
            lay->cmp_ok = 0;
    }
    benchf("layout bitmap=%d bitmap_bytes=%llu seq_ok=%d conc_ok=%d cmp_ok=%d seq_lo_base=%llu seq_mid_base=%llu seq_hi_base=%llu conc_base=%llu cmp_base=%llu",
           bm->enabled, (unsigned long long)bm->bytes, lay->seq_ok, lay->conc_ok, lay->cmp_ok,
           (unsigned long long)lay->seq_base[0], (unsigned long long)lay->seq_base[1],
           (unsigned long long)lay->seq_base[2], (unsigned long long)lay->conc_base,
           (unsigned long long)lay->cmp_base);
}

/* --- sequential throughput tests --- */

static void print_seq_rep(const char *test, const char *region, int rep, const char *path_role,
                          uint64_t off, const seqres_t *r, const char *cache, int unique, int scaled) {
    benchf("rep test=%s region=%s rep=%d path_role=%s offset=%llu bytes=%llu reads=%u ns=%llu mib_s=%.3f errors=%u short_reads=%u cache=%s unique=%d scaled=%d incomplete=%s errno=%d",
           test, region, rep, path_role, (unsigned long long)off, (unsigned long long)r->bytes,
           r->reads, (unsigned long long)r->ns, mib_s(r->bytes, r->ns), r->errors,
           r->short_reads, cache, unique, scaled, inc_name(r->incomplete), r->errnum);
}

static void print_rate_summary(const char *test, const char *region, const char *path_role, int block,
                               const dstat_t *st, uint64_t bytes, unsigned errors, unsigned short_reads,
                               const char *cache, int unique, int scaled) {
    benchf("summary test=%s region=%s path_role=%s block=%d samples=%llu bytes=%llu mib_s_mean=%.3f mib_s_sd=%.3f mib_s_min=%.3f mib_s_max=%.3f errors=%u short_reads=%u cache=%s unique=%d scaled=%d",
           test, region, path_role, block, (unsigned long long)st->n, (unsigned long long)bytes,
           st->mean, dstat_sd(st), st->n ? st->min : 0, st->n ? st->max : 0,
           errors, short_reads, cache, unique, scaled);
}

static void test_usb_seq(input_t *usb, bitmap_t *bm, const usb_layout_t *lay, void *buf) {
    static const char *names[3] = {"lo", "mid", "hi"};
    const char *cache = bm->enabled ? "unique16k" : "coldish_unproven";
    if (!lay->seq_ok) {
        benchf("skip test=usb_seq reason=layout_unavailable");
        return;
    }
    for (int region = 0; region < 3; region++) {
        dstat_t st;
        memset(&st, 0, sizeof(st));
        uint64_t bytes = 0;
        unsigned errors = 0, shorts = 0;
        for (int rep = 0; rep < 4; rep++) {
            if (limit_hit())
                break;
            uint64_t off = lay->seq_base[region] + (uint64_t)rep * 32 * MIB;
            seqres_t r;
            measure_seq(usb, buf, off, 32 * MIB, MIB, &r);
            print_seq_rep("usb_seq", names[region], rep, "usb_patch", off, &r, cache, bm->enabled, 0);
            bytes += r.bytes;
            errors += r.errors;
            shorts += r.short_reads;
            if (!r.incomplete && r.bytes == 32 * MIB)
                dstat_add(&st, mib_s(r.bytes, r.ns));
            if (r.incomplete == INC_DEADLINE || r.incomplete == INC_BYTE_CAP)
                break;
            if (r.incomplete)
                mark_abnormal();
        }
        print_rate_summary("usb_seq", names[region], "usb_patch", (int)MIB, &st, bytes, errors, shorts, cache, bm->enabled, 0);
    }
}

/* --- pseudo-random block tests with per-read latencies --- */

static void run_random_test(input_t *in, bitmap_t *bm, rng_t *rng, void *buf, const char *test,
                            uint64_t block, int reps, int reads_per_rep, int require_unique,
                            const char *cache_unique, const char *cache_fallback) {
    double lat[MAX_LAT_SAMPLES];
    unsigned latn = 0;
    dstat_t latst, ratest;
    memset(&latst, 0, sizeof(latst));
    memset(&ratest, 0, sizeof(ratest));
    unsigned collisions = 0, errors = 0, shorts = 0;
    uint64_t bytes = 0, reads_total = 0;
    int incomplete = INC_NONE;
    int unique = require_unique && bm->enabled;
    const char *cache = unique ? cache_unique : cache_fallback;
    if ((uint64_t)in->st.st_size < block) {
        benchf("skip test=%s block=%llu reason=file_too_small cache=%s unique=%d",
               test, (unsigned long long)block, cache, unique);
        return;
    }

    for (int rep = 0; rep < reps; rep++) {
        if ((incomplete = limit_hit()))
            break;
        uint64_t rep_bytes = 0, rep_io_ns = 0;
        unsigned rep_reads = 0, rep_errors = 0, rep_shorts = 0;
        int rep_inc = INC_NONE;
        int rep_errno = 0;
        for (int i = 0; i < reads_per_rep; i++) {
            uint64_t off;
            if (random_offset(in, bm, rng, block, unique, &off, &collisions)) {
                rep_inc = INC_UNIQUE_EXHAUSTED;
                break;
            }
            int lim = reserve_read(block);
            if (lim) {
                rep_inc = lim;
                break;
            }
            readres_t rr = timed_pread(in->fd, buf, (size_t)block, off);
            rep_reads++;
            rep_bytes += rr.bytes;
            rep_io_ns += rr.ns;
            rep_errors += rr.error ? 1 : 0;
            rep_shorts += rr.short_read ? 1 : 0;
            rep_errno = rr.errnum;
            if (!rr.error && latn < MAX_LAT_SAMPLES) {
                double us = (double)rr.ns / 1000.0;
                lat[latn++] = us;
                dstat_add(&latst, us);
            }
            if (rr.error) {
                rep_inc = INC_ERROR;
                break;
            }
            if (rr.short_read) {
                rep_inc = INC_SHORT_READ;
                break;
            }
        }
        uint64_t rep_ns = rep_io_ns;
        bytes += rep_bytes;
        reads_total += rep_reads;
        errors += rep_errors;
        shorts += rep_shorts;
        if (rep_inc)
            incomplete = rep_inc;
        benchf("rep test=%s block=%llu rep=%d reads=%u bytes=%llu ns=%llu mib_s=%.3f errors=%u short_reads=%u cache=%s unique=%d incomplete=%s errno=%d",
               test, (unsigned long long)block, rep, rep_reads, (unsigned long long)rep_bytes,
               (unsigned long long)rep_ns, mib_s(rep_bytes, rep_ns), rep_errors, rep_shorts,
               cache, unique, inc_name(rep_inc), rep_errno);
        if (!rep_inc && rep_reads == (unsigned)reads_per_rep)
            dstat_add(&ratest, mib_s(rep_bytes, rep_ns));
        if (rep_inc == INC_DEADLINE || rep_inc == INC_BYTE_CAP)
            break;
    }

    qsort(lat, latn, sizeof(lat[0]), cmp_double);
    benchf("summary test=%s block=%llu samples=%llu bytes=%llu reads=%llu lat_mean_us=%.3f lat_sd_us=%.3f lat_p50_us=%.3f lat_p95_us=%.3f lat_min_us=%.3f lat_max_us=%.3f mib_s_mean=%.3f mib_s_sd=%.3f mib_s_min=%.3f mib_s_max=%.3f errors=%u short_reads=%u rng_collisions=%u cache=%s unique=%d incomplete=%s",
           test, (unsigned long long)block, (unsigned long long)latst.n, (unsigned long long)bytes,
           (unsigned long long)reads_total, latst.mean, dstat_sd(&latst),
           nearest_percentile(lat, latn, 50), nearest_percentile(lat, latn, 95),
           latst.n ? latst.min : 0, latst.n ? latst.max : 0,
           ratest.mean, dstat_sd(&ratest), ratest.n ? ratest.min : 0, ratest.n ? ratest.max : 0,
           errors, shorts, collisions, cache, unique, inc_name(incomplete));
    if (incomplete)
        mark_abnormal();
}

static void test_usb_random(input_t *usb, bitmap_t *bm, rng_t *rng, void *buf) {
    static const struct {
        uint64_t block;
        int reps;
        int reads;
    } matrix[4] = {
        {4 * KIB, 4, 96},
        {64 * KIB, 4, 64},
        {MIB, 4, 24},
        {8 * MIB, 4, 6},
    };
    for (int i = 0; i < 4; i++) {
        if (limit_hit())
            break;
        run_random_test(usb, bm, rng, buf, "usb_rand", matrix[i].block, matrix[i].reps, matrix[i].reads,
                        1, "unique16k", "coldish_unproven");
    }
}

/* --- truly concurrent interleaved streams --- */

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_barrier_t barrier;
    int ready;
    int abort;
    int barrier_ok;
} stream_ctl_t;

typedef struct {
    const char *path;
    stream_ctl_t *ctl;
    void *buf;
    int stream;
    int streams;
    uint64_t base;
    uint64_t blocks;
    uint64_t block;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t bytes;
    unsigned reads;
    unsigned errors;
    unsigned short_reads;
    unsigned open_error;
    int incomplete;
    int errnum;
} stream_worker_t;

typedef struct {
    int valid;
    int incomplete;
    uint64_t bytes;
    uint64_t wall_ns;
    double rate;
    unsigned errors;
    unsigned short_reads;
} stream_round_result_t;

static void *stream_worker(void *argp) {
    stream_worker_t *w = argp;
    pthread_mutex_lock(&w->ctl->lock);
    while (!w->ctl->ready)
        pthread_cond_wait(&w->ctl->cond, &w->ctl->lock);
    int abort = w->ctl->abort || !w->ctl->barrier_ok;
    pthread_mutex_unlock(&w->ctl->lock);
    if (abort)
        return NULL;

    int fd = open(w->path, O_RDONLY);
    if (fd < 0) {
        w->open_error = 1;
        w->errnum = errno;
        w->incomplete = INC_ERROR;
    }
    int brc = pthread_barrier_wait(&w->ctl->barrier);
    w->start_ns = mono_ns();
    if (brc != 0 && brc != PTHREAD_BARRIER_SERIAL_THREAD) {
        w->incomplete = INC_THREAD_ERROR;
        w->end_ns = w->start_ns;
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    if (fd >= 0) {
        for (uint64_t idx = (uint64_t)w->stream; idx < w->blocks; idx += (uint64_t)w->streams) {
            int lim = reserve_read(w->block);
            if (lim) {
                w->incomplete = lim;
                break;
            }
            readres_t rr = timed_pread(fd, w->buf, (size_t)w->block, w->base + idx * w->block);
            w->reads++;
            w->bytes += rr.bytes;
            w->errors += rr.error ? 1 : 0;
            w->short_reads += rr.short_read ? 1 : 0;
            w->errnum = rr.errnum;
            if (rr.error) {
                w->incomplete = INC_ERROR;
                break;
            }
            if (rr.short_read) {
                w->incomplete = INC_SHORT_READ;
                break;
            }
        }
    }
    w->end_ns = mono_ns();
    if (fd >= 0)
        close(fd);
    return NULL;
}

static int worker_create(pthread_t *tid, stream_worker_t *w) {
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc)
        return rc;
    rc = pthread_attr_setstacksize(&attr, THREAD_STACK);
    if (!rc)
        rc = pthread_create(tid, &attr, stream_worker, w);
    pthread_attr_destroy(&attr);
    return rc;
}

static void run_stream_round(input_t *usb, void **bufs, int streams, int rep, int round_index,
                             uint64_t base, const char *cache, int unique,
                             stream_round_result_t *result) {
    memset(result, 0, sizeof(*result));
    stream_ctl_t ctl;
    stream_worker_t w[4];
    pthread_t tid[4];
    memset(&ctl, 0, sizeof(ctl));
    memset(w, 0, sizeof(w));

    int mutex_ok = pthread_mutex_init(&ctl.lock, NULL) == 0;
    int cond_ok = mutex_ok && pthread_cond_init(&ctl.cond, NULL) == 0;
    int created = 0;
    if (cond_ok) {
        for (int i = 0; i < streams; i++) {
            w[i].path = usb->path;
            w[i].ctl = &ctl;
            w[i].buf = bufs[i];
            w[i].stream = i;
            w[i].streams = streams;
            w[i].base = base;
            w[i].blocks = 64;
            w[i].block = MIB;
            if (worker_create(&tid[i], &w[i]))
                break;
            created++;
        }
        pthread_mutex_lock(&ctl.lock);
        if (created == streams && pthread_barrier_init(&ctl.barrier, NULL, (unsigned)streams) == 0)
            ctl.barrier_ok = 1;
        else
            ctl.abort = 1;
        ctl.ready = 1;
        pthread_cond_broadcast(&ctl.cond);
        pthread_mutex_unlock(&ctl.lock);
    }
    for (int i = 0; i < created; i++)
        pthread_join(tid[i], NULL);

    if (!cond_ok || created != streams || !ctl.barrier_ok) {
        benchf("round test=usb_conc streams=%d rep=%d round_index=%d thread_error=1 incomplete=thread_error",
               streams, rep, round_index);
        result->incomplete = INC_THREAD_ERROR;
        mark_abnormal();
    } else {
        uint64_t start = UINT64_MAX, end = 0, bytes = 0;
        unsigned reads = 0, errors = 0, shorts = 0, opens = 0;
        int incomplete = INC_NONE, errnum = 0;
        uint64_t sbytes[4] = {0, 0, 0, 0};
        for (int i = 0; i < streams; i++) {
            if (w[i].start_ns < start)
                start = w[i].start_ns;
            if (w[i].end_ns > end)
                end = w[i].end_ns;
            bytes += w[i].bytes;
            reads += w[i].reads;
            errors += w[i].errors;
            shorts += w[i].short_reads;
            opens += w[i].open_error;
            sbytes[i] = w[i].bytes;
            if (w[i].incomplete && !incomplete)
                incomplete = w[i].incomplete;
            if (w[i].errnum)
                errnum = w[i].errnum;
        }
        uint64_t wall = end > start ? end - start : 0;
        benchf("round test=usb_conc streams=%d rep=%d round_index=%d offset=%llu bytes=%llu reads=%u wall_ns=%llu mib_s=%.3f s0_bytes=%llu s1_bytes=%llu s2_bytes=%llu s3_bytes=%llu errors=%u short_reads=%u open_errors=%u cache=%s unique=%d incomplete=%s errno=%d",
               streams, rep, round_index, (unsigned long long)base, (unsigned long long)bytes, reads,
               (unsigned long long)wall, mib_s(bytes, wall),
               (unsigned long long)sbytes[0], (unsigned long long)sbytes[1],
               (unsigned long long)sbytes[2], (unsigned long long)sbytes[3],
               errors, shorts, opens, cache, unique, inc_name(incomplete), errnum);
        result->bytes = bytes;
        result->wall_ns = wall;
        result->rate = mib_s(bytes, wall);
        result->errors = errors;
        result->short_reads = shorts;
        result->incomplete = incomplete;
        result->valid = !incomplete && !errors && !shorts && !opens && bytes == 64 * MIB;
        if (incomplete)
            mark_abnormal();
    }
    if (ctl.barrier_ok)
        pthread_barrier_destroy(&ctl.barrier);
    if (cond_ok)
        pthread_cond_destroy(&ctl.cond);
    if (mutex_ok)
        pthread_mutex_destroy(&ctl.lock);
}

static void test_usb_conc(input_t *usb, bitmap_t *bm, const usb_layout_t *lay) {
    static const int schedule[9] = {1, 2, 4, 4, 2, 1, 1, 2, 4};
    const char *cache = bm->enabled ? "unique16k" : "coldish_unproven";
    if (!lay->conc_ok) {
        benchf("skip test=usb_conc reason=layout_unavailable");
        return;
    }
    void *raw[4] = {NULL, NULL, NULL, NULL};
    void *buf[4] = {NULL, NULL, NULL, NULL};
    for (int i = 0; i < 4; i++) {
        raw[i] = mem_alloc(MIB + GRANULE);
        if (!raw[i]) {
            benchf("skip test=usb_conc reason=no_memory");
            for (int j = 0; j < i; j++)
                mem_free(raw[j]);
            return;
        }
        uintptr_t p = (uintptr_t)raw[i];
        buf[i] = (void *)((p + GRANULE - 1) & ~(uintptr_t)(GRANULE - 1));
        memset(buf[i], 0, MIB);
    }

    dstat_t stats[5];
    uint64_t sum_bytes[5] = {0};
    unsigned sum_errors[5] = {0}, sum_shorts[5] = {0};
    int reps[5] = {0};
    memset(stats, 0, sizeof(stats));
    for (int round = 0; round < 9; round++) {
        if (limit_hit())
            break;
        int streams = schedule[round];
        int rep = reps[streams]++;
        uint64_t off = lay->conc_base + (uint64_t)round * 64 * MIB;
        stream_round_result_t rr;
        run_stream_round(usb, buf, streams, rep, round, off, cache, bm->enabled, &rr);
        sum_bytes[streams] += rr.bytes;
        sum_errors[streams] += rr.errors;
        sum_shorts[streams] += rr.short_reads;
        if (rr.valid)
            dstat_add(&stats[streams], rr.rate);
        if (rr.incomplete == INC_DEADLINE || rr.incomplete == INC_BYTE_CAP)
            break;
    }
    for (int streams = 1; streams <= 4; streams++) {
        if (!reps[streams])
            continue;
        benchf("summary test=usb_conc streams=%d samples=%llu bytes=%llu mib_s_mean=%.3f mib_s_sd=%.3f mib_s_min=%.3f mib_s_max=%.3f errors=%u short_reads=%u cache=%s unique=%d",
               streams, (unsigned long long)stats[streams].n, (unsigned long long)sum_bytes[streams],
               stats[streams].mean, dstat_sd(&stats[streams]),
               stats[streams].n ? stats[streams].min : 0, stats[streams].n ? stats[streams].max : 0,
               sum_errors[streams], sum_shorts[streams], cache, bm->enabled);
    }
    for (int i = 0; i < 4; i++)
        mem_free(raw[i]);
}

/* --- paired direct/nullfs comparisons over adjacent unique windows --- */

static void print_filecmp(const char *test, const input_t *a, const input_t *b) {
    int size_equal = a->st.st_size == b->st.st_size;
    int mtime_equal = a->st.st_mtime == b->st.st_mtime;
    int devino_equal = a->st.st_dev == b->st.st_dev && a->st.st_ino == b->st.st_ino;
    benchf("filecmp test=%s size_equal=%d mtime_equal=%d devino_equal=%d direct_size=%llu mounted_size=%llu direct_dev=%llu direct_ino=%llu mounted_dev=%llu mounted_ino=%llu",
           test, size_equal, mtime_equal, devino_equal,
           (unsigned long long)a->st.st_size, (unsigned long long)b->st.st_size,
           (unsigned long long)a->st.st_dev, (unsigned long long)a->st.st_ino,
           (unsigned long long)b->st.st_dev, (unsigned long long)b->st.st_ino);
}

static void run_paired_cmp(const char *test, input_t *direct, input_t *mounted,
                           const char *direct_role, const char *mounted_role,
                           uint64_t base, uint64_t window, int reps, const char *cache,
                           int unique, void *buf) {
    dstat_t st[2];
    uint64_t bytes[2] = {0, 0};
    unsigned errors[2] = {0, 0}, shorts[2] = {0, 0};
    memset(st, 0, sizeof(st));
    for (int rep = 0; rep < reps; rep++) {
        if (limit_hit())
            break;
        int direct_first = !(rep & 1);
        for (int step = 0; step < 2; step++) {
            int use_direct = (step == 0) == direct_first;
            input_t *in = use_direct ? direct : mounted;
            const char *role = use_direct ? direct_role : mounted_role;
            int side = use_direct ? 0 : 1;
            uint64_t off = base + (uint64_t)(2 * rep + step) * window;
            seqres_t r;
            measure_seq(in, buf, off, window, MIB, &r);
            print_seq_rep(test, "paired", rep, role, off, &r, cache, unique, 0);
            bytes[side] += r.bytes;
            errors[side] += r.errors;
            shorts[side] += r.short_reads;
            if (!r.incomplete && r.bytes == window)
                dstat_add(&st[side], mib_s(r.bytes, r.ns));
            if (r.incomplete == INC_DEADLINE || r.incomplete == INC_BYTE_CAP)
                break;
            if (r.incomplete)
                mark_abnormal();
        }
    }
    print_rate_summary(test, "paired", direct_role, (int)MIB, &st[0], bytes[0], errors[0], shorts[0], cache, unique, 0);
    print_rate_summary(test, "paired", mounted_role, (int)MIB, &st[1], bytes[1], errors[1], shorts[1], cache, unique, 0);
}

static void test_patch_cmp(input_t *usb, bitmap_t *bm, const usb_layout_t *lay, void *buf) {
    if (!lay->cmp_ok) {
        benchf("skip test=patch_cmp reason=layout_unavailable");
        return;
    }
    input_t mnt;
    if (open_input(MNT_PATCH, "user_patch", &mnt, 0)) {
        benchf("skip test=patch_cmp reason=no_user_patch_mount");
        return;
    }
    print_filecmp("patch_cmp", usb, &mnt);
    if (usb->st.st_size != mnt.st.st_size) {
        benchf("skip test=patch_cmp reason=size_mismatch");
        close_input(&mnt);
        return;
    }
    const char *cache = bm->enabled ? "adjacent_unique" : "coldish_unproven";
    run_paired_cmp("patch_cmp", usb, &mnt, "usb_patch", "user_patch", lay->cmp_base, 32 * MIB, 4,
                   cache, bm->enabled, buf);
    close_input(&mnt);
}

/* Optional addcont labels are compile-time inputs because the exact label paths are unknown. */
static void test_addcont_cmp(void *buf) {
    if (!BENCH_AC_USB || !BENCH_AC_MNT) {
        benchf("skip test=addcont_cmp reason=no_addcont_pair");
        return;
    }
    input_t usb, mnt;
    if (open_input(BENCH_AC_USB, "ac_usb", &usb, 0)) {
        benchf("skip test=addcont_cmp reason=no_ac_usb");
        return;
    }
    if (open_input(BENCH_AC_MNT, "ac_mnt", &mnt, 0)) {
        benchf("skip test=addcont_cmp reason=no_ac_mount");
        close_input(&usb);
        return;
    }
    print_filecmp("addcont_cmp", &usb, &mnt);
    if (usb.st.st_size != mnt.st.st_size) {
        benchf("skip test=addcont_cmp reason=size_mismatch");
        close_input(&mnt);
        close_input(&usb);
        return;
    }
    uint64_t size = (uint64_t)usb.st.st_size;
    uint64_t window = 32 * MIB;
    while (window > 8 * MIB && window * 8 > size)
        window /= 2;
    if (window < 8 * MIB || window * 8 > size) {
        benchf("skip test=addcont_cmp reason=addcont_too_small");
        close_input(&mnt);
        close_input(&usb);
        return;
    }
    bitmap_t bm;
    bitmap_init(&bm, size);
    uint64_t base = 0;
    int ok = find_free_span(&bm, size, window * 8, MIB, 20, &base);
    if (ok && bm.enabled)
        ok = bitmap_reserve(&bm, base, window * 8);
    if (!ok) {
        benchf("skip test=addcont_cmp reason=layout_unavailable");
    } else {
        const char *cache = bm.enabled ? "adjacent_unique" : "coldish_unproven";
        run_paired_cmp("addcont_cmp", &usb, &mnt, "ac_usb", "ac_mnt", base, window, 4,
                       cache, bm.enabled, buf);
    }
    bitmap_free(&bm);
    close_input(&mnt);
    close_input(&usb);
}

/* --- internal control selection: preferred existing pkg, bounded scan, app.db last --- */

typedef struct {
    input_t in;
    const char *source;
    const char *kind;
    int appdb;
    int selected;
} control_t;

static const char *control_pre_reason(const char *path, int have_usb, uint64_t usb_dev, struct stat *lst) {
    if (lstat(path, lst))
        return "missing";
    if (S_ISLNK(lst->st_mode))
        return "symlink";
    if (!S_ISREG(lst->st_mode))
        return "not_regular";
    if (have_usb && (uint64_t)lst->st_dev == usb_dev)
        return "same_dev_as_usb";
    return NULL;
}

static int control_try(const char *path, const char *source, int have_usb, uint64_t usb_dev, control_t *ctl) {
    struct stat lst;
    memset(&lst, 0, sizeof(lst));
    const char *reason = control_pre_reason(path, have_usb, usb_dev, &lst);
    char ep[1024];
    esc(path, ep, sizeof(ep));
    if (reason) {
        benchf("control_candidate source=%s path=%s result=rejected reason=%s dev=%llu",
               source, ep, reason, (unsigned long long)lst.st_dev);
        return 0;
    }
    if (open_input(path, "internal_control", &ctl->in, 1) || !ctl->in.statfs_ok) {
        if (ctl->in.fd >= 0)
            close_input(&ctl->in);
        benchf("control_candidate source=%s path=%s result=rejected reason=open_or_statfs dev=%llu",
               source, ep, (unsigned long long)lst.st_dev);
        return 0;
    }
    benchf("control_candidate source=%s path=%s result=accepted reason=none dev=%llu statfs=1",
           source, ep, (unsigned long long)lst.st_dev);
    ctl->source = source;
    ctl->selected = 1;
    return 1;
}

static int has_pkg_suffix(const char *name) {
    size_t n = strlen(name);
    return n > 4 && strcmp(name + n - 4, ".pkg") == 0;
}

typedef struct {
    char best[PATH_CAP];
    off_t best_size;
    int have_best;
    unsigned entries;
} control_scan_t;

static void control_scan_dir(const char *path, int depth, control_scan_t *sc, int have_usb, uint64_t usb_dev) {
    if (depth > 2 || sc->entries >= 256)
        return;
    DIR *d = opendir(path);
    if (!d)
        return;
    struct dirent *de;
    while (sc->entries < 256 && (de = readdir(d))) {
        sc->entries++;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        char p[PATH_CAP];
        int n = snprintf(p, sizeof(p), "%s/%s", path, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(p))
            continue;
        struct stat st;
        if (lstat(p, &st))
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 2)
                control_scan_dir(p, depth + 1, sc, have_usb, usb_dev);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !has_pkg_suffix(de->d_name))
            continue;
        if (have_usb && (uint64_t)st.st_dev == usb_dev)
            continue;
        if (!sc->have_best || st.st_size > sc->best_size) {
            snprintf(sc->best, sizeof(sc->best), "%s", p);
            sc->best_size = st.st_size;
            sc->have_best = 1;
        }
    }
    closedir(d);
}

static int control_select(control_t *ctl, int have_usb, uint64_t usb_dev) {
    memset(ctl, 0, sizeof(*ctl));
    ctl->in.fd = -1;
    if (control_try(INTERNAL_PREFERRED, "preferred_user_app", have_usb, usb_dev, ctl)) {
        ctl->kind = "internal_pkg";
        return 1;
    }

    control_scan_t sc;
    memset(&sc, 0, sizeof(sc));
    control_scan_dir("/data/PS4Mount", 0, &sc, have_usb, usb_dev);
    control_scan_dir("/data/ps4", 0, &sc, have_usb, usb_dev);
    char ep[1024];
    esc(sc.have_best ? sc.best : "none", ep, sizeof(ep));
    benchf("control_scan entries=%u candidate=%d path=%s size=%llu",
           sc.entries, sc.have_best, ep, (unsigned long long)(sc.have_best ? sc.best_size : 0));
    if (sc.have_best && control_try(sc.best, "scan_data", have_usb, usb_dev, ctl)) {
        ctl->kind = "internal_pkg";
        return 1;
    }

    if (control_try(APPDB_FALLBACK, "appdb_last_resort", have_usb, usb_dev, ctl)) {
        ctl->kind = "appdb_fallback";
        ctl->appdb = 1;
        return 1;
    }
    return 0;
}

/* --- internal control measurements --- */

static const char *control_cache_mode(control_t *ctl, bitmap_t *bm, int *require_unique) {
    uint64_t size = (uint64_t)ctl->in.st.st_size;
    *require_unique = !ctl->appdb && size >= 64 * MIB && bm->enabled;
    if (*require_unique)
        return "unique16k";
    if (ctl->appdb || size < 64 * MIB)
        return "small_file_cache_prone";
    return "coldish_unproven";
}

static void test_control_seq(control_t *ctl, bitmap_t *bm, const char *cache, int unique, void *buf) {
    uint64_t size = (uint64_t)ctl->in.st.st_size & ~((uint64_t)4 * KIB - 1);
    if (size < 4 * KIB) {
        benchf("skip test=control_seq reason=file_too_small");
        return;
    }
    int scaled = size < 48 * MIB;
    int reps = scaled ? 1 : 3;
    uint64_t len = scaled ? size : 16 * MIB;
    uint64_t base = 0;
    if (!scaled) {
        uint64_t span = 3 * len;
        base = (((uint64_t)ctl->in.st.st_size - span) / 5) & ~(MIB - 1);
    }
    dstat_t st;
    memset(&st, 0, sizeof(st));
    uint64_t bytes = 0;
    unsigned errors = 0, shorts = 0;
    for (int rep = 0; rep < reps; rep++) {
        if (limit_hit())
            break;
        uint64_t off = base + (uint64_t)rep * len;
        if (!scaled && unique && !bitmap_reserve(bm, off, len)) {
            benchf("skip test=control_seq rep=%d reason=layout_overlap", rep);
            continue;
        }
        seqres_t r;
        measure_seq(&ctl->in, buf, off, len, MIB, &r);
        print_seq_rep("control_seq", scaled ? "scaled" : "main", rep, "internal_control", off, &r,
                      cache, unique, scaled);
        bytes += r.bytes;
        errors += r.errors;
        shorts += r.short_reads;
        if (!r.incomplete && r.bytes == len)
            dstat_add(&st, mib_s(r.bytes, r.ns));
        if (r.incomplete == INC_DEADLINE || r.incomplete == INC_BYTE_CAP)
            break;
        if (r.incomplete)
            mark_abnormal();
    }
    print_rate_summary("control_seq", scaled ? "scaled" : "main", "internal_control", (int)MIB,
                       &st, bytes, errors, shorts, cache, unique, scaled);
}

static void run_control_tests(int have_usb, uint64_t usb_dev, rng_t *rng, void *buf) {
    control_t ctl;
    if (!control_select(&ctl, have_usb, usb_dev)) {
        benchf("skip test=control reason=no_internal_control");
        return;
    }
    char ep[1024];
    esc(ctl.in.path, ep, sizeof(ep));
    benchf("control selected=1 control_kind=%s source=%s path=%s size=%llu dev=%llu ino=%llu",
           ctl.kind, ctl.source, ep, (unsigned long long)ctl.in.st.st_size,
           (unsigned long long)ctl.in.st.st_dev, (unsigned long long)ctl.in.st.st_ino);

    bitmap_t bm;
    bitmap_init(&bm, (uint64_t)ctl.in.st.st_size);
    int require_unique = 0;
    const char *cache = control_cache_mode(&ctl, &bm, &require_unique);
    test_control_seq(&ctl, &bm, cache, require_unique, buf);
    if (!limit_hit()) {
        run_random_test(&ctl.in, &bm, rng, buf, "control_rand", 4 * KIB, 3, 64,
                        require_unique, cache, cache);
    }
    if (!limit_hit()) {
        run_random_test(&ctl.in, &bm, rng, buf, "control_rand", MIB, 3, 16,
                        require_unique, cache, cache);
    }
    bitmap_free(&bm);
    close_input(&ctl.in);
}

/* --- v2 (a): multi-file concurrent matrix (patch + CODMW3CONTENT003..006) --- */

#define MF_MAX_FILES 5
#define MF_RAND_READS 32

typedef struct {
    const char *path;
    const char *role;
    stream_ctl_t *ctl;
    void *buf;
    int mode_rand;
    uint64_t seq_base;
    uint64_t seq_blocks;
    uint64_t block;
    uint64_t offs[MF_RAND_READS];
    unsigned noffs;
    uint64_t start_ns;
    uint64_t end_ns;
    uint64_t bytes;
    unsigned reads;
    unsigned errors;
    unsigned short_reads;
    unsigned open_error;
    int incomplete;
    int errnum;
    double lat_us[MF_RAND_READS];
    unsigned latn;
} mf_worker_t;

static void *mf_worker(void *argp) {
    mf_worker_t *w = argp;
    pthread_mutex_lock(&w->ctl->lock);
    while (!w->ctl->ready)
        pthread_cond_wait(&w->ctl->cond, &w->ctl->lock);
    int abort = w->ctl->abort || !w->ctl->barrier_ok;
    pthread_mutex_unlock(&w->ctl->lock);
    if (abort)
        return NULL;

    int fd = open(w->path, O_RDONLY);
    if (fd < 0) {
        w->open_error = 1;
        w->errnum = errno;
        w->incomplete = INC_ERROR;
    }
    int brc = pthread_barrier_wait(&w->ctl->barrier);
    w->start_ns = mono_ns();
    if (brc != 0 && brc != PTHREAD_BARRIER_SERIAL_THREAD) {
        w->incomplete = INC_THREAD_ERROR;
        w->end_ns = w->start_ns;
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    if (fd >= 0) {
        uint64_t count = w->mode_rand ? (uint64_t)w->noffs : w->seq_blocks;
        for (uint64_t i = 0; i < count; i++) {
            int lim = reserve_read(w->block);
            if (lim) {
                w->incomplete = lim;
                break;
            }
            uint64_t off = w->mode_rand ? w->offs[i] : (w->seq_base + i * w->block);
            readres_t rr = timed_pread(fd, w->buf, (size_t)w->block, off);
            w->reads++;
            w->bytes += rr.bytes;
            w->errors += rr.error ? 1 : 0;
            w->short_reads += rr.short_read ? 1 : 0;
            w->errnum = rr.errnum;
            if (w->mode_rand && !rr.error && w->latn < MF_RAND_READS)
                w->lat_us[w->latn++] = (double)rr.ns / 1000.0;
            if (rr.error) {
                w->incomplete = INC_ERROR;
                break;
            }
            if (rr.short_read) {
                w->incomplete = INC_SHORT_READ;
                break;
            }
        }
    }
    w->end_ns = mono_ns();
    if (fd >= 0)
        close(fd);
    return NULL;
}

static int mf_worker_create(pthread_t *tid, mf_worker_t *w) {
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc)
        return rc;
    rc = pthread_attr_setstacksize(&attr, THREAD_STACK);
    if (!rc)
        rc = pthread_create(tid, &attr, mf_worker, w);
    pthread_attr_destroy(&attr);
    return rc;
}

static void run_mf_round(const char *test, int nfiles, mf_worker_t *w, int rep, const char *cache,
                         int unique, dstat_t *ratest, dstat_t *latst, double *lat_all, unsigned *lat_all_n,
                         uint64_t *sum_bytes, unsigned *sum_errors, unsigned *sum_shorts, int *out_inc) {
    stream_ctl_t ctl;
    pthread_t tid[MF_MAX_FILES];
    memset(&ctl, 0, sizeof(ctl));
    *out_inc = INC_NONE;

    int mutex_ok = pthread_mutex_init(&ctl.lock, NULL) == 0;
    int cond_ok = mutex_ok && pthread_cond_init(&ctl.cond, NULL) == 0;
    int created = 0;
    if (cond_ok) {
        for (int i = 0; i < nfiles; i++) {
            w[i].ctl = &ctl;
            if (mf_worker_create(&tid[i], &w[i]))
                break;
            created++;
        }
        pthread_mutex_lock(&ctl.lock);
        if (created == nfiles && pthread_barrier_init(&ctl.barrier, NULL, (unsigned)nfiles) == 0)
            ctl.barrier_ok = 1;
        else
            ctl.abort = 1;
        ctl.ready = 1;
        pthread_cond_broadcast(&ctl.cond);
        pthread_mutex_unlock(&ctl.lock);
    }
    for (int i = 0; i < created; i++)
        pthread_join(tid[i], NULL);

    if (!cond_ok || created != nfiles || !ctl.barrier_ok) {
        benchf("round test=%s files=%d rep=%d thread_error=1 incomplete=thread_error", test, nfiles, rep);
        *out_inc = INC_THREAD_ERROR;
        mark_abnormal();
    } else {
        uint64_t start = UINT64_MAX, end = 0, bytes = 0;
        unsigned reads = 0, errors = 0, shorts = 0, opens = 0;
        int inc = INC_NONE, errnum = 0;
        uint64_t fbytes[MF_MAX_FILES] = {0, 0, 0, 0, 0};
        for (int i = 0; i < nfiles; i++) {
            if (w[i].start_ns < start)
                start = w[i].start_ns;
            if (w[i].end_ns > end)
                end = w[i].end_ns;
            bytes += w[i].bytes;
            reads += w[i].reads;
            errors += w[i].errors;
            shorts += w[i].short_reads;
            opens += w[i].open_error;
            fbytes[i] = w[i].bytes;
            if (w[i].incomplete && !inc)
                inc = w[i].incomplete;
            if (w[i].errnum)
                errnum = w[i].errnum;
            if (latst && lat_all && lat_all_n) {
                for (unsigned k = 0; k < w[i].latn; k++) {
                    dstat_add(latst, w[i].lat_us[k]);
                    if (*lat_all_n < MAX_LAT_SAMPLES)
                        lat_all[(*lat_all_n)++] = w[i].lat_us[k];
                }
            }
        }
        uint64_t wall = end > start ? end - start : 0;
        double rate = mib_s(bytes, wall);
        benchf("round test=%s files=%d rep=%d block=%llu bytes=%llu reads=%u wall_ns=%llu mib_s=%.3f f0_bytes=%llu f1_bytes=%llu f2_bytes=%llu f3_bytes=%llu f4_bytes=%llu errors=%u short_reads=%u open_errors=%u cache=%s unique=%d incomplete=%s errno=%d",
               test, nfiles, rep, (unsigned long long)w[0].block, (unsigned long long)bytes, reads,
               (unsigned long long)wall, rate, (unsigned long long)fbytes[0], (unsigned long long)fbytes[1],
               (unsigned long long)fbytes[2], (unsigned long long)fbytes[3], (unsigned long long)fbytes[4],
               errors, shorts, opens, cache, unique, inc_name(inc), errnum);
        *sum_bytes += bytes;
        *sum_errors += errors;
        *sum_shorts += shorts;
        *out_inc = inc;
        if (!inc && !errors && !shorts && !opens && bytes > 0)
            dstat_add(ratest, rate);
        if (inc)
            mark_abnormal();
    }
    if (ctl.barrier_ok)
        pthread_barrier_destroy(&ctl.barrier);
    if (cond_ok)
        pthread_cond_destroy(&ctl.cond);
    if (mutex_ok)
        pthread_mutex_destroy(&ctl.lock);
}

static void test_multifile_conc(input_t *usb, bitmap_t *usb_bm, rng_t *rng) {
    static const struct {
        const char *role;
        const char *path;
    } addcont[4] = {
        {"ac_003", USB_AC_003},
        {"ac_004", USB_AC_004},
        {"ac_005", USB_AC_005},
        {"ac_006", USB_AC_006},
    };
    input_t ac[4];
    int ac_open[4] = {0, 0, 0, 0};
    input_t *targets[MF_MAX_FILES];
    const char *roles[MF_MAX_FILES];
    int nfiles = 0;

    targets[nfiles] = usb;
    roles[nfiles] = "usb_patch";
    nfiles++;
    for (int i = 0; i < 4; i++) {
        memset(&ac[i], 0, sizeof(ac[i]));
        ac[i].fd = -1;
        if (open_input(addcont[i].path, addcont[i].role, &ac[i], 0) == 0 && (uint64_t)ac[i].st.st_size >= 64 * MIB) {
            ac_open[i] = 1;
            targets[nfiles] = &ac[i];
            roles[nfiles] = addcont[i].role;
            nfiles++;
        } else if (ac[i].fd >= 0) {
            close_input(&ac[i]);
        }
    }
    if (nfiles < 2) {
        benchf("skip test=multifile_conc reason=insufficient_files files=%d", nfiles);
        for (int i = 0; i < 4; i++)
            if (ac_open[i])
                close_input(&ac[i]);
        return;
    }

    void *raw[MF_MAX_FILES] = {NULL, NULL, NULL, NULL, NULL};
    void *buf[MF_MAX_FILES] = {NULL, NULL, NULL, NULL, NULL};
    for (int i = 0; i < nfiles; i++) {
        raw[i] = mem_alloc(MIB + GRANULE);
        if (!raw[i]) {
            benchf("skip test=multifile_conc reason=no_memory");
            for (int j = 0; j < i; j++)
                mem_free(raw[j]);
            for (int j = 0; j < 4; j++)
                if (ac_open[j])
                    close_input(&ac[j]);
            return;
        }
        uintptr_t p = (uintptr_t)raw[i];
        buf[i] = (void *)((p + GRANULE - 1) & ~(uintptr_t)(GRANULE - 1));
        memset(buf[i], 0, MIB);
    }

    /* Subtest 1: 3 reps x 16 MiB per file concurrent 1 MiB sequential streams */
    const uint64_t seq_win = 16 * MIB;
    uint64_t seq_base[MF_MAX_FILES] = {0};
    int seq_ok = 1;
    if (!find_free_span(usb_bm, (uint64_t)usb->st.st_size, 3 * seq_win, MIB, 35, &seq_base[0]) ||
        !bitmap_reserve(usb_bm, seq_base[0], 3 * seq_win))
        seq_ok = 0;
    for (int i = 1; i < nfiles; i++) {
        uint64_t sz = (uint64_t)targets[i]->st.st_size;
        if (sz < 3 * seq_win + MIB)
            seq_ok = 0;
        else
            seq_base[i] = (((sz - 3 * seq_win) / 5) & ~(MIB - 1));
    }
    if (!seq_ok) {
        benchf("skip test=multifile_seq1m reason=layout_unavailable files=%d", nfiles);
    } else {
        dstat_t st;
        memset(&st, 0, sizeof(st));
        uint64_t sum_bytes = 0;
        unsigned sum_errors = 0, sum_shorts = 0;
        const char *cache = usb_bm->enabled ? "unique16k" : "coldish_unproven";
        for (int rep = 0; rep < 3; rep++) {
            if (limit_hit())
                break;
            mf_worker_t w[MF_MAX_FILES];
            memset(w, 0, sizeof(w));
            for (int i = 0; i < nfiles; i++) {
                w[i].path = targets[i]->path;
                w[i].role = roles[i];
                w[i].buf = buf[i];
                w[i].mode_rand = 0;
                w[i].seq_base = seq_base[i] + (uint64_t)rep * seq_win;
                w[i].seq_blocks = 16;
                w[i].block = MIB;
            }
            int inc = INC_NONE;
            run_mf_round("multifile_seq1m", nfiles, w, rep, cache, usb_bm->enabled, &st, NULL, NULL, NULL,
                         &sum_bytes, &sum_errors, &sum_shorts, &inc);
            if (inc == INC_DEADLINE || inc == INC_BYTE_CAP)
                break;
        }
        benchf("summary test=multifile_seq1m files=%d block=%llu samples=%llu bytes=%llu mib_s_mean=%.3f mib_s_sd=%.3f mib_s_min=%.3f mib_s_max=%.3f errors=%u short_reads=%u cache=%s unique=%d",
               nfiles, (unsigned long long)MIB, (unsigned long long)st.n, (unsigned long long)sum_bytes,
               st.mean, dstat_sd(&st), st.n ? st.min : 0, st.n ? st.max : 0,
               sum_errors, sum_shorts, cache, usb_bm->enabled);
    }

    /* Subtest 2: 2 reps x 32 reads/file concurrent 64 KiB random reads */
    if (!limit_hit()) {
        dstat_t ratest, latst;
        memset(&ratest, 0, sizeof(ratest));
        memset(&latst, 0, sizeof(latst));
        double lat_all[MAX_LAT_SAMPLES];
        unsigned lat_all_n = 0;
        uint64_t sum_bytes = 0;
        unsigned sum_errors = 0, sum_shorts = 0;
        const uint64_t blk = 64 * KIB;
        const char *cache = usb_bm->enabled ? "unique16k" : "coldish_unproven";
        uint64_t prev_ac_offs[MF_MAX_FILES][2 * MF_RAND_READS];
        unsigned prev_ac_n[MF_MAX_FILES] = {0};
        for (int rep = 0; rep < 2; rep++) {
            if (limit_hit())
                break;
            mf_worker_t w[MF_MAX_FILES];
            memset(w, 0, sizeof(w));
            int offs_ok = 1;
            for (int i = 0; i < nfiles; i++) {
                w[i].path = targets[i]->path;
                w[i].role = roles[i];
                w[i].buf = buf[i];
                w[i].mode_rand = 1;
                w[i].block = blk;
                w[i].noffs = MF_RAND_READS;
                uint64_t sz = (uint64_t)targets[i]->st.st_size;
                uint64_t slots = sz >= blk ? (sz - blk) / GRANULE + 1 : 0;
                if (!slots) {
                    offs_ok = 0;
                    break;
                }
                for (unsigned k = 0; k < MF_RAND_READS; k++) {
                    if (i == 0) {
                        unsigned col = 0;
                        if (random_offset(usb, usb_bm, rng, blk, usb_bm->enabled, &w[i].offs[k], &col)) {
                            offs_ok = 0;
                            break;
                        }
                    } else {
                        int picked = 0;
                        for (unsigned att = 0; att < 1024; att++) {
                            uint64_t off = (rng_next(rng) % slots) * GRANULE;
                            /* Avoid the 3*16 MiB seq window and previously chosen add-on offsets */
                            if (seq_ok && off + blk > seq_base[i] && off < seq_base[i] + 3 * seq_win)
                                continue;
                            int dup = 0;
                            for (unsigned p = 0; p < prev_ac_n[i]; p++) {
                                if (off + blk > prev_ac_offs[i][p] && off < prev_ac_offs[i][p] + blk) {
                                    dup = 1;
                                    break;
                                }
                            }
                            if (!dup) {
                                w[i].offs[k] = off;
                                prev_ac_offs[i][prev_ac_n[i]++] = off;
                                picked = 1;
                                break;
                            }
                        }
                        if (!picked) {
                            offs_ok = 0;
                            break;
                        }
                    }
                }
                if (!offs_ok)
                    break;
            }
            if (!offs_ok) {
                benchf("skip test=multifile_rand64k rep=%d reason=offsets_unavailable", rep);
                break;
            }
            int inc = INC_NONE;
            run_mf_round("multifile_rand64k", nfiles, w, rep, cache, usb_bm->enabled, &ratest, &latst, lat_all,
                         &lat_all_n, &sum_bytes, &sum_errors, &sum_shorts, &inc);
            if (inc == INC_DEADLINE || inc == INC_BYTE_CAP)
                break;
        }
        qsort(lat_all, lat_all_n, sizeof(lat_all[0]), cmp_double);
        benchf("summary test=multifile_rand64k files=%d block=%llu samples=%llu bytes=%llu lat_mean_us=%.3f lat_sd_us=%.3f lat_p50_us=%.3f lat_p95_us=%.3f lat_min_us=%.3f lat_max_us=%.3f mib_s_mean=%.3f mib_s_sd=%.3f mib_s_min=%.3f mib_s_max=%.3f errors=%u short_reads=%u cache=%s unique=%d",
               nfiles, (unsigned long long)blk, (unsigned long long)latst.n, (unsigned long long)sum_bytes,
               latst.mean, dstat_sd(&latst), nearest_percentile(lat_all, lat_all_n, 50),
               nearest_percentile(lat_all, lat_all_n, 95), latst.n ? latst.min : 0, latst.n ? latst.max : 0,
               ratest.mean, dstat_sd(&ratest), ratest.n ? ratest.min : 0, ratest.n ? ratest.max : 0,
               sum_errors, sum_shorts, cache, usb_bm->enabled);
    }

    for (int i = 0; i < nfiles; i++)
        mem_free(raw[i]);
    for (int i = 0; i < 4; i++)
        if (ac_open[i])
            close_input(&ac[i]);
}

/* --- v2 (b): random 4K/64K paired direct /mnt/usb0 vs mounted /user/patch --- */

static void run_paired_rand_block(input_t *direct, input_t *mounted, bitmap_t *bm, rng_t *rng, void *buf,
                                  uint64_t block, int reps, int reads_per_rep) {
    double lat[2][MAX_LAT_SAMPLES];
    unsigned latn[2] = {0, 0};
    dstat_t latst[2], ratest[2];
    uint64_t bytes[2] = {0, 0}, reads_total[2] = {0, 0};
    unsigned errors[2] = {0, 0}, shorts[2] = {0, 0}, collisions = 0;
    int inc[2] = {INC_NONE, INC_NONE};
    int unique = bm->enabled;
    const char *cache = unique ? "unique16k" : "coldish_unproven";
    memset(latst, 0, sizeof(latst));
    memset(ratest, 0, sizeof(ratest));

    for (int rep = 0; rep < reps; rep++) {
        if (limit_hit())
            break;
        int direct_first = !(rep & 1);
        for (int step = 0; step < 2; step++) {
            int use_direct = (step == 0) == direct_first;
            input_t *in = use_direct ? direct : mounted;
            const char *role = use_direct ? "usb_patch" : "user_patch";
            int side = use_direct ? 0 : 1;
            uint64_t rep_bytes = 0, rep_ns = 0;
            unsigned rep_reads = 0, rep_err = 0, rep_sh = 0;
            int rep_inc = INC_NONE, rep_errno = 0;
            for (int i = 0; i < reads_per_rep; i++) {
                uint64_t off;
                if (random_offset(direct, bm, rng, block, unique, &off, &collisions)) {
                    rep_inc = INC_UNIQUE_EXHAUSTED;
                    break;
                }
                int lim = reserve_read(block);
                if (lim) {
                    rep_inc = lim;
                    break;
                }
                readres_t rr = timed_pread(in->fd, buf, (size_t)block, off);
                rep_reads++;
                rep_bytes += rr.bytes;
                rep_ns += rr.ns;
                rep_err += rr.error ? 1 : 0;
                rep_sh += rr.short_read ? 1 : 0;
                rep_errno = rr.errnum;
                if (!rr.error && latn[side] < MAX_LAT_SAMPLES) {
                    double us = (double)rr.ns / 1000.0;
                    lat[side][latn[side]++] = us;
                    dstat_add(&latst[side], us);
                }
                if (rr.error) {
                    rep_inc = INC_ERROR;
                    break;
                }
                if (rr.short_read) {
                    rep_inc = INC_SHORT_READ;
                    break;
                }
            }
            bytes[side] += rep_bytes;
            reads_total[side] += rep_reads;
            errors[side] += rep_err;
            shorts[side] += rep_sh;
            if (rep_inc && !inc[side])
                inc[side] = rep_inc;
            benchf("rep test=patch_rand_cmp block=%llu rep=%d path_role=%s reads=%u bytes=%llu ns=%llu mib_s=%.3f errors=%u short_reads=%u cache=%s unique=%d incomplete=%s errno=%d",
                   (unsigned long long)block, rep, role, rep_reads, (unsigned long long)rep_bytes,
                   (unsigned long long)rep_ns, mib_s(rep_bytes, rep_ns), rep_err, rep_sh,
                   cache, unique, inc_name(rep_inc), rep_errno);
            if (!rep_inc && rep_reads == (unsigned)reads_per_rep)
                dstat_add(&ratest[side], mib_s(rep_bytes, rep_ns));
            if (rep_inc == INC_DEADLINE || rep_inc == INC_BYTE_CAP)
                break;
            if (rep_inc)
                mark_abnormal();
        }
    }

    static const char *roles[2] = {"usb_patch", "user_patch"};
    for (int s = 0; s < 2; s++) {
        qsort(lat[s], latn[s], sizeof(lat[s][0]), cmp_double);
        benchf("summary test=patch_rand_cmp block=%llu path_role=%s samples=%llu bytes=%llu reads=%llu lat_mean_us=%.3f lat_sd_us=%.3f lat_p50_us=%.3f lat_p95_us=%.3f lat_min_us=%.3f lat_max_us=%.3f mib_s_mean=%.3f mib_s_sd=%.3f mib_s_min=%.3f mib_s_max=%.3f errors=%u short_reads=%u rng_collisions=%u cache=%s unique=%d incomplete=%s",
               (unsigned long long)block, roles[s], (unsigned long long)latst[s].n, (unsigned long long)bytes[s],
               (unsigned long long)reads_total[s], latst[s].mean, dstat_sd(&latst[s]),
               nearest_percentile(lat[s], latn[s], 50), nearest_percentile(lat[s], latn[s], 95),
               latst[s].n ? latst[s].min : 0, latst[s].n ? latst[s].max : 0,
               ratest[s].mean, dstat_sd(&ratest[s]), ratest[s].n ? ratest[s].min : 0,
               ratest[s].n ? ratest[s].max : 0, errors[s], shorts[s], collisions, cache, unique, inc_name(inc[s]));
    }
}

static void test_patch_rand_cmp(input_t *usb, bitmap_t *bm, rng_t *rng, void *buf) {
    input_t mnt;
    if (open_input(MNT_PATCH, "user_patch_rand", &mnt, 0)) {
        benchf("skip test=patch_rand_cmp reason=no_user_patch_mount");
        return;
    }
    if (usb->st.st_size != mnt.st.st_size) {
        benchf("skip test=patch_rand_cmp reason=size_mismatch");
        close_input(&mnt);
        return;
    }
    run_paired_rand_block(usb, &mnt, bm, rng, buf, 4 * KIB, 4, 64);
    if (!limit_hit())
        run_paired_rand_block(usb, &mnt, bm, rng, buf, 64 * KIB, 4, 32);
    close_input(&mnt);
}

/* --- v2 (c): scan metadata microbenchmark (stat + open + 4K pread + close) --- */

static int has_pkg_ci(const char *name) {
    size_t n = strlen(name);
    return n >= 5 && !strcasecmp(name + n - 4, ".pkg");
}

static int is_appledouble_pkg(const char *name) {
    return name[0] == '.' && name[1] == '_' && strlen(name) >= 6 && has_pkg_ci(name);
}

typedef struct {
    char (*paths)[PATH_CAP];
    unsigned count;
    unsigned cap;
    unsigned entries_seen;
} path_list_t;

static void collect_sidecars(const char *dir, path_list_t *pl) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) && pl->count < pl->cap && pl->entries_seen < 1024) {
        pl->entries_seen++;
        if (!is_appledouble_pkg(de->d_name))
            continue;
        int n = snprintf(pl->paths[pl->count], PATH_CAP, "%s/%s", dir, de->d_name);
        if (n > 0 && (size_t)n < PATH_CAP)
            pl->count++;
    }
    closedir(d);
}

static void collect_library_pkgs(const char *dir, int depth, path_list_t *pl) {
    if (depth > 4 || pl->count >= pl->cap || pl->entries_seen >= 2048)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) && pl->count < pl->cap && pl->entries_seen < 2048) {
        pl->entries_seen++;
        if (de->d_name[0] == '.')
            continue;
        char p[PATH_CAP];
        int n = snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        if (n <= 0 || (size_t)n >= sizeof(p))
            continue;
        struct stat st;
        if (lstat(p, &st))
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4)
                collect_library_pkgs(p, depth + 1, pl);
        } else if (S_ISREG(st.st_mode) && has_pkg_ci(de->d_name)) {
            snprintf(pl->paths[pl->count++], PATH_CAP, "%s", p);
        }
    }
    closedir(d);
}

static void run_meta_cohort(const char *cohort, const path_list_t *pl, int passes, void *buf) {
    if (!pl->count) {
        benchf("skip test=scan_meta cohort=%s reason=no_files", cohort);
        return;
    }
    double lat[MAX_LAT_SAMPLES];
    unsigned latn = 0;
    dstat_t latst;
    memset(&latst, 0, sizeof(latst));
    uint64_t total_bytes = 0, total_wall = 0;
    unsigned total_ops = 0, total_err = 0, total_sh = 0;
    int inc = INC_NONE;

    for (int pass = 0; pass < passes; pass++) {
        if ((inc = limit_hit()))
            break;
        uint64_t p_start = mono_ns(), p_bytes = 0;
        unsigned p_ops = 0, p_err = 0, p_sh = 0;
        for (unsigned i = 0; i < pl->count; i++) {
            int lim = reserve_read(4 * KIB);
            if (lim) {
                inc = lim;
                break;
            }
            uint64_t t0 = mono_ns();
            struct stat st;
            int err = stat(pl->paths[i], &st) != 0;
            uint64_t got = 0;
            int sh = 0;
            if (!err) {
                int fd = open(pl->paths[i], O_RDONLY);
                if (fd < 0) {
                    err = 1;
                } else {
                    size_t want = 4 * KIB;
                    while (got < want) {
                        ssize_t r = pread(fd, (uint8_t *)buf + got, want - (size_t)got, (off_t)got);
                        if (r < 0) {
                            err = 1;
                            break;
                        }
                        if (!r)
                            break;
                        got += (uint64_t)r;
                    }
                    sh = !err && got < want;
                    close(fd);
                }
            }
            uint64_t dt = elapsed_since(t0);
            account_read(got, err, sh);
            p_ops++;
            p_bytes += got;
            p_err += err ? 1 : 0;
            p_sh += sh ? 1 : 0;
            if (!err) {
                double us = (double)dt / 1000.0;
                dstat_add(&latst, us);
                if (latn < MAX_LAT_SAMPLES)
                    lat[latn++] = us;
            }
        }
        uint64_t p_wall = elapsed_since(p_start);
        double ops_s = p_wall ? (double)p_ops * 1000000000.0 / (double)p_wall : 0;
        benchf("rep test=scan_meta cohort=%s pass=%d files=%u ops=%u bytes=%llu wall_ns=%llu ops_s=%.3f errors=%u short_reads=%u incomplete=%s",
               cohort, pass, pl->count, p_ops, (unsigned long long)p_bytes, (unsigned long long)p_wall,
               ops_s, p_err, p_sh, inc_name(inc));
        total_ops += p_ops;
        total_bytes += p_bytes;
        total_wall += p_wall;
        total_err += p_err;
        total_sh += p_sh;
        if (inc)
            break;
    }

    qsort(lat, latn, sizeof(lat[0]), cmp_double);
    double total_ops_s = total_wall ? (double)total_ops * 1000000000.0 / (double)total_wall : 0;
    benchf("summary test=scan_meta cohort=%s files=%u passes=%d ops=%u bytes=%llu wall_ns=%llu ops_s=%.3f lat_mean_us=%.3f lat_sd_us=%.3f lat_p50_us=%.3f lat_p95_us=%.3f lat_min_us=%.3f lat_max_us=%.3f errors=%u short_reads=%u incomplete=%s",
           cohort, pl->count, passes, total_ops, (unsigned long long)total_bytes, (unsigned long long)total_wall,
           total_ops_s, latst.mean, dstat_sd(&latst), nearest_percentile(lat, latn, 50),
           nearest_percentile(lat, latn, 95), latst.n ? latst.min : 0, latst.n ? latst.max : 0,
           total_err, total_sh, inc_name(inc));
}

static void test_scan_meta(void *buf) {
    char (*paths)[PATH_CAP] = mem_alloc(256 * sizeof(*paths));
    if (!paths) {
        benchf("skip test=scan_meta reason=no_memory");
        return;
    }
    path_list_t pl = {paths, 0, 128, 0};
    collect_sidecars(USB_SCAN_DIR, &pl);
    run_meta_cohort("sidecars", &pl, 2, buf);

    if (!limit_hit()) {
        path_list_t lp = {paths, 0, 256, 0};
        collect_library_pkgs(USB_LIB_DIR, 0, &lp);
        run_meta_cohort("library_pkgs", &lp, 1, buf);
    }
    mem_free(paths);
}

/* --- v2 (d): warm-repeat cache contrast (exact same offsets read twice) --- */

static void run_warm_repeat_rand(input_t *usb, bitmap_t *bm, rng_t *rng, void *buf, uint64_t block) {
    enum { N = 64 };
    uint64_t offs[N];
    unsigned col = 0;
    for (int i = 0; i < N; i++) {
        if (random_offset(usb, bm, rng, block, bm->enabled, &offs[i], &col)) {
            benchf("skip test=warm_repeat mode=rand block=%llu reason=offsets_unavailable", (unsigned long long)block);
            return;
        }
    }
    double p50[2] = {0, 0}, rate[2] = {0, 0};
    for (int pass = 0; pass < 2; pass++) {
        if (limit_hit())
            break;
        const char *cache = pass == 0 ? (bm->enabled ? "unique16k_first" : "coldish_first") : "warm_repeat";
        double lat[N];
        unsigned latn = 0, errors = 0, shorts = 0, reads = 0;
        uint64_t bytes = 0, io_ns = 0;
        dstat_t latst;
        memset(&latst, 0, sizeof(latst));
        int inc = INC_NONE;
        for (int i = 0; i < N; i++) {
            int lim = reserve_read(block);
            if (lim) {
                inc = lim;
                break;
            }
            readres_t rr = timed_pread(usb->fd, buf, (size_t)block, offs[i]);
            reads++;
            bytes += rr.bytes;
            io_ns += rr.ns;
            errors += rr.error ? 1 : 0;
            shorts += rr.short_read ? 1 : 0;
            if (!rr.error && latn < N) {
                double us = (double)rr.ns / 1000.0;
                lat[latn++] = us;
                dstat_add(&latst, us);
            }
            if (rr.error) {
                inc = INC_ERROR;
                break;
            }
            if (rr.short_read) {
                inc = INC_SHORT_READ;
                break;
            }
        }
        qsort(lat, latn, sizeof(lat[0]), cmp_double);
        p50[pass] = nearest_percentile(lat, latn, 50);
        rate[pass] = mib_s(bytes, io_ns);
        benchf("summary test=warm_repeat mode=rand block=%llu pass=%d samples=%u bytes=%llu reads=%u lat_mean_us=%.3f lat_sd_us=%.3f lat_p50_us=%.3f lat_p95_us=%.3f lat_min_us=%.3f lat_max_us=%.3f mib_s=%.3f errors=%u short_reads=%u cache=%s unique=%d incomplete=%s",
               (unsigned long long)block, pass, latn, (unsigned long long)bytes, reads,
               latst.mean, dstat_sd(&latst), p50[pass], nearest_percentile(lat, latn, 95),
               latst.n ? latst.min : 0, latst.n ? latst.max : 0, rate[pass], errors, shorts,
               cache, pass == 0 && bm->enabled, inc_name(inc));
    }
    benchf("compare test=warm_repeat mode=rand block=%llu cold_p50_us=%.3f warm_p50_us=%.3f speedup_p50=%.3f cold_mib_s=%.3f warm_mib_s=%.3f speedup_mib_s=%.3f cache=warm_repeat",
           (unsigned long long)block, p50[0], p50[1], p50[1] > 0 ? p50[0] / p50[1] : 0,
           rate[0], rate[1], rate[0] > 0 ? rate[1] / rate[0] : 0);
}

static void test_warm_repeat(input_t *usb, bitmap_t *bm, rng_t *rng, void *buf) {
    run_warm_repeat_rand(usb, bm, rng, buf, 4 * KIB);
    if (!limit_hit())
        run_warm_repeat_rand(usb, bm, rng, buf, 64 * KIB);
    if (limit_hit())
        return;

    const uint64_t win = 16 * MIB;
    uint64_t off = 0;
    if (!find_free_span(bm, (uint64_t)usb->st.st_size, win, MIB, 75, &off) || !bitmap_reserve(bm, off, win)) {
        benchf("skip test=warm_repeat mode=seq reason=layout_unavailable");
        return;
    }
    double rates[2] = {0, 0};
    for (int pass = 0; pass < 2; pass++) {
        if (limit_hit())
            break;
        const char *cache = pass == 0 ? (bm->enabled ? "unique16k_first" : "coldish_first") : "warm_repeat";
        seqres_t r;
        measure_seq(usb, buf, off, win, MIB, &r);
        rates[pass] = mib_s(r.bytes, r.ns);
        print_seq_rep("warm_repeat", pass == 0 ? "first" : "repeat", pass, "usb_patch", off, &r, cache,
                      pass == 0 && bm->enabled, 0);
    }
    benchf("compare test=warm_repeat mode=seq block=%llu offset=%llu cold_mib_s=%.3f warm_mib_s=%.3f speedup_mib_s=%.3f cache=warm_repeat",
           (unsigned long long)MIB, (unsigned long long)off, rates[0], rates[1],
           rates[0] > 0 ? rates[1] / rates[0] : 0);
}

/* --- O_DIRECT acceptance probe; never claimed as proof of cache bypass --- */

static void test_odirect(input_t *usb, bitmap_t *bm, rng_t *rng, void *buf) {
    errno = 0;
    int fd = open(USB_PATCH, O_RDONLY | O_DIRECT);
    int open_errno = errno;
    if (fd < 0) {
        benchf("probe test=odirect odirect_open=0 odirect_accepted=0 odirect_read=0 errno=%d errors=0 cache=odirect_probe caveat=not_proof_of_cache_bypass",
               open_errno);
        return;
    }

    unsigned collisions = 0, errors = 0, shorts = 0;
    uint64_t seq_off = 0;
    uint64_t seq_bytes = 0, seq_ns = 0;
    int seq_errno = 0;
    int read_ok = 0;
    if (!random_offset(usb, bm, rng, MIB, bm->enabled, &seq_off, &collisions)) {
        int lim = reserve_read(MIB);
        if (!lim) {
            readres_t rr = timed_pread(fd, buf, MIB, seq_off);
            seq_bytes = rr.bytes;
            seq_ns = rr.ns;
            seq_errno = rr.errnum;
            errors += rr.error ? 1 : 0;
            shorts += rr.short_read ? 1 : 0;
            read_ok = !rr.error && rr.bytes == MIB;
        } else {
            seq_errno = lim == INC_DEADLINE ? ETIMEDOUT : ENOSPC;
        }
    }

    double lat[MAX_LAT_SAMPLES];
    unsigned latn = 0;
    dstat_t latst;
    memset(&latst, 0, sizeof(latst));
    unsigned rand_errors = 0, rand_shorts = 0;
    uint64_t rand_bytes = 0;
    if (read_ok) {
        for (int i = 0; i < 32; i++) {
            uint64_t off;
            if (random_offset(usb, bm, rng, 4 * KIB, bm->enabled, &off, &collisions))
                break;
            int lim = reserve_read(4 * KIB);
            if (lim)
                break;
            readres_t rr = timed_pread(fd, buf, 4 * KIB, off);
            rand_bytes += rr.bytes;
            rand_errors += rr.error ? 1 : 0;
            rand_shorts += rr.short_read ? 1 : 0;
            if (!rr.error && latn < MAX_LAT_SAMPLES) {
                double us = (double)rr.ns / 1000.0;
                lat[latn++] = us;
                dstat_add(&latst, us);
            }
            if (rr.error) {
                seq_errno = rr.errnum;
                break;
            }
        }
    }
    close(fd);
    qsort(lat, latn, sizeof(lat[0]), cmp_double);
    benchf("probe test=odirect odirect_open=1 odirect_accepted=1 odirect_read=%d errno=%d seq_offset=%llu seq_bytes=%llu seq_ns=%llu seq_mib_s=%.3f rand_samples=%u rand_bytes=%llu lat_mean_us=%.3f lat_sd_us=%.3f lat_p50_us=%.3f lat_p95_us=%.3f lat_min_us=%.3f lat_max_us=%.3f errors=%u short_reads=%u rng_collisions=%u cache=odirect_probe caveat=not_proof_of_cache_bypass",
           read_ok, seq_errno, (unsigned long long)seq_off, (unsigned long long)seq_bytes,
           (unsigned long long)seq_ns, mib_s(seq_bytes, seq_ns), latn, (unsigned long long)rand_bytes,
           latst.mean, dstat_sd(&latst), nearest_percentile(lat, latn, 50),
           nearest_percentile(lat, latn, 95), latst.n ? latst.min : 0, latst.n ? latst.max : 0,
           errors + rand_errors, shorts + rand_shorts, collisions);
}

/* --- payload entry: close everything and return; never exit() --- */

static void *aligned_buffer(size_t size, void **raw) {
    *raw = mem_alloc(size + GRANULE);
    if (!*raw)
        return NULL;
    uintptr_t p = (uintptr_t)*raw;
    void *buf = (void *)((p + GRANULE - 1) & ~(uintptr_t)(GRANULE - 1));
    memset(buf, 0, size);
    return buf;
}

static void fw_decode_local(uint32_t raw, unsigned *major, unsigned *minor) {
    *major = ((raw >> 28) & 0xf) * 10 + ((raw >> 24) & 0xf);
    *minor = ((raw >> 20) & 0xf) * 10 + ((raw >> 16) & 0xf);
}

int main(void) {
    g_start_mono = mono_ns();
    time_t realtime = time(NULL);
    pid_t pid = getpid();
    snprintf(g_run, sizeof(g_run), "%lld-%d", (long long)realtime, (int)pid);
    int net = net_connect_result() == 0;

    uint32_t fw_raw = kernel_get_fw_version();
    unsigned fw_major = 0, fw_minor = 0;
    fw_decode_local(fw_raw, &fw_major, &fw_minor);
    struct timespec res;
    long clock_res = -1;
    if (!clock_getres(CLOCK_MONOTONIC, &res))
        clock_res = res.tv_sec * 1000000000L + res.tv_nsec;
    long page_size = sysconf(_SC_PAGESIZE);

    benchf("payload=benchdisk bench_ver=%d build=%s sdk=%s method=pread fw=%u.%02u fw_raw=0x%08x pid=%d page_size=%ld clock_res_ns=%ld seed=0xc0de000000023827 start_realtime=%lld net=%d net_peer=%s:%d stdout=1 deadline_s=240 byte_cap=%llu",
           BENCH_VER, BENCH_BUILD, BENCH_SDK, fw_major, fw_minor, fw_raw, (int)pid,
           page_size, clock_res, (long long)realtime, net, RESULT_HOST, RESULT_PORT,
           (unsigned long long)BYTE_CAP);
    benchf("caveat cache=normal_cached_io uncached=0 cache_purge=0 unique16k_only_avoids_reusing_granules=1 odirect_probe_not_proof=1");

    void *rawbuf = NULL;
    void *buf = aligned_buffer(8 * MIB, &rawbuf);
    if (!buf) {
        benchf("skip test=all reason=no_memory");
    } else {
        rng_t rng = {BENCH_SEED};
        input_t usb;
        int usb_ok = open_input(USB_PATCH, "usb_patch", &usb, 0) == 0;
        bitmap_t usb_bm;
        memset(&usb_bm, 0, sizeof(usb_bm));
        usb_layout_t layout;
        memset(&layout, 0, sizeof(layout));
        if (usb_ok) {
            bitmap_init(&usb_bm, (uint64_t)usb.st.st_size);
            plan_usb_layout(&usb, &usb_bm, &layout);
            test_usb_seq(&usb, &usb_bm, &layout, buf);
            test_usb_random(&usb, &usb_bm, &rng, buf);
            test_usb_conc(&usb, &usb_bm, &layout);
            test_patch_cmp(&usb, &usb_bm, &layout, buf);
            if (!limit_hit())
                test_multifile_conc(&usb, &usb_bm, &rng);
            if (!limit_hit())
                test_patch_rand_cmp(&usb, &usb_bm, &rng, buf);
            if (!limit_hit())
                test_scan_meta(buf);
            if (!limit_hit())
                test_warm_repeat(&usb, &usb_bm, &rng, buf);
        } else {
            benchf("skip test=usb_seq reason=no_usb_patch");
            benchf("skip test=usb_rand reason=no_usb_patch");
            benchf("skip test=usb_conc reason=no_usb_patch");
            benchf("skip test=patch_cmp reason=no_usb_patch");
            benchf("skip test=multifile_conc reason=no_usb_patch");
            benchf("skip test=patch_rand_cmp reason=no_usb_patch");
            benchf("skip test=scan_meta reason=no_usb_patch");
            benchf("skip test=warm_repeat reason=no_usb_patch");
        }

        test_addcont_cmp(buf);

        if (!limit_hit())
            run_control_tests(usb_ok, usb_ok ? (uint64_t)usb.st.st_dev : 0, &rng, buf);
        else
            benchf("skip test=control incomplete=%s", inc_name(limit_hit()));

        if (usb_ok && !limit_hit())
            test_odirect(&usb, &usb_bm, &rng, buf);
        else
            benchf("skip test=odirect reason=%s", usb_ok ? "limit_reached" : "no_usb_patch");

        if (usb_ok) {
            bitmap_free(&usb_bm);
            close_input(&usb);
        }
    }

    pthread_mutex_lock(&g_limit_lock);
    uint64_t bytes = g_bytes_read;
    uint64_t reserved = g_reserved_bytes;
    uint64_t reads = g_total_reads;
    uint64_t errors = g_errors;
    uint64_t shorts = g_short_reads;
    unsigned abnormal = g_abnormal;
    pthread_mutex_unlock(&g_limit_lock);
    int inc = limit_hit();
    const char *status = !errors && !abnormal && !inc ? "ok" : "partial";
    benchf("done status=%s elapsed_ns=%llu total_bytes=%llu reserved_bytes=%llu total_reads=%llu errors=%llu short_reads=%llu incomplete=%s end_realtime=%lld net=%d",
           status, (unsigned long long)elapsed_since(g_start_mono), (unsigned long long)bytes,
           (unsigned long long)reserved, (unsigned long long)reads, (unsigned long long)errors,
           (unsigned long long)shorts, inc_name(inc), (long long)time(NULL), g_net_on);

    if (rawbuf)
        mem_free(rawbuf);
    if (g_out_fd >= 0)
        close(g_out_fd);
    return 0;
}
