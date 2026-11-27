/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * PS4Mount daemon: scans USB drives, registers compatible local PS4 packages, follows klog and serves the web panel. */
/* ps4mount: mount compatible PS4 packages from external storage.
 *
 * Every scan:
 *   1. A PS4 app pkg found at the root of /mnt/usbN or /mnt/extN is moved to
 *      <root>/ps4/<TITLE_ID>/app.pkg (same disk, rename only).
 *   2. For each <root>/ps4/<TITLE_ID>/app.pkg the files a real install leaves
 *      behind are generated from the pkg into an internal folder next to a symlink
 *      to the pkg, that folder is mounted over /user/app/<TITLE_ID> (nullfs) and
 *      the title is added to app.db/appinfo.db.
 *   3. Mounts whose storage disappeared are released.
 * Titles installed the normal way are never touched.
 *
 * With organize off, step 1 is skipped: the pkgs are used where they are,
 * in any folder of the drive (the ps4/<TITLE_ID>/ folders of older versions included),
 * and the catalog (catalog.c) groups them per game from what they carry inside.
 * Nothing is renamed or moved. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <ps4/kernel.h>

#include "autorun.h"
#include "badpatch.h"
#include "bgftscan.h"
#include "build.h"
#include "catalog.h"
#include "catsnap.h"
#include "check.h"
#include "coexist.h"
#include "upload.h"
#include "crypto.h"
#include "db.h"
#include "delete.h"
#include "dlcvec.h"
#include "links.h"
#include "fw.h"
#include "https.h"
#include "install.h"
#include "klogdup.h"
#include "lang.h"
#include "logts.h"
#include "mem.h"
#include "mountplan.h"
#include "origin.h"
#include "pkg.h"
#include "retention.h"
#include "scanfilter.h"
#include "scanreq.h"
#include "restore.h"
#include "shortpath.h"
#include "emu.h"
#include "stage.h"
#include "telemetry.h"
#include "titlecache.h"
#include "web.h"

#define VERSION LODESTAR_VERSION
#define LOCK_PORT 9032
/* A full walk of the drive is slower on drives with many files: the catalog is brought
 * up to date every so often, while the games already found are checked every scan. */
#define CATALOG_SECONDS 60
#define STATE_DIR "/data/lodestar"
/* Internal folders mounted over /user/app/<TID> and /user/patch/<TID>: the files an
 * install leaves there plus app.pkg/patch.pkg as symlinks to the pkgs on the drive.
 * /user is bfs, which has no symlinks; /system_data is ufs. A delete from the
 * console only ever removes these links, never the pkgs. */
#define HOLD_DIR "/system_data/lodestar"
#define MAX_TITLES 768
#define MAX_DLC 64
#define BATCH_QUIET 10 /* seconds without writes to the system databases between groups */
#define MAX_DLC_TIDS 8
/* Add-ons up to this size are installed with BGFT, not mounted: only the system
 * may write their record in addcont.db, which the game and its information page
 * read. Most are 1-2 MB licenses (some with a little data); copying them costs nothing. */
#define DLC_INSTALL_MAX (256ull << 20)

/* Where add-ons are served from. OFF: none. EXTERNAL (the default): every add-on is
 * mounted straight from the drive (place_dlc + nullfs over /user/addcont/<TID>), with
 * 0 bytes copied to internal storage. INTERNAL: add-ons are installed with BGFT like
 * before, up to DLC_INSTALL_MAX or any size with dlc_big. */
enum { DLC_OFF = 0, DLC_EXTERNAL = 1, DLC_INTERNAL = 2 };

static const char *dlc_mode_name(int m) {
    return m == DLC_INTERNAL ? "internal" : m == DLC_EXTERNAL ? "external" : "off";
}

static int dlc_mode_parse(const char *s) {
    if (!strcmp(s, "internal") || !strcmp(s, "compat")) /* compat: name used by
        * the interim external-only builds; internal is the same install path */
        return DLC_INTERNAL;
    if (!strcmp(s, "external"))
        return DLC_EXTERNAL;
    return !strcmp(s, "off") ? DLC_OFF : -1;
}

static int dlc_skip(uint64_t size, int mode, int big) {
    return mode == DLC_INTERNAL && !big && size > DLC_INSTALL_MAX;
}

#define MAX_SCAN_PATHS 64
#define CONFIG_PATH STATE_DIR "/config.txt"
#define RESTORED_LIST STATE_DIR "/restored.txt" /* pkgs moved back: not picked up again */

/* Settings, kept in CONFIG_PATH and edited in the panel. */
typedef struct {
    int batch;      /* new games are added in groups */
    int batch_size; /* games per group */
    int dlc;        /* add-ons are served: kept in sync with dlc_mode (dlc_mode != DLC_OFF) */
    int dlc_mode;   /* DLC_OFF / DLC_EXTERNAL / DLC_INTERNAL: where add-ons are served from */
    char paths[MAX_SCAN_PATHS][128]; /* folders searched for pkgs, relative to each drive; "" is the root */
    int npaths;
    int restore_batch; /* games removed from the library per group when moving pkgs back */
    int internal;      /* pkgs in the console's /data/Lodestar are used too */
    int organize;      /* pkgs are moved into <root>/ps4/<TID>/ (default); off: used where they are */
    int lang;          /* LANG_* of notifications and the panel, -1: the console's */
    int dlc_big;       /* add-ons over DLC_INSTALL_MAX are installed too (they take space on the console) */
    int klog;          /* the console's kernel log is read (game start/stop); off leaves it to
                        * another payload (AnyPad needs it to identify its virtual pad) */
    int autorun;       /* the .bin is listed in GoldHEN's payloads.ini: it starts with the console */
} config_t;

#define MAX_RESTORE_BATCH 10

/* The folder searched by default: one of Lodestar's own, so nothing else on a drive is
 * moved until the user puts it there (the drive root, "", is still a choice). */
#define DEFAULT_SCAN_PATH "Lodestar"

static config_t g_cfg = {1, 5, 1, DLC_EXTERNAL, {DEFAULT_SCAN_PATH}, 1, 5, 1, 1, -1, 1, 1, 0};

/* WEBAPI-5: the whole config is main-loop state, but links callbacks need these few
 * fields from their own threads. Keep the mutex confined to copying a scalar/path; scans and
 * network work never run under it. */
static pthread_mutex_t g_cfg_mu = PTHREAD_MUTEX_INITIALIZER;

static void config_publish(const config_t *c) {
    pthread_mutex_lock(&g_cfg_mu);
    g_cfg = *c;
    pthread_mutex_unlock(&g_cfg_mu);
}

static int cfg_organize(void) {
    pthread_mutex_lock(&g_cfg_mu);
    int v = g_cfg.organize;
    pthread_mutex_unlock(&g_cfg_mu);
    return v;
}

static int cfg_internal(void) {
    pthread_mutex_lock(&g_cfg_mu);
    int v = g_cfg.internal;
    pthread_mutex_unlock(&g_cfg_mu);
    return v;
}

static void cfg_scan_path(int i, char *out, size_t n) {
    pthread_mutex_lock(&g_cfg_mu);
    snprintf(out, n, "%s", i >= 0 && i < MAX_SCAN_PATHS ? g_cfg.paths[i] : "");
    pthread_mutex_unlock(&g_cfg_mu);
}

/* GET /api/config can run in a web worker: one consistent copy, with no scan or network
 * work under the mutex. Mutations still publish through config_publish in the owner. */
static void cfg_snapshot(config_t *out) {
    pthread_mutex_lock(&g_cfg_mu);
    *out = g_cfg;
    pthread_mutex_unlock(&g_cfg_mu);
}
static int g_setup; /* a first start: no game is added until the settings are saved (first_start) */
/* A first start with nobody at the panel for this long goes on with the default settings:
 * most never open the panel, and they would never get a game. */
#define SETUP_WAIT 120
static time_t g_setup_start;
static time_t g_setup_seen; /* the panel's last look at the list during a first start */

static int setup_active(void) { return __atomic_load_n(&g_setup, __ATOMIC_ACQUIRE); }
static void setup_set(int on) { __atomic_store_n(&g_setup, on, __ATOMIC_RELEASE); }
static time_t setup_seen_at(void) { return __atomic_load_n(&g_setup_seen, __ATOMIC_ACQUIRE); }
static void setup_seen_touch(void) { __atomic_store_n(&g_setup_seen, time(NULL), __ATOMIC_RELEASE); }

int sceSystemServiceParamGetInt(int param_id, int *value);
int sceSystemServicePowerTick(void);

/* The language in use: the one chosen in the settings, else the console's. */
#define REG_LANGUAGE 0x02020000 /* the system language in the registry */

int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t, void *, int *);

/* The console's language. Asked to the system service it is always English (1) inside
 * GoldHEN's process, where AutoRun starts PS4Mount: the registry, read from the kernel,
 * gives the real one in any process. The service is the fallback. */
static int console_language(void) {
    int v = -1, reg = -1, rc = -1;
    int res = 0;
    uint32_t h = 0;
    int loaded = 0;
    if (kernel_dynlib_handle(getpid(), "libSceRegMgr.sprx", &h)) { /* not in this process yet */
        int lh = sceKernelLoadStartModule("/system/common/lib/libSceRegMgr.sprx", 0, NULL, 0, NULL, &res);
        h = lh > 0 ? (uint32_t)lh : 0;
        loaded = lh > 0;
    }
    int (*get)(long, int *) = h ? (void *)kernel_dynlib_dlsym(getpid(), h, "sceRegMgrGetInt") : NULL;
    if (get && !(rc = get(REG_LANGUAGE, &reg)) && reg >= 0)
        v = reg;
    if (v < 0 && sceSystemServiceParamGetInt(1, &v)) /* 1: the system language */
        v = -1;
    printf("[main] console language %d (registry %d: handle %u%s, call 0x%x)\n", v, reg, h,
           loaded ? " loaded" : "", (unsigned)rc);
    return v;
}

static void apply_lang(void) {
    static int console = -2;
    if (console == -2)
        console = console_language();
    lang_set(g_cfg.lang >= 0 ? g_cfg.lang : lang_from_console(console));
}

/* sceKernelSendNotificationRequest's request, the OrbisNotificationRequest layout the
 * OpenOrbis, GoldHEN and Apollo notifications use: 3120 bytes, the text at offset 45. */
typedef struct {
    int32_t type;
    int32_t req_id;
    int32_t priority;
    int32_t msg_id;
    int32_t target_id;
    int32_t user_id;
    int32_t unk1;
    int32_t unk2;
    int32_t app_id;
    int32_t error_num;
    int32_t unk3;
    uint8_t use_icon_uri;
    char message[1024];
    char icon_uri[1024];
    char unk[1024];
} notify_request_t;
_Static_assert(sizeof(notify_request_t) == 3120, "notify_request_t must be 3120 bytes");
_Static_assert(offsetof(notify_request_t, message) == 45, "notify_request_t message offset must be 45");

typedef enum {
    NOTIFY_INFO = 0,
    NOTIFY_READY = 1,
    NOTIFY_WARN = 2,
    NOTIFY_DOWNLOAD = 3,
    NOTIFY_DRIVE = 4
} notify_kind_t;

/* The icon each kind shows: cxml URIs of the system's own notification icons. */
static const char *notify_icon(notify_kind_t kind) {
    switch (kind) {
    case NOTIFY_DOWNLOAD:
        return "cxml://psnotification/tex_default_icon_download";
    case NOTIFY_DRIVE:
        return "cxml://psnotification/tex_icon_cloud_client";
    case NOTIFY_READY:
    case NOTIFY_WARN:
        return "cxml://psnotification/tex_default_icon_notification";
    default:
        return "cxml://psnotification/tex_icon_system";
    }
}

static const char *notify_kind_name(notify_kind_t kind) {
    switch (kind) {
    case NOTIFY_READY:
        return "ready";
    case NOTIFY_WARN:
        return "warn";
    case NOTIFY_DOWNLOAD:
        return "download";
    case NOTIFY_DRIVE:
        return "drive";
    default:
        return "info";
    }
}

int sceUserServiceInitialize(void *);
int sceUserServiceGetForegroundUser(int32_t *);
int sceAppInstUtilInitialize(void);
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

/* The last notifications, newest first (g_notices[0] is the latest one), shown in the
 * panel. notify() also runs on worker threads: the buffer is locked. */
#define NOTICE_MAX 12
typedef struct {
    int64_t ts;
    notify_kind_t kind;
    char text[256];
} notice_entry_t;
static notice_entry_t g_notices[NOTICE_MAX];
static int g_nnotices;
static pthread_mutex_t g_notice_mu = PTHREAD_MUTEX_INITIALIZER;

static void notify_v(notify_kind_t kind, int record, int popup, const char *fmt, va_list ap) {
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    req.target_id = -1;
    req.use_icon_uri = 1;
    snprintf(req.icon_uri, sizeof(req.icon_uri), "%s", notify_icon(kind));
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    if (record)
        printf("[notify] %s\n", req.message);
    if (record) {
        pthread_mutex_lock(&g_notice_mu);
        memmove(&g_notices[1], &g_notices[0], sizeof(g_notices[0]) * (NOTICE_MAX - 1));
        g_notices[0].ts = (int64_t)time(NULL);
        g_notices[0].kind = kind;
        snprintf(g_notices[0].text, sizeof(g_notices[0].text), "%s", req.message);
        if (g_nnotices < NOTICE_MAX)
            g_nnotices++;
        pthread_mutex_unlock(&g_notice_mu);
    }
    if (popup)
        sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static void notify_kind(notify_kind_t kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void notify_kind(notify_kind_t kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    notify_v(kind, 1, 1, fmt, ap);
    va_end(ap);
}

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    notify_v(NOTIFY_INFO, 1, 1, fmt, ap);
    va_end(ap);
}

/* Detail worth keeping in the panel/log, but not worth interrupting a game for. */
static void notify_detail_kind(notify_kind_t kind, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void notify_detail_kind(notify_kind_t kind, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    notify_v(kind, 1, 0, fmt, ap);
    va_end(ap);
}

static void notify_detail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify_detail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    notify_v(NOTIFY_INFO, 1, 0, fmt, ap);
    va_end(ap);
}

/* What the daemon is doing right now, for the panel: the phase of the scan or restore
 * under way ("" when idle), and the add-ons the system is still installing. Written on
 * the main loop only; the panel reads it under the same mutex. */
typedef struct {
    char phase[24];   /* "", "scanning", "registering", "waiting_db", "restoring" */
    char drive[64];
    char title[128];
    char title_id[16];
    int done;
    int total;
    int group;
    int groups;
} activity_t;

static activity_t g_activity;
static pthread_mutex_t g_act_mu = PTHREAD_MUTEX_INITIALIZER;

static void activity_set(const char *phase, const char *drive, const char *title, const char *tid, int done,
                         int total, int group, int groups) {
    pthread_mutex_lock(&g_act_mu);
    snprintf(g_activity.phase, sizeof(g_activity.phase), "%s", phase ? phase : "");
    snprintf(g_activity.drive, sizeof(g_activity.drive), "%s", drive ? drive : "");
    snprintf(g_activity.title, sizeof(g_activity.title), "%s", title ? title : "");
    snprintf(g_activity.title_id, sizeof(g_activity.title_id), "%s", tid ? tid : "");
    g_activity.done = done;
    g_activity.total = total;
    g_activity.group = group;
    g_activity.groups = groups;
    pthread_mutex_unlock(&g_act_mu);
}

static void activity_clear(void) {
    pthread_mutex_lock(&g_act_mu);
    memset(&g_activity, 0, sizeof(g_activity));
    pthread_mutex_unlock(&g_act_mu);
}

/* --- small fs helpers --- */

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static void mkdirs(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    mkdir(tmp, mode);
}

static int write_file(const char *path, const void *data, size_t len, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    const uint8_t *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
}

static int mounted_from(const char *mountpoint, char *from, size_t n) {
    struct statfs sf;
    if (statfs(mountpoint, &sf) || strcmp(sf.f_mntonname, mountpoint) || strcmp(sf.f_fstypename, "nullfs"))
        return 0;
    if (from)
        snprintf(from, n, "%s", sf.f_mntfromname);
    return 1;
}

/* Main-loop scan epoch telemetry (P4N-22). Never touched by web worker threads. */
static scan_metrics_t g_scan_m;

static uint64_t mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return 0;
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void scan_note_usb_stat(const char *path) {
    if (g_scan_m.active && is_usb_storage_path(path))
        g_scan_m.usb_stat++;
}

static void scan_note_usb_pkg(const char *path, const pkg_t *pkg) {
    if (!g_scan_m.active || !is_usb_storage_path(path))
        return;
    g_scan_m.usb_open++;
    g_scan_m.usb_read += pkg->read_ops;
    g_scan_m.usb_bytes += pkg->bytes_read;
}

static void scan_note_mount_read(void) {
    if (g_scan_m.active)
        g_scan_m.mount_reads++;
}

/* Mounts on path from the mount table. Unlike statfs(path) this also sees a layer
 * whose source drive was disconnected (statfs on it fails). */
static int layers_on(const char *path) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT), count = 0;
    if (n <= 0)
        return 0;
    struct statfs *list = mem_alloc(sizeof(*list) * (size_t)(n + 16));
    if (!list)
        return 0;
    scan_note_mount_read();
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].f_mntonname, path))
            count++;
    mem_free(list);
    return count;
}

/* For detach postconditions an unreadable mount table is not "no layers". Returns nonzero
 * both when a layer is present and when absence cannot be proven. */
static int layers_on_failclosed(const char *path) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT), count = 0;
    if (n <= 0)
        return 1;
    struct statfs *list = mem_alloc(sizeof(*list) * (size_t)(n + 16));
    if (!list)
        return 1;
    scan_note_mount_read();
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    if (n < 0) {
        mem_free(list);
        return 1;
    }
    for (int i = 0; i < n; i++)
        if (!strcmp(list[i].f_mntonname, path))
            count++;
    mem_free(list);
    return count != 0;
}

/* A layer is on path but cannot be read: its drive was disconnected (and maybe
 * connected again, which gives it a new mount). */
static int dead_layer(const char *path) {
    struct statfs sf;
    return statfs(path, &sf) && layers_on(path) > 0;
}

static int is_mountpoint(const char *path) {
    struct statfs sf;
    return statfs(path, &sf) == 0 && !strcmp(sf.f_mntonname, path);
}

/* --- where games are looked for --- */

/* The USB drives, the extended storage, when turned on the console's own storage (pkgs
 * sent to /data/Lodestar are registered from there, library /data/ps4, with no second
 * copy and no install to wait for; they still take internal space). */
#define NROOTS 11
#define INTERNAL_ROOT "/data"

static void root_path(int i, char *out, size_t n) {
    if (i < 8)
        snprintf(out, n, "/mnt/usb%d", i);
    else if (i < 10)
        snprintf(out, n, "/mnt/ext%d", i - 8);
    else
        snprintf(out, n, INTERNAL_ROOT);
}

static int is_internal(const char *root) { return !strcmp(root, INTERNAL_ROOT); }

/* A pkg path as the system writes it in its records: /data is an alias the payload can
 * use, but BGFT refuses it ("invalid content_url") and starts nothing; the installs of
 * the system say /user/data. */
static const char *sys_path(const char *path, char *out, size_t n) {
    if (!strncmp(path, INTERNAL_ROOT "/", sizeof(INTERNAL_ROOT)))
        snprintf(out, n, "/user%s", path);
    else
        snprintf(out, n, "%s", path);
    return out;
}

/* The pkg in the hold folder is a link to this: the pkg's own path when it is short, and
 * otherwise STATE_DIR/l/<TID>-<kind>.pkg, a short link to it (shortpath.h), made or fixed
 * here. The alias /data -> /user/data is not used: a link through it does not mount (the
 * system cannot build the pkg's block map, 0x8002000c). */
static const char *pkg_link(const char *tid, const char *kind, const char *path, char *out, size_t n) {
    char link[PATH_MAX];
    struct stat a, b;
    if (!short_pkg_link(STATE_DIR "/l", tid, kind, path, link, sizeof(link))) {
        /* the path fits: a link left from when the pkg was somewhere with a longer one goes */
        snprintf(link, sizeof(link), STATE_DIR "/l/%s-%s.pkg", tid, kind);
        unlink(link);
        snprintf(out, n, "%s", path);
        return out;
    }
    /* the pkg moved, or the drive it is on was mounted again: the link is made anew */
    if (stat(link, &a) || stat(path, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino) {
        mkdirs(STATE_DIR "/l", 0777);
        unlink(link);
        if (symlink(path, link))
            printf("[reg] %s: short link %s: %s\n", tid, link, strerror(errno));
    }
    snprintf(out, n, "%s", link);
    return out;
}

/* The path handed to the system for a pkg in its records (app.json, BGFT): /user/data,
 * which is what the system writes and what BGFT accepts. */
static const char *pkg_url(const char *tid, const char *kind, const char *path, char *out, size_t n) {
    char p[PATH_MAX];
    return sys_path(pkg_link(tid, kind, path, p, sizeof(p)), out, n);
}

/* Pkgs used where they are: with organizing off. */
static int in_place_cfg(const config_t *cfg) { return !cfg->organize; }

static int in_place(void) { return !cfg_organize(); }

static int root_present_cfg(const char *root, const config_t *cfg) {
    return is_internal(root) ? cfg->internal : is_mountpoint(root);
}

static int root_present(const char *root) {
    return is_internal(root) ? cfg_internal() : is_mountpoint(root);
}

/* "usb0", "ext0", "internal storage" */
static const char *root_label(const char *root) {
    return is_internal(root) ? TR("internal storage", "memoria interna") : root + 5;
}

/* The short name of a drive in the state folder and in the panel. */
static const char *root_id(const char *root, char *out, size_t n) {
    if (is_internal(root))
        snprintf(out, n, "%s", "internal");
    else
        snprintf(out, n, "%s", root + 5);
    return out;
}

/* The pkgs found on each drive when they are used where they are (organize off). The
 * catalog is changed under a lock; the scan holds it while it walks. */
static catalog_t g_cat[NROOTS];
static int g_cat_loaded[NROOTS]; /* the saved catalog was read since the drive appeared */
static time_t g_cat_walked[NROOTS]; /* when the drive was last searched for pkgs */
static pthread_mutex_t g_cat_lock = PTHREAD_MUTEX_INITIALIZER;

/* Immutable completed-epoch snapshots for panel workers. The catalog itself stays owner
 * state behind g_cat_lock because a scan can hold that lock while pumping events; readers
 * instead acquire a reference here and never enter that lock. */
static pthread_mutex_t g_cat_snap_mu = PTHREAD_MUTEX_INITIALIZER;
static catsnap_t *g_cat_snap[NROOTS];

static int root_index(const char *root) {
    char r[PATH_MAX];
    for (int i = 0; i < NROOTS; i++) {
        root_path(i, r, sizeof(r));
        if (!strcmp(r, root))
            return i;
    }
    return -1;
}

static void catsnap_put(char *arena, uint32_t *at, const char *text) {
    size_t n = strlen(text) + 1;
    memcpy(arena + *at, text, n);
    *at += (uint32_t)n;
}

/* Builds the compact immutable form of one grouped catalog. Add-on duplicates stay in the
 * snapshot exactly as catalog grouping chose them; src_open keeps its historical dedupe. */
static catsnap_t *catalog_snapshot_build(const catalog_t *c) {
    uint32_t ngames = c->ngames < MAX_TITLES ? (uint32_t)c->ngames : MAX_TITLES;
    uint32_t ndlcs = 0;
    uint64_t slen = 0;
    for (uint32_t i = 0; i < ngames; i++) {
        const cat_game_t *g = &c->games[i];
        slen += strlen(c->pkgs[g->app].path) + 1;
        const cat_pkg_t *patch = catalog_patch(g_cat, c, g);
        if (patch)
            slen += strlen(patch->path) + 1;
        const cat_pkg_t *list[CAT_MAX_DLC];
        int nlist = catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC);
        ndlcs += (uint32_t)nlist;
        for (int k = 0; k < nlist; k++)
            slen += strlen(list[k]->path) + 1;
    }
    if (slen > UINT32_MAX)
        return NULL;
    catsnap_t *s = catsnap_alloc(ngames, ndlcs, (uint32_t)slen);
    if (!s)
        return NULL;
    catsnap_game_t *games = catsnap_games(s);
    catsnap_dlc_t *dlcs = catsnap_dlcs(s);
    char *arena = catsnap_strings(s);
    uint32_t at = 0, di = 0;
    for (uint32_t i = 0; i < ngames; i++) {
        const cat_game_t *g = &c->games[i];
        catsnap_game_t *out = &games[i];
        memset(out, 0, sizeof(*out));
        snprintf(out->title_id, sizeof(out->title_id), "%s", g->title_id);
        out->app_off = at;
        catsnap_put(arena, &at, c->pkgs[g->app].path);
        const cat_pkg_t *patch = catalog_patch(g_cat, c, g);
        out->patch_off = CATSNAP_NO_PATH;
        if (patch) {
            out->patch_off = at;
            catsnap_put(arena, &at, patch->path);
        }
        const cat_pkg_t *list[CAT_MAX_DLC];
        int nlist = catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC);
        out->first_dlc = di;
        out->ndlc = (uint32_t)nlist;
        for (int k = 0; k < nlist; k++) {
            catsnap_dlc_t *d = &dlcs[di++];
            d->path_off = at;
            catsnap_put(arena, &at, list[k]->path);
            snprintf(d->content_id, sizeof(d->content_id), "%s", list[k]->content_id);
        }
    }
    return s;
}

/* g_cat_lock held: the grouped catalogs of one completed epoch become the visible snapshot.
 * Publication is all-or-nothing across roots: a memory failure keeps the previous epoch. */
static void catalog_epoch_publish_locked(void) {
    catsnap_t *next[NROOTS] = {0};
    int bad = 0;
    for (int i = 0; i < NROOTS; i++) {
        if (!g_cat[i].ngames)
            continue;
        next[i] = catalog_snapshot_build(&g_cat[i]);
        bad |= next[i] == NULL;
    }
    if (bad) {
        for (int i = 0; i < NROOTS; i++)
            catsnap_unref(next[i]);
        printf("[scan] could not publish the catalog snapshot: not enough memory\n");
        return;
    }
    catsnap_t *old[NROOTS];
    pthread_mutex_lock(&g_cat_snap_mu);
    for (int i = 0; i < NROOTS; i++) {
        old[i] = g_cat_snap[i];
        g_cat_snap[i] = next[i];
    }
    pthread_mutex_unlock(&g_cat_snap_mu);
    for (int i = 0; i < NROOTS; i++)
        catsnap_unref(old[i]);
}

static catsnap_t *catalog_snapshot_acquire(const char *root) {
    int idx = root_index(root);
    if (idx < 0)
        return NULL;
    pthread_mutex_lock(&g_cat_snap_mu);
    catsnap_t *s = g_cat_snap[idx];
    catsnap_ref(s);
    pthread_mutex_unlock(&g_cat_snap_mu);
    return s;
}

static int catalog_presence_has(const char *root, const char *tid) {
    catsnap_t *s = catalog_snapshot_acquire(root);
    int found = catsnap_find(s, tid) != NULL;
    catsnap_unref(s);
    return found;
}

static catalog_t *drive_catalog(const char *root) {
    char r[PATH_MAX];
    for (int i = 0; i < NROOTS; i++) {
        root_path(i, r, sizeof(r));
        if (!strcmp(r, root))
            return &g_cat[i];
    }
    return NULL;
}

/* 1 when the catalog of the drive at root has games to serve. */
static int catalog_has_games(const char *root) {
    const catalog_t *c = drive_catalog(root);
    return c && c->ngames > 0;
}

/* --- appmeta entries copied verbatim from the pkg --- */

typedef struct {
    uint32_t id;
    const char *name;
} meta_entry_t;

static const meta_entry_t k_user_meta[] = {
    {0x1200, "icon0.png"}, {0x1220, "pic0.png"},  {0x1006, "pic1.png"},          {0x1240, "snd0.at9"},
    {0x1280, "icon0.dds"}, {0x12a0, "pic0.dds"},  {0x1004, "pronunciation.xml"}, {0x100b, "shareparam.json"},
    {0, NULL},
};

/* Registration writes these entries while panel workers may read them. Serialize only the
 * file write/read boundary: pkg extraction and event pumping stay outside the mutex. */
static pthread_mutex_t g_meta_file_mu = PTHREAD_MUTEX_INITIALIZER;

static int write_entry(pkg_t *pkg, uint32_t id, const char *path) {
    size_t len;
    uint8_t *data = pkg_entry(pkg, id, &len);
    if (!data)
        return 1; /* absent entry is fine */
    pthread_mutex_lock(&g_meta_file_mu);
    int rc = write_file(path, data, len, 0777);
    pthread_mutex_unlock(&g_meta_file_mu);
    mem_free(data);
    return rc;
}

static int write_buf(const char *path, const buf_t *b) { return write_file(path, b->data, b->len, 0777); }

/* --- registration --- */

/* An add-on: <root>/ps4/<TID>/addcont/<ADD-ON TID>/<LABEL>/ac.pkg. */
typedef struct {
    char path[PATH_MAX];
    pkg_t pkg;
    pkg_meta_t meta;
    int data;    /* 1: has data (mounted by the game), 0: only unlocks content */
    int install; /* 1: installed with BGFT (compat mode, tiny license-only), 0: mounted */
} dlc_src_t;

/* One library folder: <root>/ps4/<TID>/app.pkg plus an optional patch.pkg and add-ons. */
typedef struct {
    char dir[PATH_MAX], app_path[PATH_MAX], patch_path[PATH_MAX];
    pkg_t app, patch;
    int has_patch;
    pkg_meta_t app_meta, patch_meta;
    /* the update's param.sfo, as /system_data/priv/appmeta keeps it */
    unsigned char patch_sfo_sha[32];
    uint32_t patch_sfo_len;
    char name[128], ver[8], marker[PATH_MAX + 160];
    char update_issue[256]; /* why patch.pkg was ignored, empty when used or absent */
    dlc_src_t *dlc;
    int ndlc, dlc_cap;
    char dlc_tids[MAX_DLC_TIDS][10]; /* add-on title IDs: /user/addcont/<TID> folders */
    int ndlc_tids;
} title_src_t;

/* dlcvec starts at ten slots because that still fits one 16 KiB mapping with mem_alloc's
 * header. If this layout grows, choose the initial capacity again instead of silently
 * crossing the page boundary. */
_Static_assert(10 * sizeof(dlc_src_t) <= 16320, "initial add-on allocation must fit one mapped page");

/* title_info_t is title_src_t without the open pkgs (titlecache.h) */
_Static_assert(MAX_DLC_TIDS == TITLE_DLC_TIDS, "the cache keeps the add-on title IDs of a game");

/* Add-on paths as one coherent view: the scan owner uses the live catalog it just linked,
 * while a panel worker uses an immutable epoch reference acquired before opening anything. */
typedef struct {
    const catalog_t *cat;
    const cat_game_t *game;
    const catsnap_t *snap;
    const catsnap_game_t *sgame;
} addon_view_t;

/* sha and slen, when given, take the digest and length of the param.sfo (the launch
 * check compares the system's copy with it). */
static int read_sfo_ex(pkg_t *pkg, sfo_t *sfo, unsigned char *sha, uint32_t *slen) {
    size_t len;
    uint8_t *d = pkg_entry(pkg, 0x1000, &len);
    int rc = d ? sfo_parse(d, len, sfo) : -1;
    if (d && !rc && sha && slen && len <= 65536) {
        sha256(d, len, sha);
        *slen = (uint32_t)len;
    }
    mem_free(d);
    return rc;
}

static int read_sfo(pkg_t *pkg, sfo_t *sfo) { return read_sfo_ex(pkg, sfo, NULL, NULL); }

/* Registration keeps only its decision keys; callers that need other keys still use the
 * full bounded parser above. */
static int read_meta_ex(pkg_t *pkg, pkg_meta_t *meta, unsigned char *sha, uint32_t *slen) {
    sfo_t sfo;
    int rc = read_sfo_ex(pkg, &sfo, sha, slen);
    if (!rc)
        sfo_meta(&sfo, meta);
    return rc;
}

static int read_meta(pkg_t *pkg, pkg_meta_t *meta) { return read_meta_ex(pkg, meta, NULL, NULL); }

static void src_close(title_src_t *t) {
    pkg_close(&t->app);
    if (t->has_patch)
        pkg_close(&t->patch);
    for (int i = 0; i < t->ndlc; i++)
        pkg_close(&t->dlc[i].pkg);
    mem_free(t->dlc);
    t->dlc = NULL;
    t->ndlc = 0;
    t->dlc_cap = 0;
}

/* The next zeroed add-on slot of t, growing its array only when the slots already reserved
 * are full. MAX_DLC and allocation failure both return NULL; the caller distinguishes them
 * because the first is an input bound and the second is unknown work left undone. */
static dlc_src_t *dlc_next(title_src_t *t) {
    int ncap = dlcvec_next_cap(t->ndlc, t->dlc_cap, MAX_DLC);
    if (ncap < 0)
        return NULL;
    if (ncap != t->dlc_cap) {
        dlc_src_t *n = mem_realloc(t->dlc, (size_t)ncap * sizeof(*n));
        if (!n)
            return NULL;
        t->dlc = n;
        t->dlc_cap = ncap;
    }
    dlc_src_t *a = &t->dlc[t->ndlc];
    memset(a, 0, sizeof(*a));
    return a;
}

static int dlc_cmp(const void *a, const void *b) {
    return strcmp(((const dlc_src_t *)a)->pkg.content_id, ((const dlc_src_t *)b)->pkg.content_id);
}

/* Add-ons are used when their folder names match the pkg (title ID and label). */
static void dlc_open(title_src_t *t, const config_t *cfg) {
    char base[PATH_MAX];
    snprintf(base, sizeof(base), "%s/addcont", t->dir);
    DIR *d = opendir(base);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_game_id(e->d_name))
            continue;
        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", base, e->d_name);
        DIR *l = opendir(sub);
        if (!l)
            continue;
        struct dirent *f;
        while ((f = readdir(l))) {
            if (f->d_name[0] == '.' || strlen(f->d_name) != 16)
                continue;
            if (t->ndlc == MAX_DLC)
                break;
            dlc_src_t *a = dlc_next(t);
            if (!a) {
                printf("[scan] %s: no memory for its add-ons\n", t->dir);
                break;
            }
            snprintf(a->path, sizeof(a->path), "%s/%s/ac.pkg", sub, f->d_name);
            if (!exists(a->path))
                continue;
            if (pkg_open(&a->pkg, a->path) || read_meta(&a->pkg, &a->meta)) {
                if (a->pkg.fd > 0)
                    pkg_close(&a->pkg);
                continue;
            }
            /* the entry key was checked when the pkg was moved here; checking it again on
             * every scan would cost an RSA operation per add-on */
            if (!a->meta.has_category || strcmp(a->meta.category, "ac") || strcmp(a->pkg.title_id, e->d_name) ||
                strcmp(a->pkg.content_id + 20, f->d_name)) {
                pkg_close(&a->pkg);
                continue;
            }
            if (dlc_skip(a->pkg.pkg_size, cfg->dlc_mode, cfg->dlc_big)) {
                pkg_close(&a->pkg);
                continue;
            }
            char rm[96];
            snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", a->pkg.content_id);
            if (exists(rm)) { /* deleted on the console */
                pkg_close(&a->pkg);
                continue;
            }
            a->data = a->pkg.content_type == 0x1b; /* 0x1b with data, 0x1c license only */
            a->install = cfg->dlc_mode == DLC_INTERNAL && (a->pkg.pkg_size <= DLC_INSTALL_MAX || cfg->dlc_big);
            t->ndlc++;
            if (a->install)
                continue; /* not mounted: no /user/addcont folder of PS4Mount's */
            int known = 0;
            for (int i = 0; i < t->ndlc_tids; i++)
                known |= !strcmp(t->dlc_tids[i], e->d_name);
            if (!known && t->ndlc_tids < MAX_DLC_TIDS)
                snprintf(t->dlc_tids[t->ndlc_tids++], 10, "%s", e->d_name);
        }
        closedir(l);
    }
    closedir(d);
    if (t->ndlc)
        qsort(t->dlc, (size_t)t->ndlc, sizeof(*t->dlc), dlc_cmp);
}

static int first_time(const char *path);
static int find_root(const char *tid, char *root, size_t n);

/* Where the game is. Older versions moved the app pkg to <root>/ps4/<TID>/app.pkg and kept
 * its update and add-ons next to it; an in-place game has no folder of its own, and is the
 * pkg file itself (see the catalog for its update and add-ons). */
static void game_location(const char *root, const char *tid, const char *app_path, char *out, size_t n) {
    char lib[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4/%s/app.pkg", root, tid);
    if (!strcmp(app_path, lib))
        snprintf(out, n, "%s/ps4/%s", root, tid);
    else
        snprintf(out, n, "%s", app_path);
}

/* The add-ons the catalog grouped with the game, wherever they are. A second copy of an
 * add-on and the ones deleted on the console are left out. */
static void dlc_open_catalog(title_src_t *t, const catalog_t *c, const cat_game_t *g, const config_t *cfg) {
    const cat_pkg_t *list[CAT_MAX_DLC];
    int nlist = catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC);
    for (int k = 0; k < nlist && t->ndlc < MAX_DLC; k++) {
        const cat_pkg_t *p = list[k];
        int dup = 0;
        for (int i = 0; i < t->ndlc; i++)
            dup |= !strcmp(t->dlc[i].pkg.content_id, p->content_id);
        char rm[96];
        snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", p->content_id);
        if (dup || exists(rm)) /* removed: deleted on the console */
            continue;
        dlc_src_t *a = dlc_next(t);
        if (!a) {
            printf("[scan] %s: no memory for its add-ons\n", t->dir);
            break;
        }
        snprintf(a->path, sizeof(a->path), "%s", p->path);
        /* the catalog checked the category and entry key when it first read the pkg;
         * checking the key again on every scan would cost an RSA operation per add-on */
        if (pkg_open(&a->pkg, a->path) || read_meta(&a->pkg, &a->meta)) {
            if (a->pkg.fd >= 0)
                pkg_close(&a->pkg);
            continue;
        }
        if (dlc_skip(a->pkg.pkg_size, cfg->dlc_mode, cfg->dlc_big)) {
            pkg_close(&a->pkg);
            continue;
        }
        a->data = a->pkg.content_type == 0x1b; /* 0x1b with data, 0x1c license only */
        a->install = cfg->dlc_mode == DLC_INTERNAL && (a->pkg.pkg_size <= DLC_INSTALL_MAX || cfg->dlc_big);
        t->ndlc++;
        if (a->install)
            continue; /* not mounted: no /user/addcont folder of PS4Mount's */
        int known = 0;
        for (int i = 0; i < t->ndlc_tids; i++)
            known |= !strcmp(t->dlc_tids[i], a->pkg.title_id);
        if (!known && t->ndlc_tids < MAX_DLC_TIDS)
            snprintf(t->dlc_tids[t->ndlc_tids++], 10, "%s", a->pkg.title_id);
    }
    if (t->ndlc)
        qsort(t->dlc, (size_t)t->ndlc, sizeof(*t->dlc), dlc_cmp);
}

/* Same selection as dlc_open_catalog, but from an immutable catalog epoch. The snapshot
 * carries the same duplicates the catalog grouped; duplicate content IDs are still mounted once. */
static void dlc_open_snapshot(title_src_t *t, const catsnap_t *snap, const catsnap_game_t *g, const config_t *cfg) {
    const catsnap_dlc_t *dlcs = catsnap_dlcs((catsnap_t *)snap);
    for (uint32_t k = 0; k < g->ndlc && t->ndlc < MAX_DLC; k++) {
        const catsnap_dlc_t *p = &dlcs[g->first_dlc + k];
        const char *path = catsnap_path(snap, p->path_off);
        int dup = 0;
        for (int i = 0; i < t->ndlc; i++)
            dup |= !strcmp(t->dlc[i].pkg.content_id, p->content_id);
        char rm[96];
        snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", p->content_id);
        if (dup || exists(rm)) /* removed: deleted on the console */
            continue;
        dlc_src_t *a = dlc_next(t);
        if (!a) {
            printf("[scan] %s: no memory for its add-ons\n", t->dir);
            break;
        }
        snprintf(a->path, sizeof(a->path), "%s", path);
        /* the catalog checked the category and entry key when it first read the pkg;
         * checking the key again on every scan would cost an RSA operation per add-on */
        if (pkg_open(&a->pkg, a->path) || read_meta(&a->pkg, &a->meta)) {
            if (a->pkg.fd >= 0)
                pkg_close(&a->pkg);
            continue;
        }
        if (dlc_skip(a->pkg.pkg_size, cfg->dlc_mode, cfg->dlc_big)) {
            pkg_close(&a->pkg);
            continue;
        }
        a->data = a->pkg.content_type == 0x1b; /* 0x1b with data, 0x1c license only */
        a->install = cfg->dlc_mode == DLC_INTERNAL && (a->pkg.pkg_size <= DLC_INSTALL_MAX || cfg->dlc_big);
        t->ndlc++;
        if (a->install)
            continue; /* not mounted: no /user/addcont folder of PS4Mount's */
        int known = 0;
        for (int i = 0; i < t->ndlc_tids; i++)
            known |= !strcmp(t->dlc_tids[i], a->pkg.title_id);
        if (!known && t->ndlc_tids < MAX_DLC_TIDS)
            snprintf(t->dlc_tids[t->ndlc_tids++], 10, "%s", a->pkg.title_id);
    }
    if (t->ndlc)
        qsort(t->dlc, (size_t)t->ndlc, sizeof(*t->dlc), dlc_cmp);
}

/* --- the updates the system turns down --- */

/* STATE_DIR/<TID>.badpatch names the update pkg the system refused (its size and time), so
 * a game the console will not start with it is served without it, and another update pkg
 * is tried again. */
static const char *badpatch_path(const char *tid, char *out, size_t n) {
    snprintf(out, n, STATE_DIR "/%s.badpatch", tid);
    return out;
}

/* 1 when the update at patch_path is the one the system turned down. */
static int patch_refused(const char *tid, const char *patch_path) {
    char p[64], text[64];
    struct stat st;
    badpatch_path(tid, p, sizeof(p));
    int fd = open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, text, sizeof(text) - 1);
    close(fd);
    text[n > 0 ? n : 0] = 0;
    if (badpatch_is_digest(text)) {
        /* a mark of 0.6.0 and older: the pkg's digest, in its 4 KB header */
        uint8_t hdr[0x1000], digest[32];
        fd = open(patch_path, O_RDONLY);
        if (fd < 0)
            return 0;
        ssize_t got = read(fd, hdr, sizeof(hdr));
        close(fd);
        if (got != (ssize_t)sizeof(hdr) || memcmp(hdr, "\x7f" "CNT", 4))
            return 0;
        memcpy(digest, hdr + 0xfe0, sizeof(digest));
        return badpatch_digest_matches(text, digest);
    }
    if (stat(patch_path, &st))
        return 0;
    return badpatch_matches(text, (uint64_t)st.st_size, (int64_t)st.st_mtime);
}

/* Writes the mark of the update at patch_path. 0 when it is written. */
static int write_badpatch(const char *tid, const char *patch_path) {
    char p[64], text[64];
    struct stat st;
    if (!patch_path[0] || stat(patch_path, &st))
        return -1;
    badpatch_path(tid, p, sizeof(p));
    badpatch_text(text, sizeof(text), (uint64_t)st.st_size, (int64_t)st.st_mtime);
    if (write_file(p, text, strlen(text), 0666)) {
        printf("[reg] %s: cannot keep the refused update: %s\n", tid, strerror(errno));
        return -1;
    }
    return 0;
}

/* Remembers that the system refused this update (CheckCumulativePatch: the console will
 * not start the game with it). The game is served without the update from now on. */
static void refuse_patch(const char *tid, const char *patch_path, const char *name) {
    if (write_badpatch(tid, patch_path))
        return;
    printf("[reg] %s: the system refuses update %s: left out until the pkg changes\n", tid, patch_path);
    notify_kind(NOTIFY_WARN, TR("%s: the system refuses the update, so the game is added without it",
                                      "%s: el sistema rechaza la actualización, así que el juego se añade sin ella"),
           name && name[0] ? name : tid);
}

/* The pkg of the update the drive has for tid (catalog or folder). 0 when there is none. */
static int title_patch_path(const char *root, const char *tid, char *out, size_t n) {
    out[0] = 0;
    if (in_place()) {
        const catalog_t *c = drive_catalog(root);
        const cat_game_t *g = c ? catalog_game(c, tid) : NULL;
        const cat_pkg_t *p = g ? catalog_patch(g_cat, c, g) : NULL;
        if (!p)
            return 0;
        snprintf(out, n, "%s", p->path);
        return 1;
    }
    snprintf(out, n, "%s/ps4/%s/patch.pkg", root, tid);
    return exists(out);
}

/* Opens the pkgs of tid with the paths already chosen (dir, app_path and patch_path).
 * cat is NULL for the folders of older versions. */
static int src_open_paths(title_src_t *t, const char *tid, const addon_view_t *view, const config_t *cfg) {
    if (pkg_open(&t->app, t->app_path) || read_meta(&t->app, &t->app_meta)) {
        if (first_time(t->app_path))
            printf("[scan] %s: unreadable app pkg\n", t->app_path);
        if (t->app.fd >= 0)
            pkg_close(&t->app);
        return -1;
    }
    if (strcmp(t->app.title_id, tid)) {
        printf("[scan] %s: the game pkg is for %s\n", t->app_path, t->app.title_id);
        pkg_close(&t->app);
        return -1;
    }
    if (t->patch_path[0] && exists(t->patch_path)) {
        if (patch_refused(tid, t->patch_path)) {
            snprintf(t->update_issue, sizeof(t->update_issue), "%s",
                     TR("Update left out: the system refuses it. It is tried again when the pkg changes",
                        "Actualización descartada: el sistema la rechaza. Se vuelve a probar cuando cambie el pkg"));
        } else if (pkg_open(&t->patch, t->patch_path) ||
            read_meta_ex(&t->patch, &t->patch_meta, t->patch_sfo_sha, &t->patch_sfo_len)) {
            snprintf(t->update_issue, sizeof(t->update_issue), "%s",
                     TR("Update ignored: the update pkg could not be read",
                        "Actualización ignorada: no se pudo leer el pkg de la actualización"));
            if (t->patch.fd > 0)
                pkg_close(&t->patch);
        } else {
            /* An update only fits the title it was made for. (Master VERSION may differ:
             * installed games run with base 01.02 + update 01.00.) */
            const char *gver = t->app_meta.has_version ? t->app_meta.version : "?",
                         *uver = t->patch_meta.has_version ? t->patch_meta.version : "?";
            if (strcmp(t->patch.title_id, tid)) {
                snprintf(t->update_issue, sizeof(t->update_issue),
                         TR("Update ignored: it is for %s version %s, the game is %s version %s",
                            "Actualización ignorada: es para %s versión %s, el juego es %s versión %s"),
                         t->patch.title_id, uver, tid, gver);
                pkg_close(&t->patch);
            } else {
                t->has_patch = 1;
            }
        }
        if (t->update_issue[0] && first_time(t->patch_path)) {
            notify_kind(NOTIFY_WARN, "%s: %s", t->app_meta.has_title ? t->app_meta.title : tid, t->update_issue);
        }
    }
    const pkg_meta_t *eff = t->has_patch ? &t->patch_meta : &t->app_meta;
    snprintf(t->name, sizeof(t->name), "%s", eff->has_title ? eff->title : tid);
    snprintf(t->ver, sizeof(t->ver), "%s", eff->app_ver_valid ? eff->app_ver : "01.00");
    char digest[65] = "";
    if (t->has_patch)
        for (int i = 0; i < 32; i++)
            snprintf(digest + i * 2, 3, "%02x", t->patch.digest[i]);
    snprintf(t->marker, sizeof(t->marker), "%s\n%s\n", t->dir, t->has_patch ? digest : "nopatch");
    if (cfg->dlc) {
        if (view && view->cat)
            dlc_open_catalog(t, view->cat, view->game, cfg);
        else if (view && view->snap)
            dlc_open_snapshot(t, view->snap, view->sgame, cfg);
        else
            dlc_open(t, cfg);
    }
    if (t->ndlc) {
        /* one line for all add-ons, so markers of titles without add-ons stay as before */
        uint8_t h[32];
        buf_t all = {0};
        for (int i = 0; i < t->ndlc; i++) {
            buf_put(&all, t->dlc[i].pkg.digest, 32);
            buf_put(&all, t->dlc[i].install ? "i" : "m", 1);
        }
        sha256(all.data, all.len, h);
        buf_free(&all);
        size_t n = strlen(t->marker);
        snprintf(t->marker + n, sizeof(t->marker) - n, "dlc %d ", t->ndlc);
        for (int i = 0; i < 32; i++) {
            n = strlen(t->marker);
            snprintf(t->marker + n, sizeof(t->marker) - n, "%02x", h[i]);
        }
        n = strlen(t->marker);
        snprintf(t->marker + n, sizeof(t->marker) - n, "\n");
    }
    return 0;
}

/* Opens the pkgs PS4Mount serves for tid from root: its ps4/<TID> folder (organized) or,
 * with organize off, where the catalog of the drive found them. */
static int src_open(title_src_t *t, const char *root, const char *tid, const config_t *cfg) {
    memset(t, 0, sizeof(*t));
    if (in_place_cfg(cfg)) {
        const catalog_t *cat = drive_catalog(root);
        const cat_game_t *g = cat ? catalog_game(cat, tid) : NULL;
        if (!g)
            return -1;
        snprintf(t->app_path, sizeof(t->app_path), "%s", cat->pkgs[g->app].path);
        const cat_pkg_t *patch = catalog_patch(g_cat, cat, g);
        if (patch)
            snprintf(t->patch_path, sizeof(t->patch_path), "%s", patch->path);
        game_location(root, tid, t->app_path, t->dir, sizeof(t->dir));
        addon_view_t view = {cat, g, NULL, NULL};
        return src_open_paths(t, tid, &view, cfg);
    }
    snprintf(t->dir, sizeof(t->dir), "%s/ps4/%s", root, tid);
    snprintf(t->app_path, sizeof(t->app_path), "%s/app.pkg", t->dir);
    snprintf(t->patch_path, sizeof(t->patch_path), "%s/patch.pkg", t->dir);
    return src_open_paths(t, tid, NULL, cfg);
}

/* Same pkg opening as src_open, but the in-place paths come from an immutable epoch held by
 * the caller. Organized libraries do not need the catalog in either mode. */
static int src_open_snapshot(title_src_t *t, const char *root, const char *tid, const config_t *cfg, const catsnap_t *snap) {
    memset(t, 0, sizeof(*t));
    if (in_place_cfg(cfg)) {
        const catsnap_game_t *g = catsnap_find(snap, tid);
        if (!g)
            return -1;
        snprintf(t->app_path, sizeof(t->app_path), "%s", catsnap_path(snap, g->app_off));
        if (g->patch_off != CATSNAP_NO_PATH)
            snprintf(t->patch_path, sizeof(t->patch_path), "%s", catsnap_path(snap, g->patch_off));
        game_location(root, tid, t->app_path, t->dir, sizeof(t->dir));
        addon_view_t view = {NULL, NULL, snap, g};
        return src_open_paths(t, tid, &view, cfg);
    }
    snprintf(t->dir, sizeof(t->dir), "%s/ps4/%s", root, tid);
    snprintf(t->app_path, sizeof(t->app_path), "%s/app.pkg", t->dir);
    snprintf(t->patch_path, sizeof(t->patch_path), "%s/patch.pkg", t->dir);
    return src_open_paths(t, tid, NULL, cfg);
}

/* --- what each game is, kept between looks (titlecache.c) --- */

/* One file in the hash: its path, size and time. A missing one still counts (a pkg that
 * appears or goes away changes the fingerprint). */
static uint64_t fp_stat(uint64_t h, const char *path, fp_stat_kind_t kind, fp_counts_t *fc) {
    struct stat st;
    fp_counts_note_stat(fc, kind, path);
    if (!path[0] || stat(path, &st))
        return title_fp_add(h, path, UINT64_MAX, 0);
    return title_fp_add(h, path, (uint64_t)st.st_size, (int64_t)st.st_mtime);
}

/* The add-ons of the game, added to h; order independent (they come from a folder or a
 * catalog, in no fixed order). */
static uint64_t fp_addons(uint64_t h, const char *root, const char *tid, int inplace, const catsnap_t *snap,
                          fp_counts_t *fc) {
    char x[40];
    uint64_t dh = 0;
    int nd = 0;
    if (inplace) {
        if (snap) {
            const catsnap_game_t *g = catsnap_find(snap, tid);
            const catsnap_dlc_t *dlcs = catsnap_dlcs((catsnap_t *)snap);
            uint32_t nl = g ? g->ndlc : 0;
            for (uint32_t k = 0; k < nl; k++) {
                const catsnap_dlc_t *d = &dlcs[g->first_dlc + k];
                char rm[96];
                uint64_t v = fp_stat(title_fp_start(), catsnap_path(snap, d->path_off), FP_STAT_AC, fc);
                snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", d->content_id);
                if (exists(rm)) /* deleted on the console: src_open leaves it out too */
                    v ^= 0x9e3779b97f4a7c15ull;
                dh ^= v;
                nd++;
            }
        } else {
            const catalog_t *c = drive_catalog(root);
            const cat_game_t *g = c ? catalog_game(c, tid) : NULL;
            const cat_pkg_t *list[CAT_MAX_DLC];
            int nl = g ? catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC) : 0;
            for (int k = 0; k < nl; k++) {
                char rm[96];
                uint64_t v = fp_stat(title_fp_start(), list[k]->path, FP_STAT_AC, fc);
                snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", list[k]->content_id);
                if (exists(rm)) /* deleted on the console: src_open leaves it out too */
                    v ^= 0x9e3779b97f4a7c15ull;
                dh ^= v;
                nd++;
            }
        }
    } else {
        char base[PATH_MAX];
        snprintf(base, sizeof(base), "%s/ps4/%s/addcont", root, tid);
        DIR *d = opendir(base);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (!is_game_id(e->d_name))
                continue;
            char sub[PATH_MAX];
            snprintf(sub, sizeof(sub), "%s/%s", base, e->d_name);
            DIR *l = opendir(sub);
            struct dirent *f;
            while (l && (f = readdir(l))) {
                if (f->d_name[0] == '.' || strlen(f->d_name) != 16)
                    continue;
                char p[PATH_MAX];
                snprintf(p, sizeof(p), "%s/%s/ac.pkg", sub, f->d_name);
                dh ^= fp_stat(title_fp_start(), p, FP_STAT_AC, fc);
                nd++;
            }
            if (l)
                closedir(l);
        }
        if (d)
            closedir(d);
    }
    snprintf(x, sizeof(x), "addons %016llx", (unsigned long long)dh);
    return title_fp_add(h, x, (uint64_t)nd, 0);
}

/* The hash of the files of tid on root, from stats only: reading a pkg is a megabyte
 * where each of these is one getattr. */
static uint64_t title_files_fp_mode(const char *root, const char *tid, const config_t *cfg, const catsnap_t *snap,
                                    fp_counts_t *fc) {
    char app[PATH_MAX], patch[PATH_MAX];
    int inplace = in_place_cfg(cfg);
    app[0] = patch[0] = 0;
    if (inplace) {
        if (snap) {
            const catsnap_game_t *g = catsnap_find(snap, tid);
            if (!g)
                return 0;
            snprintf(app, sizeof(app), "%s", catsnap_path(snap, g->app_off));
            if (g->patch_off != CATSNAP_NO_PATH)
                snprintf(patch, sizeof(patch), "%s", catsnap_path(snap, g->patch_off));
        } else {
            const catalog_t *c = drive_catalog(root);
            const cat_game_t *g = c ? catalog_game(c, tid) : NULL;
            if (!g)
                return 0;
            snprintf(app, sizeof(app), "%s", c->pkgs[g->app].path);
            const cat_pkg_t *p = catalog_patch(g_cat, c, g);
            if (p)
                snprintf(patch, sizeof(patch), "%s", p->path);
        }
    } else {
        char dir[PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/ps4/%s", root, tid);
        snprintf(app, sizeof(app), "%s/app.pkg", dir);
        snprintf(patch, sizeof(patch), "%s/patch.pkg", dir);
    }
    uint64_t h = fp_stat(title_fp_start(), app, FP_STAT_APP, fc);
    h = fp_stat(h, patch, FP_STAT_PATCH, fc);
    /* an update left out (or tried again) changes what the game is */
    char mark[96];
    h = fp_stat(h, badpatch_path(tid, mark, sizeof(mark)), FP_STAT_BADPATCH, fc);
    h = title_fp_add(h, cfg->dlc ? "add-ons" : "no add-ons", 0, cfg->dlc_mode | (cfg->dlc_big ? 4 : 0));
    return cfg->dlc ? fp_addons(h, root, tid, inplace, snap, fc) : h;
}

static uint64_t title_files_fp(const char *root, const char *tid, const config_t *cfg) {
    return title_files_fp_mode(root, tid, cfg, NULL, g_scan_m.active ? &g_scan_m.fp : NULL);
}

static void info_from_src(const title_src_t *t, title_info_t *e) {
    snprintf(e->title_id, sizeof(e->title_id), "%s", t->app.title_id);
    snprintf(e->dir, sizeof(e->dir), "%s", t->dir);
    snprintf(e->app_path, sizeof(e->app_path), "%s", t->app_path);
    snprintf(e->patch_path, sizeof(e->patch_path), "%s", t->patch_path);
    e->has_patch = t->has_patch;
    e->app_size = t->app.pkg_size;
    e->patch_size = t->has_patch ? t->patch.pkg_size : 0;
    snprintf(e->name, sizeof(e->name), "%s", t->name);
    snprintf(e->ver, sizeof(e->ver), "%s", t->ver);
    snprintf(e->content_id, sizeof(e->content_id), "%s", t->app.content_id);
    snprintf(e->update_issue, sizeof(e->update_issue), "%s", t->update_issue);
    if (title_marker_tail(t->marker, t->dir, e->marker_tail, sizeof(e->marker_tail)))
        e->marker_tail[0] = 0; /* impossible for a generated marker; it simply never matches */
    const pkg_meta_t *eff = t->has_patch ? &t->patch_meta : &t->app_meta;
    e->system_ver = eff->system_ver_valid ? eff->system_ver : 0;
    snprintf(e->game_ver, sizeof(e->game_ver), "%s", t->app_meta.app_ver_valid ? t->app_meta.app_ver : "");
    if (t->has_patch) {
        memcpy(e->sfo_sha, t->patch_sfo_sha, sizeof(e->sfo_sha));
        e->sfo_len = t->patch_sfo_len;
    }
    e->ndlc_tids = t->ndlc_tids;
    memcpy(e->dlc_tids, t->dlc_tids, sizeof(e->dlc_tids));
    if (t->ndlc > 0) {
        title_dlc_t *d = title_cache_give_dlc(e, t->ndlc);
        if (d)
            for (int i = 0; i < t->ndlc; i++) {
                snprintf(d[i].title_id, sizeof(d[i].title_id), "%s", t->dlc[i].pkg.title_id);
                snprintf(d[i].content_id, sizeof(d[i].content_id), "%s", t->dlc[i].pkg.content_id);
                d[i].install = t->dlc[i].install;
            }
        e->ndlc = d ? t->ndlc : 0; /* no room: the game goes without them */
    }
}

/* Cache reads/mutations are serialized at this boundary. A fill stays inside it because
 * the scan owner and panel workers share the same evidence (TCACHE-4); every consumer receives
 * an owned copy, so no cache pointer escapes while a later scan rewrites entries. */
static pthread_mutex_t g_titlecache_mu = PTHREAD_MUTEX_INITIALIZER;

/* What tid is, read from its pkgs only when its files changed since the last look (or it
 * was never looked at). NULL when the drive has no such game. */
static const title_info_t *title_info_fp_impl(const char *root, const char *tid, uint64_t fp, const config_t *cfg,
                                              const catsnap_t *snap, fp_counts_t *fc) {
    if (!fp)
        return NULL;
    /* the games are kept per drive, not per path */
    char key[64];
    root_id(root, key, sizeof(key));
    const title_info_t *hit = title_cache_find(key, tid, fp);
    if (hit)
        return hit;
    if (fc)
        fc->cache_miss++;
    title_src_t src;
    int opened = snap ? src_open_snapshot(&src, root, tid, cfg, snap) : src_open(&src, root, tid, cfg);
    if (opened)
        return NULL;
    if (fc) {
        fp_counts_note_usb_io(fc, src.app_path, src.app.read_ops, src.app.bytes_read);
        if (src.has_patch)
            fp_counts_note_usb_io(fc, src.patch_path, src.patch.read_ops, src.patch.bytes_read);
        for (int i = 0; i < src.ndlc; i++)
            fp_counts_note_usb_io(fc, src.dlc[i].path, src.dlc[i].pkg.read_ops, src.dlc[i].pkg.bytes_read);
    }
    title_info_t *e = title_cache_add(key, tid, fp);
    if (e)
        info_from_src(&src, e);
    src_close(&src);
    return e;
}

/* Owned copy of one cache entry. The fixed add-on storage keeps the copy independent of the
 * cache's pool even when a later scan reuses or rewrites that entry. No cache pointer leaves
 * the mutex: scan and panel consumers both use this representation. */
typedef struct {
    title_info_t info;
    title_dlc_t dlc[TITLE_DLC_MAX];
} title_info_copy_t;

static void title_info_copy_data(title_info_copy_t *out, const title_info_t *src) {
    memcpy(&out->info, src, sizeof(out->info));
    if (src->ndlc > 0 && src->dlc) {
        int ndlc = src->ndlc < TITLE_DLC_MAX ? src->ndlc : TITLE_DLC_MAX;
        memcpy(out->dlc, src->dlc, (size_t)ndlc * sizeof(out->dlc[0]));
        out->info.ndlc = ndlc;
        out->info.dlc = out->dlc;
    } else {
        out->info.dlc = NULL;
    }
}

static int title_info_fp_copy(const char *root, const char *tid, uint64_t fp, const config_t *cfg, const catsnap_t *snap,
                              title_info_copy_t *out, fp_counts_t *fc) {
    pthread_mutex_lock(&g_titlecache_mu);
    const title_info_t *t = title_info_fp_impl(root, tid, fp, cfg, snap, fc);
    if (t)
        title_info_copy_data(out, t);
    pthread_mutex_unlock(&g_titlecache_mu);
    return t ? 0 : -1;
}

/* Cache lookup for a worker using a catalog epoch. The fingerprint is taken before the cache
 * mutex; the fill/copy itself is serialized so no live cache pointer escapes to the panel. */
static int title_info_snapshot_copy(const char *root, const char *tid, const config_t *cfg, const catsnap_t *snap,
                                    title_info_copy_t *out, fp_counts_t *fc) {
    uint64_t fp = title_files_fp_mode(root, tid, cfg, snap, fc);
    if (!fp)
        return -1;
    return title_info_fp_copy(root, tid, fp, cfg, snap, out, fc);
}

static void write_meta(pkg_t *pkg, const char *meta_dir, int *rc) {
    char q[PATH_MAX];
    for (const meta_entry_t *m = k_user_meta; m->name; m++) {
        snprintf(q, sizeof(q), "%s/%s", meta_dir, m->name);
        *rc |= write_entry(pkg, m->id, q) < 0;
    }
    for (uint32_t id = 0x1260; id <= 0x127f; id++) {
        size_t len;
        uint8_t *d = pkg_entry(pkg, id, &len);
        if (!d)
            continue;
        snprintf(q, sizeof(q), "%s/changeinfo", meta_dir);
        mkdirs(q, 0777);
        if (id == 0x1260)
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo.xml", meta_dir);
        else
            snprintf(q, sizeof(q), "%s/changeinfo/changeinfo_%02u.xml", meta_dir, id - 0x1261);
        *rc |= write_file(q, d, len, 0777);
        mem_free(d);
    }
}

static void rm_tree(const char *path) {
    struct stat root;
    if (lstat(path, &root))
        return;
    if (!S_ISDIR(root.st_mode)) {
        unlink(path);
        return;
    }
    DIR *d = opendir(path);
    if (!d) {
        unlink(path);
        return;
    }
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (snprintf(p, sizeof(p), "%s/%s", path, e->d_name) >= (int)sizeof(p))
            continue;
        struct stat st;
        if (!lstat(p, &st) && S_ISDIR(st.st_mode))
            rm_tree(p);
        else
            unlink(p);
    }
    closedir(d);
    rmdir(path);
}

static void hold_path(char *out, size_t n, const char *kind, const char *tid) {
    snprintf(out, n, HOLD_DIR "/%s/%s", kind, tid);
}

/* Folder where 0.1.0-beta builds before the release mounted read-only views of the
 * libraries for the links: BGFT opens a game's pkg for writing when the game starts, and
 * through a read-only view it records EROFS in the game task and refuses the game. Links
 * ending there are written again. */
#define RO_DIR HOLD_DIR "/ro"

/* Fresh internal folder for kind ("app"/"patch") with name -> pkg_path. */
static int hold_prepare(const char *kind, const char *tid, const char *name, const char *pkg_path) {
    char dir[PATH_MAX], link[PATH_MAX];
    hold_path(dir, sizeof(dir), kind, tid);
    mkdirs(dir, 0777);
    snprintf(link, sizeof(link), "%s/%s", dir, name);
    unlink(link);
    if (symlink(pkg_path, link)) {
        printf("[reg] symlink %s: %s\n", link, strerror(errno));
        return -1;
    }
    return 0;
}

/* The link of kind reaches its pkg, not through a read-only view (see RO_DIR). readlink
 * fails for payloads here (0xffffffd1), so the link is followed instead: it must end on
 * the pkg itself. */
static int hold_ready(const char *kind, const char *tid, const char *name, const char *pkg_path) {
    char p[PATH_MAX];
    struct stat a, b;
    struct statfs sf;
    if (g_scan_m.active) {
        g_scan_m.hold_ready++;
        if (is_usb_storage_path(pkg_path))
            g_scan_m.usb_stat += 2; /* hold symlink stat + direct pkg_path stat */
    }
    hold_path(p, sizeof(p), kind, tid);
    snprintf(p + strlen(p), sizeof(p) - strlen(p), "/%s", name);
    if (stat(p, &a) || stat(pkg_path, &b) || statfs(p, &sf))
        return 0;
    return a.st_ino == b.st_ino && strncmp(sf.f_mntonname, RO_DIR "/", sizeof(RO_DIR));
}

static uint8_t *read_all(const char *path, size_t *len) {
    struct stat st;
    *len = 0;
    if (stat(path, &st) || st.st_size <= 0 || st.st_size > (1 << 20))
        return NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    uint8_t *d = mem_alloc((size_t)st.st_size);
    if (d && read(fd, d, (size_t)st.st_size) != st.st_size) {
        mem_free(d);
        d = NULL;
    }
    close(fd);
    if (d)
        *len = (size_t)st.st_size;
    return d;
}

/* Adds or replaces the license slot of pkg in the rif/idx of its content ID prefix,
 * keeping the slots of the game and of other add-ons. */
static int place_license(pkg_t *pkg) {
    size_t lic_len, rl, il;
    uint8_t *lic = pkg_entry(pkg, 0x400, &lic_len);
    if (!lic || lic_len != 0x400) {
        mem_free(lic);
        printf("[reg] %s: no license.dat\n", pkg->content_id);
        return -1;
    }
    char rp[96], ip[96];
    /* the PS4 names them free<CID> (the prefix differs on the PS5) */
    snprintf(rp, sizeof(rp), "/user/license/free%.19s.rif", pkg->content_id);
    snprintf(ip, sizeof(ip), "/user/license/free%.19s.idx", pkg->content_id);
    uint8_t *old_rif = read_all(rp, &rl), *old_idx = read_all(ip, &il);
    buf_t rif = {0}, idx = {0};
    build_license(&rif, &idx, old_rif, rl, old_idx, il, pkg->content_id, lic);
    int rc = write_buf(rp, &rif) | write_buf(ip, &idx);
    mem_free(lic);
    mem_free(old_rif);
    mem_free(old_idx);
    buf_free(&rif);
    buf_free(&idx);
    return rc ? -1 : 0;
}

/* Internal folder of an add-on: HOLD_DIR/addcont/<ADD-ON TID>/<LABEL>, mounted (by
 * title ID) over /user/addcont/<ADD-ON TID>. It holds ac.pkg as a symlink to the pkg on
 * the drive plus ac.json/ac.pbm. Also writes the PlayGo records, the license slot and,
 * for add-ons with data, the icon. */
static int place_dlc(dlc_src_t *a) {
    pkg_t *pkg = &a->pkg;
    const char *ver = a->meta.version_valid ? a->meta.version : "01.00", *label = pkg->content_id + 20;
    char dir[PATH_MAX], p[PATH_MAX], q[PATH_MAX];
    snprintf(dir, sizeof(dir), HOLD_DIR "/addcont/%s/%s", pkg->title_id, label);
    mkdirs(dir, 0777);
    buf_t json = {0}, pbm = {0}, pdb = {0};
    char url[PATH_MAX + 8], link[PATH_MAX], kind[24];
    snprintf(kind, sizeof(kind), "ac-%.16s", label);
    pkg_url(pkg->title_id, kind, a->path, url, sizeof(url));
    pkg_link(pkg->title_id, kind, a->path, link, sizeof(link));
    snprintf(q, sizeof(q), "%s/ac.pkg", dir);
    unlink(q);
    int rc = 0;
    if (symlink(link, q)) {
        printf("[reg] symlink %s: %s\n", q, strerror(errno));
        rc = -1;
    }
    build_app_json(&json, pkg, url);
    build_app_pbm(&pbm, pkg, ver, a->data ? PBM_ADDCONT : PBM_LICENSE);
    build_ac_pdb(&pdb, pkg, url, &json);
    snprintf(q, sizeof(q), "%s/ac.json", dir);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/ac.pbm", dir);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/ac.pbm.backup", dir);
    rc |= write_buf(q, &pbm);
    snprintf(p, sizeof(p), "/user/playgo/addcont/%s/00", pkg->content_id);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/d0.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/d1.pdb", p);
    rc |= write_buf(q, &pdb);
    snprintf(q, sizeof(q), "%s/f0.pdb", p);
    rc |= write_file(q, "", 0, 0777);
    rc |= place_license(pkg);
    if (a->data) {
        snprintf(p, sizeof(p), "/user/appmeta/addcont/%s/%s", pkg->title_id, label);
        mkdirs(p, 0777);
        snprintf(q, sizeof(q), "%s/icon0.png", p);
        rc |= write_entry(pkg, 0x1200, q) < 0;
    }
    buf_free(&json);
    buf_free(&pbm);
    buf_free(&pdb);
    if (rc)
        printf("[reg] add-on %s: could not write its files\n", pkg->content_id);
    return rc ? -1 : 0;
}

/* The ac.pkg link of every add-on exists and reaches its pkg. */
/* 1 when the add-on is installed the system's way: its own pkg in /user/addcont, not
 * PS4Mount's mount there. */
static int detach_target(const char *target, const char *expected, int force);

static int dlc_installed_ids(const char *dtid, const char *cid) {
    char p[96];
    struct stat st;
    snprintf(p, sizeof(p), "/user/addcont/%s", dtid);
    if (mounted_from(p, NULL, 0) || dead_layer(p)) {
        /* an older PS4Mount left the add-on folder mounted over the real one: what the
         * system installed is below it, and would be installed again on every registration */
        char expected[PATH_MAX];
        hold_path(expected, sizeof(expected), "addcont", dtid);
        detach_target(p, expected, 1);
    }
    snprintf(p, sizeof(p), "/user/addcont/%s/%s/ac.pkg", dtid, cid + 20);
    return !lstat(p, &st) && S_ISREG(st.st_mode);
}

static int dlc_installed(const dlc_src_t *a) { return dlc_installed_ids(a->pkg.title_id, a->pkg.content_id); }

/* Add-ons the system's installer would not take in this run: the game goes on without
 * them (and is not registered again for them on every scan); the next PS4Mount tries
 * again, after the restart a new game needs anyway. */
#define MAX_DLC_FAILED 64
static char g_dlc_failed[MAX_DLC_FAILED][40];
static int g_ndlc_failed;

static int dlc_failed(const char *content_id) {
    for (int i = 0; i < g_ndlc_failed; i++)
        if (!strcmp(g_dlc_failed[i], content_id))
            return 1;
    return 0;
}

/* Add-ons the installer would not take in an earlier load, with the size and digest of the
 * pkg they were: they are not tried again until that pkg changes, so a backup the system
 * refuses is not installed again at every load. Delete the file to try them all again. */
#define MAX_DLC_REFUSED 256
#define DLC_REFUSED_LIST STATE_DIR "/dlcfail.txt"
typedef struct {
    char content_id[40];
    uint64_t size;
    char digest[65];
} dlc_refused_t;
static dlc_refused_t g_dlc_refused[MAX_DLC_REFUSED];
static int g_ndlc_refused, g_dlc_refused_loaded;

static void dlc_hex(const uint8_t d[32], char out[65]) {
    for (int i = 0; i < 32; i++)
        snprintf(out + i * 2, 3, "%02x", d[i]);
}

static void dlc_refused_load(void) {
    if (g_dlc_refused_loaded)
        return;
    g_dlc_refused_loaded = 1;
    FILE *f = fopen(DLC_REFUSED_LIST, "r");
    char line[160];
    if (!f)
        return;
    while (g_ndlc_refused < MAX_DLC_REFUSED && fgets(line, sizeof(line), f)) {
        dlc_refused_t *r = &g_dlc_refused[g_ndlc_refused];
        unsigned long long sz = 0;
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "%39s %llu %64s", r->content_id, &sz, r->digest) == 3) {
            r->size = sz;
            g_ndlc_refused++;
        }
    }
    fclose(f);
}

static int dlc_refused(const dlc_src_t *a) {
    char dig[65];
    dlc_refused_load();
    dlc_hex(a->pkg.digest, dig);
    for (int i = 0; i < g_ndlc_refused; i++)
        if (!strcmp(g_dlc_refused[i].content_id, a->pkg.content_id) && g_dlc_refused[i].size == a->pkg.pkg_size &&
            !strcmp(g_dlc_refused[i].digest, dig))
            return 1;
    return 0;
}

static void dlc_refused_add(const dlc_src_t *a) {
    dlc_refused_load();
    int i;
    for (i = 0; i < g_ndlc_refused; i++)
        if (!strcmp(g_dlc_refused[i].content_id, a->pkg.content_id))
            break;
    if (i == g_ndlc_refused) {
        if (i >= MAX_DLC_REFUSED) { /* the oldest makes room */
            memmove(&g_dlc_refused[0], &g_dlc_refused[1], sizeof(g_dlc_refused[0]) * (MAX_DLC_REFUSED - 1));
            i = MAX_DLC_REFUSED - 1;
        } else {
            g_ndlc_refused++;
        }
    }
    dlc_refused_t *r = &g_dlc_refused[i];
    snprintf(r->content_id, sizeof(r->content_id), "%s", a->pkg.content_id);
    r->size = a->pkg.pkg_size;
    dlc_hex(a->pkg.digest, r->digest);
    FILE *f = fopen(DLC_REFUSED_LIST, "w");
    if (!f)
        return;
    for (int k = 0; k < g_ndlc_refused; k++)
        fprintf(f, "%s\t%llu\t%s\n", g_dlc_refused[k].content_id, (unsigned long long)g_dlc_refused[k].size,
                g_dlc_refused[k].digest);
    fclose(f);
}

/* Big add-ons being installed: the system copies them in the background, minutes for a
 * few GB, while PS4Mount goes on. Each one is said when it is there. */
#define MAX_DLC_PENDING 64
#define DLC_SPACE_MARGIN (2ull << 30) /* left free on the console after queued installs */
/* Bytes submitted to BGFT in this payload run. Installed add-ons keep occupying /user after
 * they finish, so the reservation is deliberately not lowered; a failed one is retried after
 * the next load reevaluates the real disk. */
static uint64_t g_dlc_install_reserved;
#define DLC_PENDING_SECONDS (2 * 3600) /* an install not done by then is given up for this run */

typedef struct {
    char content_id[40], name[128];
    uint64_t size;
    time_t since;
} dlc_pending_t;

static dlc_pending_t g_dlc_pending[MAX_DLC_PENDING];
static int g_ndlc_pending;

static int dlc_pending(const char *content_id) {
    for (int i = 0; i < g_ndlc_pending; i++)
        if (!strcmp(g_dlc_pending[i].content_id, content_id))
            return 1;
    return 0;
}

/* /user/addcont/<TID>/<LABEL>/ac.pkg is there, whole, and is the system's own (not mounted). */
static int dlc_copy_done(const char *content_id, uint64_t size) {
    char tid[10], dir[64], p[96];
    struct stat st;
    snprintf(tid, sizeof(tid), "%.9s", content_id + 7);
    snprintf(dir, sizeof(dir), "/user/addcont/%s", tid);
    if (mounted_from(dir, NULL, 0))
        return 0;
    snprintf(p, sizeof(p), "%s/%s/ac.pkg", dir, content_id + 20);
    return !lstat(p, &st) && S_ISREG(st.st_mode) && (uint64_t)st.st_size == size;
}

static int dlc_ready(const title_info_t *t) {
    char p[PATH_MAX];
    for (int i = 0; i < t->ndlc; i++) {
        const title_dlc_t *d = &t->dlc[i];
        if (d->install && (dlc_failed(d->content_id) || dlc_pending(d->content_id)))
            continue;
        snprintf(p, sizeof(p), HOLD_DIR "/addcont/%s/%s/ac.pkg", d->title_id, d->content_id + 20);
        if (!d->install && g_scan_m.active && is_usb_storage_path(t->dir))
            g_scan_m.usb_stat++;
        if (d->install ? !dlc_installed_ids(d->title_id, d->content_id) : !exists(p))
            return 0;
    }
    return 1;
}

/* --- BGFT game task (PS4) --- */

/* The PS4 starts a game only when its background transfer service (BGFT) has the
 * title's game task, /user/bgft/task/<ID>/d0.pdb and d1.pdb, and BGFT reads them at
 * boot: a game added (or whose update changed) plays after the next restart. The
 * ids used here are far above the ones the system gives its own tasks. */
#define BGFT_DIR "/user/bgft/task"
#define BGFT_FIRST 0xf001
#define BGFT_LAST 0xffff

static uint32_t g_fw; /* kernel_get_fw_version(), written in the tasks */

/* Titles whose game task was written in this run. */
static char g_reboot_tids[MAX_TITLES][10];
static int g_nreboot;
static pthread_mutex_t g_reboot_mu = PTHREAD_MUTEX_INITIALIZER;

static void reboot_record_locked(const char *tid) {
    for (int i = 0; i < g_nreboot; i++)
        if (!strcmp(g_reboot_tids[i], tid))
            return;
    if (g_nreboot < MAX_TITLES)
        snprintf(g_reboot_tids[g_nreboot++], 10, "%s", tid);
}

static int same_file(const char *path, const void *data, size_t size);

/* When the console booted (0 when unknown). */
/* When the console booted: the clock less the time since boot. (kern.boottime is
 * refused to payloads: it gave 0 and every restart mark was dropped as stale.) It moves
 * by a second or so between calls, and with a clock set over the network. */
static long boot_time(void) {
    struct timespec up;
    if (clock_gettime(CLOCK_MONOTONIC, &up))
        return 0;
    return (long)time(NULL) - (long)up.tv_sec;
}

#define SAME_BOOT_SECONDS 60 /* a console takes longer than this to boot again */

/* STATE_DIR/<TID>.restart holds the boot time of when its task was written: the game
 * needs a restart while the console has not booted since (a PS4Mount loaded again keeps
 * knowing it). */
static int needs_reboot(const char *tid) {
    pthread_mutex_lock(&g_reboot_mu);
    for (int i = 0; i < g_nreboot; i++)
        if (!strcmp(g_reboot_tids[i], tid)) {
            pthread_mutex_unlock(&g_reboot_mu);
            return 1;
        }
    char p[64], buf[32] = "";
    snprintf(p, sizeof(p), STATE_DIR "/%s.restart", tid);
    int fd = open(p, O_RDONLY);
    if (fd < 0) {
        pthread_mutex_unlock(&g_reboot_mu);
        return 0;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    long boot = boot_time(), when = n > 0 ? atol(buf) : -1;
    if (boot && when > 0 && labs(when - boot) <= SAME_BOOT_SECONDS) {
        reboot_record_locked(tid);
        pthread_mutex_unlock(&g_reboot_mu);
        return 1;
    }
    unlink(p); /* booted since: BGFT has read the task */
    pthread_mutex_unlock(&g_reboot_mu);
    return 0;
}

/* Mounts do not survive the restart a new game needs (PANEL-9): without AutoRun the user
 * has to load Lodestar again before playing. */
static const char *after_restart(void) {
    return g_cfg.autorun ? "" : TR(" Then load Lodestar again", " Después vuelve a cargar Lodestar");
}

static void mark_reboot(const char *tid) {
    char p[64], tmp[80], buf[32];
    snprintf(p, sizeof(p), STATE_DIR "/%s.restart", tid);
    snprintf(tmp, sizeof(tmp), STATE_DIR "/%s.restart.tmp", tid);
    int n = snprintf(buf, sizeof(buf), "%ld\n", boot_time());
    pthread_mutex_lock(&g_reboot_mu);
    reboot_record_locked(tid);
    if (!write_file(tmp, buf, (size_t)n, 0666)) {
        if (rename(tmp, p))
            unlink(tmp);
    } else {
        unlink(tmp);
    }
    pthread_mutex_unlock(&g_reboot_mu);
}

static int contains(const uint8_t *d, size_t n, const char *s) {
    size_t l = strlen(s);
    for (size_t i = 0; i + l <= n; i++)
        if (!memcmp(d + i, s, l))
            return 1;
    return 0;
}

static bgftscan_t g_bgftscan;
static bgft_taken_t g_bgft_taken;

/* install.c reports every task id the system assigns, before that task is started. */
void bgft_task_id_taken(uint32_t id) { bgft_taken_add(&g_bgft_taken, id); }

/* A task write/remove is rare. Invalidating the whole scan snapshot keeps the ordering
 * proof simple: later title_state calls fall back to fresh direct reads until the next
 * batch reload. This is synchronous main-loop state, not shared with the install threads. */
static void bgftscan_invalidate(void) { g_bgftscan.usable = 0; }

/* Fresh positive evidence for one task id: canonical title identity, or the legacy
 * /user/app path shape older tasks carry. Used to keep snapshot hits honest before mutation. */
static int bgft_task_identifies(uint32_t id, const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), BGFT_DIR "/%08x/d0.pdb", id);
    size_t len;
    uint8_t *data = id ? read_all(p, &len) : NULL;
    if (!data)
        return 0;
    if (g_scan_m.active) {
        g_scan_m.bgft_pdb_reads++;
        g_scan_m.bgft_pdb_bytes += len;
    }
    bgft_info_t info;
    int match = 0;
    if (!bgft_task_parse(data, len, &info) && info.has_title)
        match = !strcmp(info.title_id, tid);
    if (!match) {
        char legacy[10];
        match = bgft_legacy_title_id(data, len, legacy) && !strcmp(legacy, tid);
    }
    mem_free(data);
    return match;
}

/* The id of the game task of tid written here, 0 when there is none. */
static uint32_t bgft_find(const char *tid) {
    if (g_bgftscan.usable) {
        const bgftscan_entry_t *hit = bgftscan_find(&g_bgftscan, tid);
        if (!hit)
            return 0; /* negative evidence is valid only inside this serialized scan epoch */
        if (bgft_task_identifies(hit->id, tid))
            return hit->id;
        bgftscan_invalidate(); /* positive evidence did not survive a fresh read */
    }
    char want[48], p[64];
    snprintf(want, sizeof(want), "/user/app/%s/app.pkg", tid);
    DIR *d = opendir(BGFT_DIR);
    struct dirent *e;
    uint32_t found = 0;
    while (d && !found && (e = readdir(d))) {
        char *end;
        unsigned long id = strtoul(e->d_name, &end, 16);
        if (*end || id < BGFT_FIRST || id > BGFT_LAST)
            continue;
        if (g_scan_m.active)
            g_scan_m.bgft_dirs++;
        snprintf(p, sizeof(p), BGFT_DIR "/%s/d0.pdb", e->d_name);
        size_t len;
        uint8_t *data = read_all(p, &len);
        if (data && g_scan_m.active) {
            g_scan_m.bgft_pdb_reads++;
            g_scan_m.bgft_pdb_bytes += len;
        }
        if (data && contains(data, len, want))
            found = (uint32_t)id;
        mem_free(data);
    }
    if (d)
        closedir(d);
    return found;
}

/* Freshly read and parse one task for the fallback path. The fallback may cost more I/O
 * than the scan snapshot, but it must never use looser health semantics. */
static int bgft_read_info(uint32_t id, bgft_info_t *info) {
    char p[64];
    size_t len;
    snprintf(p, sizeof(p), BGFT_DIR "/%08x/d0.pdb", id);
    uint8_t *data = id ? read_all(p, &len) : NULL;
    if (data && g_scan_m.active) {
        g_scan_m.bgft_pdb_reads++;
        g_scan_m.bgft_pdb_bytes += len;
    }
    int ok = data && bgft_task_parse(data, len, info) == 0;
    mem_free(data);
    return ok ? 0 : -1;
}

/* 1 when the game task of tid canonically names the pkg at path (as the system writes it):
 * tasks written before /data paths were turned into /user/data ones are written again. */
static int bgft_names(const char *tid, const char *url) {
    uint32_t id = bgft_find(tid);
    bgft_info_t info;
    if (bgft_read_info(id, &info))
        return 0;
    return info.has_title && !strcmp(info.title_id, tid) && bgft_info_url_is(&info, url);
}

/* The error BGFT recorded in the game task of tid (record 0x68), 0 when none: a task
 * BGFT gave up on (it refuses the game until a restart with a clean task). A malformed
 * or conflicting task is unknown, so it answers nonzero rather than looking healthy. */
static uint32_t bgft_task_error(const char *tid) {
    uint32_t id = bgft_find(tid);
    bgft_info_t info;
    if (bgft_read_info(id, &info) || !info.has_title || strcmp(info.title_id, tid))
        return 1;
    return info.error_present ? info.error : 0;
}

/* A free odd id whose install task id (one below) is free too. This is a fresh scan even
 * inside a registration epoch: add-on installs create system tasks within that epoch. */
static uint32_t bgft_free_id(void) {
    return bgftscan_alloc_id(&g_bgftscan, &g_bgft_taken, BGFT_DIR, BGFT_FIRST, BGFT_LAST);
}

static void bgft_remove(const char *tid) {
    uint32_t id = bgft_find(tid);
    if (!id)
        return;
    char p[64];
    snprintf(p, sizeof(p), BGFT_DIR "/%08x", id);
    rm_tree(p);
    bgft_taken_add(&g_bgft_taken, id); /* BGFT keeps the removed task's state for this boot */
    bgftscan_invalidate();
    printf("[reg] %s: BGFT game task %08x removed\n", tid, id);
}

static int place_bgft(title_src_t *t, const buf_t *json, const patch_ref_t *pr) {
    const char *tid = t->app.title_id;
    uint32_t id = bgft_find(tid);
    int fresh = !id, rc = 0, changed = 0;
    if (fresh && !(id = bgft_free_id())) {
        printf("[reg] %s: no free BGFT task id\n", tid);
        return -1;
    }
    char dir[64], p[96];
    snprintf(dir, sizeof(dir), BGFT_DIR "/%08x", id);
    mkdirs(dir, 0770);
    /* creation time, microseconds since 0001-01-01; an existing task keeps its own */
    uint64_t now = ((uint64_t)time(NULL) + 62135596800ull) * 1000000ull;
    size_t len;
    snprintf(p, sizeof(p), "%s/d0.pdb", dir);
    uint8_t *old = read_all(p, &len);
    if (old && len >= 172 && old[152] == 0x76 && !old[153] && !old[154] && !old[155]) {
        now = 0;
        for (int i = 7; i >= 0; i--)
            now = now << 8 | old[164 + i];
    }
    mem_free(old);
    const char *name = strrchr(t->app_path, '/');
    name = name ? name + 1 : t->app_path;
    char url[PATH_MAX + 8];
    pkg_url(tid, "app", t->app_path, url, sizeof(url));
    buf_t written0 = {0};
    for (int d1 = 0; d1 < 2; d1++) {
        bgft_task_t bt = {d1, g_fw, now, id - 1, &t->app, name, url, json, pr};
        buf_t b = {0};
        build_bgft_task(&b, &bt);
        snprintf(p, sizeof(p), "%s/d%d.pdb", dir, d1);
        if (!same_file(p, b.data, b.len)) {
            rc |= write_buf(p, &b);
            changed = 1;
        }
        if (!d1)
            buf_put(&written0, b.data, b.len);
        buf_free(&b);
    }
    /* Whether bytes were written or both files were already byte-identical, d0 is fresh
     * proof here. A failed or uncertain write instead invalidates the whole epoch. */
    int indexed = !rc && g_bgftscan.usable && bgftscan_update(&g_bgftscan, id, written0.data, written0.len);
    if (!indexed)
        bgftscan_invalidate();
    if (changed) {
        printf("[reg] %s: BGFT game task %08x %s\n", tid, id, fresh ? "written" : "updated");
        mark_reboot(tid);
    }
    buf_free(&written0);
    return rc ? -1 : 0;
}

static void pump_events(int ms);

/* Installs a small add-on with the system's installer and waits for it (a minute at
 * most): the system copies the pkg into /user/addcont and writes its addcont.db record. */
/* 0 when installed, 1 when the installer would not take it now, 2 when it already would
 * not earlier in this run (see g_dlc_failed: said once). */
static uint64_t free_bytes(const char *dir);

/* 0 installed, 1 refused (tried again at the next load), 2 skipped, 3 started in the background. */
static int dlc_install(dlc_src_t *a) {
    if (dlc_installed(a))
        return 0;
    if (dlc_failed(a->pkg.content_id))
        return 2;
    if (dlc_refused(a)) { /* the same pkg was refused before: not again at every load */
        if (g_ndlc_failed < MAX_DLC_FAILED)
            snprintf(g_dlc_failed[g_ndlc_failed++], sizeof(g_dlc_failed[0]), "%s", a->pkg.content_id);
        return 2;
    }
    if (!a->pkg.pkg_size) {
        printf("[dlc] %s: a nonzero pkg whose header declares 0 bytes is damaged; not queued\n", a->pkg.content_id);
        dlc_refused_add(a);
        if (g_ndlc_failed < MAX_DLC_FAILED)
            snprintf(g_dlc_failed[g_ndlc_failed++], sizeof(g_dlc_failed[0]), "%s", a->pkg.content_id);
        return 2;
    }
    int big = a->pkg.pkg_size > DLC_INSTALL_MAX;
    if (big && dlc_pending(a->pkg.content_id))
        return 3;
    uint64_t fb_user = free_bytes("/user");
    uint64_t usable = fb_user > DLC_SPACE_MARGIN ? fb_user - DLC_SPACE_MARGIN : 0;
    int no_room = a->pkg.pkg_size > usable || g_dlc_install_reserved > usable - a->pkg.pkg_size;
    if (no_room || g_ndlc_pending == MAX_DLC_PENDING) {
        const char *dlc_name = a->meta.title_valid ? a->meta.title : a->pkg.content_id;
        notify_kind(NOTIFY_WARN, TR("Not enough space on the console to install the add-on %s (%llu MB): free some space and load Lodestar again",
                  "No hay espacio en la consola para instalar el DLC %s (%llu MB): libera espacio y vuelve a cargar Lodestar"),
               dlc_name, (unsigned long long)(a->pkg.pkg_size >> 20));
        if (g_ndlc_failed < MAX_DLC_FAILED)
            snprintf(g_dlc_failed[g_ndlc_failed++], sizeof(g_dlc_failed[0]), "%s", a->pkg.content_id);
        return 2;
    }
    char target[64], url[PATH_MAX + 8], msg[PATH_MAX + 64];
    snprintf(target, sizeof(target), "/user/addcont/%s", a->pkg.title_id);
    /* a folder mounted here by an older PS4Mount: the install must reach the real one.
     * Dead layers have no readable top, so prove the whole target clean before installing. */
    char expected[PATH_MAX];
    hold_path(expected, sizeof(expected), "addcont", a->pkg.title_id);
    if (detach_target(target, expected, 1)) {
        printf("[dlc] %s: install refused, its add-on folder could not be detached safely\n", a->pkg.content_id);
        return 1;
    }
    const char *dlc_name = a->meta.title_valid ? a->meta.title : a->pkg.content_id;
    int32_t user = 0;
    sceUserServiceGetForegroundUser(&user);
    char kind[24];
    snprintf(kind, sizeof(kind), "ac-%.16s", a->pkg.content_id + 20);
    int task = install_pkg(pkg_url(a->pkg.title_id, kind, a->path, url, sizeof(url)),
                           dlc_name, user, msg, sizeof(msg));
    printf("[dlc] %s: %s\n", a->pkg.content_id, msg);
    if (task >= 0) {
        if (UINT64_MAX - g_dlc_install_reserved < a->pkg.pkg_size)
            g_dlc_install_reserved = UINT64_MAX;
        else
            g_dlc_install_reserved += a->pkg.pkg_size;
    }
    if (big && task >= 0) { /* minutes: followed by dlc_pending_check() */
        pthread_mutex_lock(&g_act_mu);
        dlc_pending_t *p = &g_dlc_pending[g_ndlc_pending++];
        snprintf(p->content_id, sizeof(p->content_id), "%s", a->pkg.content_id);
        snprintf(p->name, sizeof(p->name), "%s", dlc_name);
        p->size = a->pkg.pkg_size;
        p->since = time(NULL);
        pthread_mutex_unlock(&g_act_mu);
        return 3;
    }
    for (int i = 0; task >= 0 && i < 120 && !dlc_installed(a); i++)
        pump_events(500);
    if (!dlc_installed(a)) {
        if (task >= 0)
            printf("[dlc] %s: not installed after a minute\n", a->pkg.content_id);
        if (g_ndlc_failed < MAX_DLC_FAILED)
            snprintf(g_dlc_failed[g_ndlc_failed++], sizeof(g_dlc_failed[0]), "%s", a->pkg.content_id);
        dlc_refused_add(a); /* not tried again at every load until its pkg changes */
        return 1;
    }
    printf("[dlc] %s installed\n", a->pkg.content_id);
    return 0;
}

/* Big add-ons that the system has finished installing are said; ones that never finish
 * are given up for this run. */
static void dlc_pending_check(void) {
    for (int i = 0; i < g_ndlc_pending;) {
        dlc_pending_t *p = &g_dlc_pending[i];
        int done = dlc_copy_done(p->content_id, p->size), late = time(NULL) - p->since > DLC_PENDING_SECONDS;
        if (!done && !late) {
            i++;
            continue;
        }
        if (done)
            notify_kind(NOTIFY_READY, TR("Add-on installed: %s", "DLC instalado: %s"), p->name);
        else {
            printf("[dlc] %s: not installed after %d s, given up\n", p->content_id, DLC_PENDING_SECONDS);
            if (g_ndlc_failed < MAX_DLC_FAILED)
                snprintf(g_dlc_failed[g_ndlc_failed++], sizeof(g_dlc_failed[0]), "%s", p->content_id);
        }
        printf("[dlc] %s %s\n", p->content_id, done ? "installed" : "given up");
        pthread_mutex_lock(&g_act_mu);
        *p = g_dlc_pending[--g_ndlc_pending];
        pthread_mutex_unlock(&g_act_mu);
    }
}

/* Installs the small add-ons of a registered game. One the installer refuses does not
 * keep the game out of the library: it is said, and tried again at the next load. */
static void install_title_dlc(title_src_t *t) {
    int left = 0, started = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < t->ndlc; i++) {
        int was = dlc_pending(t->dlc[i].pkg.content_id), rc = t->dlc[i].install ? dlc_install(&t->dlc[i]) : 0;
        left += rc == 1;
        if (rc == 3 && !was)
            started++, bytes += t->dlc[i].pkg.pkg_size;
    }
    if (started)
        notify(TR("%s: installing %d big add-on(s) (%llu.%llu GB) in the background; each one is said when it is ready",
                  "%s: instalando %d DLC grande(s) (%llu,%llu GB) en segundo plano; se avisa cuando cada uno esté listo"),
               t->name, started, (unsigned long long)(bytes * 10 >> 30) / 10, (unsigned long long)(bytes * 10 >> 30) % 10);
    if (left)
        notify_kind(NOTIFY_WARN, TR("%s: %d add-on(s) could not be installed now; the game is added without them and they are tried again "
                  "the next time Lodestar loads",
                  "%s: %d DLC no se pudieron instalar ahora; el juego se añade sin ellos y se vuelven a intentar la "
                  "próxima vez que cargues Lodestar"),
               t->name, left);
}

static int place_files(title_src_t *t) {
    pkg_t *app = &t->app, *pat = t->has_patch ? &t->patch : NULL;
    const char *tid = app->title_id;
    char p[PATH_MAX], q[PATH_MAX], hold[PATH_MAX];
    int rc = 0;

    snprintf(p, sizeof(p), "/user/appmeta/%s", tid);
    mkdirs(p, 0777);
    write_meta(app, p, &rc);
    if (pat)
        write_meta(pat, p, &rc); /* the update's artwork wins, as with a real install */

    snprintf(p, sizeof(p), "/system_data/priv/appmeta/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/param.sfo", p);
    rc |= write_entry(pat ? pat : app, 0x1000, q) != 0;
    snprintf(q, sizeof(q), "%s/nptitle.dat", p);
    rc |= write_entry(app, 0x402, q) < 0;
    snprintf(q, sizeof(q), "%s/npbind.dat", p);
    rc |= write_entry(app, 0x403, q) < 0;

    if (place_license(app))
        return -1;
    for (int i = 0; i < t->ndlc; i++)
        if (!t->dlc[i].install)
            rc |= place_dlc(&t->dlc[i]);
    /* the installer takes an add-on only for a game the system has: one not registered
     * yet gets them once it is (register_finish) */
    if (db_is_registered(tid))
        install_title_dlc(t);

    size_t pl = 0, ppl = 0;
    uint8_t *pg = pkg_entry(app, 0x1001, &pl), *ppg = pat ? pkg_entry(pat, 0x1001, &ppl) : NULL;
    buf_t json = {0}, pbm = {0}, xml = {0}, pjson = {0}, ppbm_patch = {0}, pxml = {0}, ppbm = {0};
    char app_url[PATH_MAX + 8], patch_url[PATH_MAX + 8], app_link[PATH_MAX], patch_link[PATH_MAX];
    build_app_json(&json, app, pkg_url(tid, "app", t->app_path, app_url, sizeof(app_url)));
    pkg_link(tid, "app", t->app_path, app_link, sizeof(app_link));
    /* the PS4 leaves no version and type 0 in app.pbm and patch.pbm (as every install
     * does on 13.52) */
    build_app_pbm(&pbm, app, "", 0);
    patch_ref_t pr = {0}, *prp = NULL;
    if (pat) {
        build_app_json(&pjson, pat, pkg_url(tid, "patch", t->patch_path, patch_url, sizeof(patch_url)));
        pkg_link(tid, "patch", t->patch_path, patch_link, sizeof(patch_link));
        build_app_pbm(&ppbm_patch, pat, "", 0);
        pr = (patch_ref_t){pat, ppg, ppl, t->ver, patch_url, &pjson};
        prp = &pr;
    }
    if (build_app_xml(&xml, pg, pl) || build_pfs_pbm(&ppbm, app, pg, pl, prp) ||
        (pat && build_app_xml(&pxml, ppg, ppl))) {
        printf("[reg] %s: playgo-chunk.dat unusable\n", tid);
        rc = -1;
        goto out;
    }
    rc |= hold_prepare("app", tid, "app.pkg", app_link);
    hold_path(hold, sizeof(hold), "app", tid);
    snprintf(q, sizeof(q), "%s/app.json", hold);
    rc |= write_buf(q, &json);
    snprintf(q, sizeof(q), "%s/app.pbm", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.pbm.backup", hold);
    rc |= write_buf(q, &pbm);
    snprintf(q, sizeof(q), "%s/app.xml", hold);
    rc |= write_buf(q, &xml);
    snprintf(p, sizeof(p), "/system_data/playgo/%s", tid);
    mkdirs(p, 0777);
    snprintf(q, sizeof(q), "%s/pfs.pbm", p);
    rc |= write_buf(q, &ppbm);
    snprintf(q, sizeof(q), "%s/pfs.pbm.backup", p);
    rc |= write_buf(q, &ppbm);
    rc |= place_bgft(t, &json, prp);
    if (pat) {
        rc |= hold_prepare("patch", tid, "patch.pkg", patch_link);
        hold_path(hold, sizeof(hold), "patch", tid);
        snprintf(q, sizeof(q), "%s/patch.json", hold);
        rc |= write_buf(q, &pjson);
        snprintf(q, sizeof(q), "%s/patch.pbm", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.pbm.backup", hold);
        rc |= write_buf(q, &ppbm_patch);
        snprintf(q, sizeof(q), "%s/patch.xml", hold);
        rc |= write_buf(q, &pxml);
    } else {
        hold_path(hold, sizeof(hold), "patch", tid);
        rm_tree(hold);
    }
out:
    mem_free(pg);
    mem_free(ppg);
    buf_free(&json);
    buf_free(&pbm);
    buf_free(&xml);
    buf_free(&pjson);
    buf_free(&ppbm_patch);
    buf_free(&pxml);
    buf_free(&ppbm);
    return rc ? -1 : 0;
}

#define IOV(s) {(void *)(s), strlen(s) + 1}

/* Titles this daemon serves. */
typedef struct {
    int used, has_patch, running, ndlc, ndlc_tids, mount_warned;
    char dlc_tids[MAX_DLC_TIDS][10];
    time_t running_since; /* launch seen in klog; the sandbox mount appears shortly after */
    char tid[10], cid[37], name[128], dir[PATH_MAX];
} managed_t;

static managed_t g_managed[MAX_TITLES];
/* Storage reads only the identity fields (used/tid). The owner keeps runtime/mount use of
 * the table; this short mutex only makes that identity snapshot consistent for workers. */
static pthread_mutex_t g_managed_id_mu = PTHREAD_MUTEX_INITIALIZER;

/* Small published mirror of "this title is running" for panel reads. g_managed remains
 * owner/runtime machinery; this list is synchronized because klog and scans update it. */
static char g_running_tids[MAX_TITLES][10];
static int g_nrunning;
static pthread_mutex_t g_running_mu = PTHREAD_MUTEX_INITIALIZER;

static void runtime_publish(const char *tid, int running) {
    pthread_mutex_lock(&g_running_mu);
    int at = -1;
    for (int i = 0; i < g_nrunning; i++)
        if (!strcmp(g_running_tids[i], tid))
            at = i;
    if (running && at < 0 && g_nrunning < MAX_TITLES)
        snprintf(g_running_tids[g_nrunning++], 10, "%s", tid);
    else if (!running && at >= 0) {
        memmove(g_running_tids[at], g_running_tids[at + 1], (size_t)(g_nrunning - at - 1) * sizeof(g_running_tids[0]));
        g_nrunning--;
    }
    pthread_mutex_unlock(&g_running_mu);
}

static int runtime_is_running(const char *tid) {
    int found = 0;
    pthread_mutex_lock(&g_running_mu);
    for (int i = 0; i < g_nrunning; i++)
        found |= !strcmp(g_running_tids[i], tid);
    pthread_mutex_unlock(&g_running_mu);
    return found;
}

static void runtime_clear(void) {
    pthread_mutex_lock(&g_running_mu);
    g_nrunning = 0;
    pthread_mutex_unlock(&g_running_mu);
}
static int g_scan_manual_active; /* main-loop epoch snapshot of a panel "scan for games" request */
static int g_deleting; /* a delete screen is open: game folders are unmounted */
static time_t g_deleting_since;
static time_t g_deleting_close_at; /* set when the uninstall finished */
/* A validated user delete is pending until the system proves the uninstall outcome.
 * The state is deliberately memory-only: losing it can re-add a title, never delete data. */
static char g_will_delete[10];
static time_t g_will_since, g_will_report_at;
static int g_will_report = -1; /* -1 no result yet, 0 success, 1 failure */
static int g_will_seen_registered;
static int g_will_mount_warned;

static int delete_pending_is(const char *tid) { return g_will_delete[0] && !strcmp(g_will_delete, tid); }

static void managed_mark_unused(managed_t *m) {
    char tid[10];
    pthread_mutex_lock(&g_managed_id_mu);
    snprintf(tid, sizeof(tid), "%s", m->tid);
    m->used = 0;
    pthread_mutex_unlock(&g_managed_id_mu);
    if (tid[0])
        runtime_publish(tid, 0);
}

static managed_t *managed_by(const char *tid, const char *cid) {
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used && ((tid && !strcmp(g_managed[i].tid, tid)) || (cid && !strcmp(g_managed[i].cid, cid))))
            return &g_managed[i];
    return NULL;
}

/* Top mount on target: 0 none, 1 our folder, -1 something else (like a USB
 * folder mounted by an older PS4Mount). */
static int top_layer(const char *target, const char *dir) {
    struct statfs sf;
    if (statfs(target, &sf) || strcmp(sf.f_mntonname, target))
        return 0;
    return strcmp(sf.f_fstypename, "nullfs") || strcmp(sf.f_mntfromname, dir) ? -1 : 1;
}

static void mount_table_invalidate(void);

static int nullfs_mount(const char *dir, const char *target) {
    struct iovec iov[] = {IOV("fstype"), IOV("nullfs"), IOV("from"), IOV(dir), IOV("fspath"), IOV(target)};
    int rc = nmount(iov, 6, 0);
    mount_table_invalidate();
    if (rc) {
        printf("[mount] %s -> %s: %s\n", dir, target, strerror(errno));
        return -1;
    }
    return 0;
}



/* A running PS4 title keeps /mnt/sandbox/pfsmnt/<TID>-app0 mounted. Unlike the klog
 * events this survives a restart of the daemon, so it is the reference for "running". */
/* Sandbox mounts of running titles, read from the mount table at most once a second
 * (the table holds over a thousand entries; reading it per title would be wasteful). */
static char (*g_sandboxes)[10];
static int g_nsandboxes;
static time_t g_sandboxes_at;

static void refresh_sandboxes(void) {
    if (g_sandboxes && time(NULL) == g_sandboxes_at)
        return;
    g_sandboxes_at = time(NULL);
    g_nsandboxes = 0;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return;
    struct statfs *list = mem_alloc(sizeof(*list) * (size_t)(n + 16));
    char (*ids)[10] = realloc(g_sandboxes, sizeof(*ids) * (size_t)(n + 16));
    if (ids)
        g_sandboxes = ids;
    if (!list || !ids) {
        mem_free(list);
        return;
    }
    scan_note_mount_read();
    n = getfsstat(list, (long)(sizeof(*list) * (size_t)(n + 16)), MNT_NOWAIT);
    static const char prefix[] = "/mnt/sandbox/pfsmnt/";
    for (int i = 0; i < n; i++) {
        const char *on = list[i].f_mntonname;
        if (strlen(on) != sizeof(prefix) - 1 + 9 + 5 || strncmp(on, prefix, sizeof(prefix) - 1) ||
            strcmp(on + sizeof(prefix) - 1 + 9, "-app0"))
            continue;
        snprintf(g_sandboxes[g_nsandboxes++], 10, "%.9s", on + sizeof(prefix) - 1);
    }
    mem_free(list);
}

static int title_running(const char *tid) {
    refresh_sandboxes();
    for (int i = 0; i < g_nsandboxes; i++)
        if (!strcmp(g_sandboxes[i], tid))
            return 1;
    return 0;
}

/* Set while /user/app shows only the games being registered (see mask_user_app): the
 * other game folders are hidden then, and must not be mounted or unmounted. */
static int g_masked;

static int masked_path(const char *target) {
    if (!g_masked || strncmp(target, "/user/app/", 10))
        return 0;
    printf("[mount] %s: skipped while /user/app shows only the new games\n", target);
    return 1;
}

typedef struct {
    int table_known, layers, all_owned;
    int top, top_exact, top_owned;
    char foreign_from[PATH_MAX], foreign_type[32];
} mount_obs_t;

/* The first line of <TID>.src is the library location written when the game was registered.
 * For app/patch targets it is the extra evidence that distinguishes a legacy direct drive
 * mount of PS4Mount from any other nullfs that happens to come from /mnt. */
static int mount_marker_dir(const char *target, char *out, size_t n) {
    char tid[10], path[64], buf[PATH_MAX + 256] = {0};
    mount_target_kind_t kind = mount_target_kind(target, tid);
    if (kind != MOUNT_TARGET_APP && kind != MOUNT_TARGET_PATCH)
        return 0;
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t len = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (len <= 0)
        return 0;
    buf[strcspn(buf, "\n")] = 0;
    if (!buf[0] || strlen(buf) >= n)
        return 0;
    snprintf(out, n, "%s", buf);
    return 1;
}

/* Full mount tables are large. One table may answer routine no-work observations for a
 * second, but never a mutation decision: before unmounting or mounting, mount_scan_fresh
 * replaces it and the pure planner runs again. Local mutations invalidate it at once. */
static struct statfs *g_mount_table;
static const struct statfs **g_mount_index;
static size_t g_mount_table_cap, g_mount_index_cap;
static int g_mount_table_n, g_mount_table_valid, g_mount_index_valid;
static time_t g_mount_table_at;

static void mount_table_invalidate(void) {
    g_mount_table_n = 0;
    g_mount_table_valid = 0;
    g_mount_index_valid = 0;
    g_mount_table_at = 0;
}

static int mount_index_cmp(const void *a, const void *b) {
    const struct statfs *aa = *(const struct statfs *const *)a;
    const struct statfs *bb = *(const struct statfs *const *)b;
    return strcmp(aa->f_mntonname, bb->f_mntonname);
}

/* Pointers sorted by mountpoint, built once for a snapshot. Layer order is not part of the
 * ownership decision: the live top comes from statfs and stacked layers are accepted only
 * when every visible layer is owned, so sorting cannot weaken the planner. */
static void mount_index_build(void) {
    g_mount_index_valid = 0;
    if ((size_t)g_mount_table_n > g_mount_index_cap) {
        const struct statfs **next = mem_alloc((size_t)g_mount_table_n * sizeof(*next));
        if (!next)
            return; /* the caller falls back to one linear scan of this snapshot */
        mem_free(g_mount_index);
        g_mount_index = next;
        g_mount_index_cap = (size_t)g_mount_table_n;
    }
    for (int i = 0; i < g_mount_table_n; i++)
        g_mount_index[i] = &g_mount_table[i];
    qsort(g_mount_index, (size_t)g_mount_table_n, sizeof(*g_mount_index), mount_index_cmp);
    g_mount_index_valid = 1;
}

static int mount_table(const struct statfs **out, int *nout, int fresh) {
    time_t now = time(NULL);
    if (!fresh && g_mount_table_valid && g_mount_table_at == now) {
        *out = g_mount_table;
        *nout = g_mount_table_n;
        return 0;
    }
    g_mount_table_valid = 0;
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0) {
        mount_table_invalidate();
        return -1;
    }
    size_t need = (size_t)n + 16;
    if (need > g_mount_table_cap) {
        struct statfs *next = mem_alloc(need * sizeof(*next));
        if (!next) {
            mount_table_invalidate();
            return -1;
        }
        mem_free(g_mount_table);
        g_mount_table = next;
        g_mount_table_cap = need;
    }
    scan_note_mount_read();
    n = getfsstat(g_mount_table, (long)(g_mount_table_cap * sizeof(*g_mount_table)), MNT_NOWAIT);
    if (n < 0) {
        mount_table_invalidate();
        return -1;
    }
    g_mount_table_n = n;
    if (fresh) {
        /* A one-off destructive/mutation observation is linear: sorting it would cost more
         * than the scan and its result must not become the routine no-work snapshot. */
        g_mount_table_valid = 0;
        g_mount_index_valid = 0;
        g_mount_table_at = 0;
    } else {
        g_mount_table_valid = 1;
        g_mount_table_at = now;
        mount_index_build();
    }
    *out = g_mount_table;
    *nout = n;
    return 0;
}

/* Observe both the live top (statfs, always fresh) and every table entry on the target
 * (getfsstat, cached only as MNT-7 allows). The two answer different questions: statfs
 * proves what the system would use now, while the table still shows dead layers whose
 * backing drive went away. */
static void mount_scan_mode(const char *target, const char *expected, mount_obs_t *o, int fresh) {
    memset(o, 0, sizeof(*o));
    o->all_owned = 1;
    char marker[PATH_MAX];
    int has_marker = mount_marker_dir(target, marker, sizeof(marker));
    struct statfs sf;
    if (!statfs(target, &sf) && !strcmp(sf.f_mntonname, target)) {
        mount_owner_t owner = mount_layer_owner(target, expected, sf.f_fstypename, sf.f_mntfromname,
                                                has_marker ? marker : NULL);
        o->top = 1;
        o->top_exact = owner == MOUNT_OWNER_EXACT;
        o->top_owned = mount_owner_owned(owner);
    }
    const struct statfs *list = NULL;
    int n = 0;
    if (mount_table(&list, &n, fresh))
        return;
    o->table_known = 1;
    int first = 0, count = n, indexed = !fresh && g_mount_index_valid;
    if (indexed) {
        int lo = 0, hi = n;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            int cmp = strcmp(g_mount_index[mid]->f_mntonname, target);
            if (cmp < 0)
                lo = mid + 1;
            else
                hi = mid;
        }
        first = lo;
        count = 0;
        while (first + count < n && !strcmp(g_mount_index[first + count]->f_mntonname, target))
            count++;
    }
    for (int i = 0; i < count; i++) {
        const struct statfs *m = indexed ? g_mount_index[first + i] : &list[i];
        if (!indexed && strcmp(m->f_mntonname, target))
            continue;
        o->layers++;
        mount_owner_t owner = mount_layer_owner(target, expected, m->f_fstypename, m->f_mntfromname,
                                                has_marker ? marker : NULL);
        if (!mount_owner_owned(owner)) {
            o->all_owned = 0;
            if (!o->foreign_type[0]) {
                snprintf(o->foreign_type, sizeof(o->foreign_type), "%s", m->f_fstypename);
                snprintf(o->foreign_from, sizeof(o->foreign_from), "%s", m->f_mntfromname);
            }
        }
    }
}

static void mount_scan(const char *target, const char *expected, mount_obs_t *o) {
    mount_scan_mode(target, expected, o, 0);
}

static void mount_scan_fresh(const char *target, const char *expected, mount_obs_t *o) {
    mount_scan_mode(target, expected, o, 1);
}

static int unmount_tracked(const char *target, int flags) {
    int rc = unmount(target, flags);
    mount_table_invalidate(); /* the syscall may have succeeded or partially raced: no stale table */
    return rc;
}

static int mount_refused(const char *target, const mount_obs_t *o, int unknown) {
    char key[PATH_MAX + 96];
    snprintf(key, sizeof(key), "mount-refused:%s:%s:%s", target, unknown ? "unknown" : o->foreign_type,
             unknown ? "" : o->foreign_from);
    if (!first_time(key))
        return -1; /* the scan will keep refusing it; the evidence is not repeated every 10 s */
    if (unknown)
        printf("[mount] %s: the mount table could not be read; leaving it as it is\n", target);
    else
        printf("[mount] %s: foreign layer (%s from %s), left alone\n", target,
               o->foreign_type[0] ? o->foreign_type : "unknown", o->foreign_from[0] ? o->foreign_from : "unknown");
    return -1;
}

/* Remove owned layers until the target has no layer at all. force may force the removal of
 * an owned layer, never of a foreign one; foreign or unknown ownership is a failure. */
static int detach_target_mode(const char *target, const char *expected, int force, int cache_ok) {
    if (masked_path(target))
        return 0;
    for (int i = 0; i < 8; i++) {
        mount_obs_t o;
        mount_scan_mode(target, expected, &o, !cache_ok);
        mount_step_t step = mount_detach_step(o.table_known, o.layers, o.top, o.top_owned, o.all_owned);
        if (step == MOUNT_STEP_DONE)
            return 0;
        if (step != MOUNT_STEP_UNMOUNT)
            return mount_refused(target, &o, step == MOUNT_STEP_UNKNOWN);
        if (cache_ok) { /* the first observation may be the snapshot; mutation never uses it */
            mount_scan_fresh(target, expected, &o);
            step = mount_detach_step(o.table_known, o.layers, o.top, o.top_owned, o.all_owned);
            if (step == MOUNT_STEP_DONE)
                return 0;
            if (step != MOUNT_STEP_UNMOUNT)
                return mount_refused(target, &o, step == MOUNT_STEP_UNKNOWN);
        }
        int flags = force || !o.top ? MNT_FORCE : 0;
        if (unmount_tracked(target, flags)) {
            printf("[mount] unmount %s: %s\n", target, strerror(errno));
            return -1;
        }
    }
    printf("[mount] %s: still mounted after 8 owned layers\n", target);
    return -1;
}

/* Default is fresh: delete, uninstall and installer preparation never use the no-work snapshot. */
static int detach_target(const char *target, const char *expected, int force) {
    return detach_target_mode(target, expected, force, 0);
}

/* Routine scan maintenance may use the bounded no-work snapshot; mutations inside it are
 * still confirmed fresh before any unmount. */
static int detach_target_routine(const char *target, const char *expected, int force) {
    return detach_target_mode(target, expected, force, 1);
}

static int mount_dir(const char *dir, const char *target) {
    if (masked_path(target))
        return 0;
    /* /user/patch and /user/addcont only exist once the console installed an update or
     * an add-on the normal way: create the parent like the system does (0770) */
    char parent[64];
    snprintf(parent, sizeof(parent), "%s", target);
    char *slash = strrchr(parent, '/');
    if (slash && slash != parent) {
        *slash = 0;
        if (mkdir(parent, 0770) == 0)
            printf("[mount] created %s\n", parent);
    }
    mkdir(target, 0777);
    /* Common steady state: the live top is already the exact hold folder. Lower layers do
     * not affect what the system uses, so the full mount-table scan is not needed. */
    if (top_layer(target, dir) == 1)
        return 0;
    int can_mount = 0;
    for (int i = 0; i < 8 && !can_mount; i++) {
        mount_obs_t o;
        mount_scan(target, dir, &o);
        mount_prepare_t step = mount_prepare_step(o.table_known, o.layers, o.top, o.top_exact, o.top_owned, o.all_owned);
        if (step == MOUNT_PREPARE_ALREADY)
            return 0;
        if (step == MOUNT_PREPARE_MOUNT || step == MOUNT_PREPARE_UNMOUNT) {
            mount_scan_fresh(target, dir, &o);
            step = mount_prepare_step(o.table_known, o.layers, o.top, o.top_exact, o.top_owned, o.all_owned);
            if (step == MOUNT_PREPARE_ALREADY)
                return 0;
        }
        if (step == MOUNT_PREPARE_MOUNT) {
            can_mount = 1;
        } else if (step == MOUNT_PREPARE_UNMOUNT) {
            int flags = o.top ? 0 : MNT_FORCE;
            if (unmount_tracked(target, flags)) {
                printf("[mount] unmount %s: %s\n", target, strerror(errno));
                return -1;
            }
        } else {
            return mount_refused(target, &o, step == MOUNT_PREPARE_UNKNOWN);
        }
    }
    if (!can_mount) {
        printf("[mount] %s: still occupied after 8 owned layers\n", target);
        return -1;
    }
    if (nullfs_mount(dir, target))
        return -1;
    mount_obs_t done;
    mount_scan(target, dir, &done);
    if (done.top_exact)
        return 0;
    printf("[mount] %s: mount postcondition failed\n", target);
    return -1;
}

/* Removes every layer of the title PS4Mount can prove it owns. Foreign or unknown layers
 * stay in place and are a failed postcondition for destructive operations. */
static void unmount_title(const char *tid, char (*dtids)[10], int ndtids) {
    char target[64], expected[PATH_MAX];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    hold_path(expected, sizeof(expected), "app", tid);
    detach_target(target, expected, 1);
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    hold_path(expected, sizeof(expected), "patch", tid);
    detach_target(target, expected, 1);
    for (int i = 0; i < ndtids; i++) {
        snprintf(target, sizeof(target), "/user/addcont/%s", dtids[i]);
        hold_path(expected, sizeof(expected), "addcont", dtids[i]);
        detach_target(target, expected, 1);
    }
}

/* Postcondition after unmount_title: no app, patch or add-on target still exposes the drive. */
static int title_detached(const char *tid, char (*dtids)[10], int ndtids) {
    char target[64];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    int app_mounted = layers_on_failclosed(target);
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    int patch_mounted = layers_on_failclosed(target);
    int addcont_mounted = 0;
    for (int i = 0; i < ndtids; i++) {
        snprintf(target, sizeof(target), "/user/addcont/%s", dtids[i]);
        addcont_mounted |= layers_on_failclosed(target);
    }
    return remove_detached_ok(app_mounted, patch_mounted, addcont_mounted);
}

/* Add-ons installed the normal way: their folder is never covered by a mount. */
static int addcont_installed(const char *dtid) {
    char target[64];
    snprintf(target, sizeof(target), "/user/addcont/%s", dtid);
    if (mounted_from(target, NULL, 0))
        return 0;
    DIR *d = opendir(target);
    if (!d)
        return 0;
    struct dirent *e;
    int used = 0;
    while (!used && (e = readdir(d)))
        used = e->d_name[0] != '.';
    closedir(d);
    return used;
}

/* /user/app/<TID> always, /user/patch/<TID> only with an update and
 * /user/addcont/<ADD-ON TID> for each add-on title. */
static int mount_paths(const char *tid, int has_patch, const char (*dtids)[10], int ndtids) {
    char target[64], hold[PATH_MAX];
    snprintf(target, sizeof(target), "/user/app/%s", tid);
    hold_path(hold, sizeof(hold), "app", tid);
    if (mount_dir(hold, target))
        return -1;
    snprintf(target, sizeof(target), "/user/patch/%s", tid);
    hold_path(hold, sizeof(hold), "patch", tid);
    if (has_patch) {
        if (mount_dir(hold, target))
            return -1;
    } else if (detach_target_routine(target, hold, 1)) {
        return -1;
    }
    for (int i = 0; g_cfg.dlc_mode == DLC_EXTERNAL && i < ndtids; i++) {
        if (addcont_installed(dtids[i])) {
            printf("[mount] /user/addcont/%s holds add-ons installed the normal way, not mounted\n", dtids[i]);
            continue;
        }
        snprintf(target, sizeof(target), "/user/addcont/%s", dtids[i]);
        hold_path(hold, sizeof(hold), "addcont", dtids[i]);
        if (mount_dir(hold, target))
            return -1;
    }
    return 0;
}

static int mount_title(title_src_t *t) {
    return mount_paths(t->app.title_id, t->has_patch, t->dlc_tids, t->ndlc_tids);
}

static int mount_info(const title_info_t *t) {
    return mount_paths(t->title_id, t->has_patch, t->dlc_tids, t->ndlc_tids);
}

static void manage_fields(const char *tid, const char *cid, const char *name, const char *dir, int has_patch,
                          int ndlc, const char (*dlc_tids)[10], int ndlc_tids) {
    managed_t *m = managed_by(tid, NULL);
    for (int i = 0; !m && i < MAX_TITLES; i++)
        if (!g_managed[i].used)
            m = &g_managed[i];
    if (!m)
        return;
    /* keep the runtime state across rescans */
    pthread_mutex_lock(&g_managed_id_mu);
    managed_t keep = *m;
    memset(m, 0, sizeof(*m));
    m->used = 1;
    if (keep.used) {
        m->running = keep.running;
        m->running_since = keep.running_since;
        m->mount_warned = keep.mount_warned;
    }
    m->has_patch = has_patch;
    m->ndlc = ndlc;
    m->ndlc_tids = ndlc_tids;
    memcpy(m->dlc_tids, dlc_tids, sizeof(m->dlc_tids));
    snprintf(m->tid, sizeof(m->tid), "%s", tid);
    snprintf(m->cid, sizeof(m->cid), "%s", cid);
    snprintf(m->name, sizeof(m->name), "%s", name);
    snprintf(m->dir, sizeof(m->dir), "%s", dir);
    pthread_mutex_unlock(&g_managed_id_mu);
}

static void manage(const title_src_t *t) {
    manage_fields(t->app.title_id, t->app.content_id, t->name, t->dir, t->has_patch, t->ndlc, t->dlc_tids,
                  t->ndlc_tids);
}

static void manage_info(const title_info_t *t) {
    manage_fields(t->title_id, t->content_id, t->name, t->dir, t->has_patch, t->ndlc, t->dlc_tids, t->ndlc_tids);
}


static int removed(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), STATE_DIR "/%s.removed", tid);
    return exists(p);
}

/* Real installs keep app.pkg on the internal drive; never touch those. */
static int installed_internally(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), "/user/app/%s", tid);
    if (mounted_from(p, NULL, 0))
        return 0;
    snprintf(p, sizeof(p), "/user/app/%s/app.pkg", tid);
    return exists(p);
}

static int marker_matches(const char *tid, const char *expected) {
    char path[64], buf[PATH_MAX + 256] = {0};
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", tid);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return n > 0 && !strcmp(buf, expected);
}

/* 1 = needs (re)registration, 0 = registered here, -1 = skip. */
/* The title is served from another connected drive: the marker names another
 * folder whose game pkg is still there. The first drive keeps it, so the same game
 * on two drives is not switched (and registered again) on every scan. */
static int served_elsewhere(const title_info_t *t) {
    char path[64], buf[PATH_MAX + 256] = {0}, pkg[PATH_MAX + 16];
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", t->title_id);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[strcspn(buf, "\n")] = 0;
    if (!strcmp(buf, t->dir))
        return 0;
    size_t l = strlen(buf);
    if (l > 4 && !strcasecmp(buf + l - 4, ".pkg")) /* a pkg used where it is */
        return exists(buf);
    snprintf(pkg, sizeof(pkg), "%s/app.pkg", buf);
    return exists(pkg);
}

/* These two stay lazy in title_state's chain, as the direct scans were before: when the
 * database already says the title is unregistered no task lookup happens, and the error is
 * read only after the exact canonical URL matched. */
static int title_bgft_names(const char *tid, const char *url) {
    const bgftscan_entry_t *scan_task = bgftscan_find(&g_bgftscan, tid);
    return g_bgftscan.usable ? bgftscan_names(scan_task, url) : bgft_names(tid, url);
}

static uint32_t title_bgft_error(const char *tid) {
    if (!g_bgftscan.usable)
        return bgft_task_error(tid);
    const bgftscan_entry_t *scan_task = bgftscan_find(&g_bgftscan, tid);
    return scan_task ? scan_task->error : 0;
}

static int title_state(const title_info_t *t) {
    const char *tid = t->title_id;
    if (installed_internally(tid) || removed(tid) || delete_pending_is(tid))
        return -1;
    if (served_elsewhere(t)) {
        if (first_time(t->app_path))
            printf("[scan] %s: %s is already served from another drive, this copy is not used\n", t->dir, tid);
        return -1;
    }
    char app_url[PATH_MAX + 8], marker[PATH_MAX + 160];
    pkg_url(tid, "app", t->app_path, app_url, sizeof(app_url));
    int have_marker = !title_marker_full(marker, sizeof(marker), t->dir, t->marker_tail);
    /* folders set up by 0.3.1 and older lack the internal folder: registered again */
    const char *why = !db_is_registered(tid)                ? "not registered"
                      : !title_bgft_names(tid, app_url)      ? "no game task naming its pkg"
                      : title_bgft_error(tid)                ? "its game task holds an error"
                      : !have_marker || !marker_matches(tid, marker) ? "its pkgs changed"
                      : !hold_ready("app", tid, "app.pkg", t->app_path) ? "game link"
                      : t->has_patch && !hold_ready("patch", tid, "patch.pkg", t->patch_path) ? "update link"
                      : !dlc_ready(t)                        ? "add-ons"
                                                             : NULL;
    if (why)
        printf("[scan] %s: to register (%s)\n", tid, why);
    return why != NULL;
}

static void pump_events(int ms);
static void pump_now(void);

/* The launch check compares /system_data/priv/appmeta/<TID>/param.sfo with the
 * update's param.sfo byte for byte; AppInstallAll writes the game's one there. */
static void enforce_patch_sfo(const char *tid, int has_patch, const char *patch_path, uint32_t sfo_len,
                              const unsigned char *sfo_sha) {
    if (!has_patch)
        return;
    char path[96];
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/param.sfo", tid);
    int fd = open(path, O_RDONLY), same = 0;
    if (fd >= 0) {
        uint8_t cur[8192], sha[32];
        ssize_t n = read(fd, cur, sizeof(cur));
        if (n > 0 && (uint32_t)n == sfo_len && sfo_len <= sizeof(cur)) {
            sha256(cur, (size_t)n, sha);
            same = !memcmp(sha, sfo_sha, sizeof(sha));
        }
        close(fd);
    }
    if (same)
        return;
    /* not the one of the update: its own param.sfo goes in its place */
    pkg_t p;
    if (pkg_open(&p, patch_path))
        return;
    size_t len;
    uint8_t *sfo = pkg_entry(&p, 0x1000, &len);
    if (sfo) {
        printf("[reg] %s: priv param.sfo was not the update's, rewritten\n", tid);
        write_file(path, sfo, len, 0777);
        mem_free(sfo);
    }
    pkg_close(&p);
}

/* Registration method: see fw.h. */
typedef int (*title_dir_fn)(const char *title_id, const char *dir, void *reserved);
typedef int (*install_all_fn)(void *param);
typedef int (*uninstall_fn)(const char *title_id);

static int g_reg_mode = REG_ALL;
static title_dir_fn g_title_dir;
static install_all_fn g_install_all;
static uninstall_fn g_uninstall_call;
static char g_fw_text[16] = "unknown";

static const char *reg_mode_name(void) {
    return g_reg_mode == REG_ALL ? "AppInstallAll" : g_reg_mode == REG_TITLEDIR ? "AppInstallTitleDir" : "none";
}

static void check_firmware(void) {
    uint32_t fw = g_fw = kernel_get_fw_version();
    unsigned major, minor;
    fw_decode(fw, &major, &minor);
    fw_text(fw, g_fw_text, sizeof(g_fw_text));
    /* looked up at runtime: linking them would stop the payload from loading where a
     * firmware does not export them (sceAppInstUtilInitialize stays linked, so the
     * module is loaded and the handle below always resolves) */
    uint32_t h = 0;
    if (!kernel_dynlib_handle(getpid(), "libSceAppInstUtil.sprx", &h)) {
        g_title_dir = (title_dir_fn)kernel_dynlib_dlsym(getpid(), h, "sceAppInstUtilAppInstallTitleDir");
        g_install_all = (install_all_fn)kernel_dynlib_dlsym(getpid(), h, "sceAppInstUtilAppInstallAll");
        g_uninstall_call = (uninstall_fn)kernel_dynlib_dlsym(getpid(), h, "sceAppInstUtilAppUnInstall");
    }
    g_reg_mode = fw_reg_mode(major, g_title_dir != NULL);
    if (g_reg_mode == REG_ALL && !g_install_all)
        g_reg_mode = g_title_dir ? REG_TITLEDIR : REG_NONE;
    printf("[main] firmware %s (0x%08x), InstallAll %s, TitleDir %s, UnInstall %s, registration: %s\n",
           g_fw_text, fw, g_install_all ? "present" : "missing", g_title_dir ? "present" : "missing",
           g_uninstall_call ? "present" : "missing", reg_mode_name());
}

/* The system registration runs in its own thread: after rest mode InstallAll was seen to
 * block for good, which froze the whole daemon (panel, klog, stop request). A call still
 * running is never doubled; titles keep being checked in the database. */
static int g_install_busy, g_install_rc;
static char g_install_tids[MAX_TITLES][10];
static int g_install_n;

static int register_call(const char *tid) {
    if (g_reg_mode == REG_TITLEDIR) {
        int rc = g_title_dir(tid, "/user/app/", NULL);
        printf("[reg] %s: AppInstallTitleDir=0x%08x\n", tid, rc);
        return rc == (int)0x80990002 ? 0 : rc; /* 0x80990002: already known, restored */
    }
    if (!g_install_all)
        return -1;
    return g_install_all(NULL);
}

static void *install_thread(void *arg) {
    (void)arg;
    int rc = 0;
    if (g_reg_mode == REG_TITLEDIR) {
        for (int i = 0; i < g_install_n; i++) {
            int r = register_call(g_install_tids[i]);
            if (r)
                rc = r;
        }
    } else {
        rc = register_call(NULL);
    }
    __atomic_store_n(&g_install_rc, rc, __ATOMIC_RELAXED);
    __atomic_store_n(&g_install_busy, 0, __ATOMIC_RELEASE); /* result before completion */
    return NULL;
}

/* Registers the titles of tids that are not registered yet. */
static int install_all_start(char (*tids)[10], int n) {
    if (g_reg_mode == REG_NONE)
        return -1; /* never where it corrupts the database */
    if (__atomic_load_n(&g_install_busy, __ATOMIC_ACQUIRE)) {
        printf("[reg] registration still running from an earlier call\n");
        return -1;
    }
    g_install_n = 0;
    for (int i = 0; i < n && g_install_n < MAX_TITLES; i++)
        if (!db_is_registered(tids[i]))
            snprintf(g_install_tids[g_install_n++], 10, "%s", tids[i]);
    __atomic_store_n(&g_install_busy, 1, __ATOMIC_RELEASE);
    pthread_t th;
    if (pthread_create(&th, NULL, install_thread, NULL)) {
        install_thread(NULL); /* no thread: call it here */
        return 0;
    }
    pthread_detach(th);
    return 0;
}

/* Registration runs in three steps so a group of games costs the system one
 * AppInstallAll and no write to its databases while it is still busy:
 *   1. register_prepare: files and mounts of each game of the group;
 *   2. one sceAppInstUtilAppInstallAll, then wait until ShellCore has the rows;
 *   3. register_finish: checks the title is registered, writes the marker. */
static int register_prepare(title_src_t *t) {
    if (place_files(t) || mount_title(t))
        return -1;
    manage(t);
    return 0;
}

/* Max mtime of the system databases (and their journals): ShellCore is busy while it changes. */
static time_t db_mtime(void) {
    static const char *files[] = {"/system_data/priv/mms/app.db", "/system_data/priv/mms/app.db-journal",
                                  "/system_data/priv/mms/appinfo.db", "/system_data/priv/mms/appinfo.db-journal",
                                  "/system_data/priv/mms/addcont.db", NULL};
    time_t t = 0;
    struct stat st;
    for (int i = 0; files[i]; i++)
        if (!stat(files[i], &st) && st.st_mtime > t)
            t = st.st_mtime;
    return t;
}

/* Waits until the system databases were not written for quiet seconds (max limit).
 * While it waits the activity shows "waiting_db"; the caller's phase comes back after. */
static void wait_db_quiet(int quiet, int limit) {
    char prev[24];
    pthread_mutex_lock(&g_act_mu);
    snprintf(prev, sizeof(prev), "%s", g_activity.phase);
    snprintf(g_activity.phase, sizeof(g_activity.phase), "%s", "waiting_db");
    pthread_mutex_unlock(&g_act_mu);
    time_t start = time(NULL), last = db_mtime(), changed = time(NULL);
    int said = 0;
    while (time(NULL) - changed < quiet && time(NULL) - start < limit) {
        if (!said)
            printf("[reg] the system is writing its databases: waiting for them (up to %d s)\n", limit);
        said = 1;
        pump_events(1000);
        time_t m = db_mtime();
        if (m != last) {
            last = m;
            changed = time(NULL);
        }
    }
    pthread_mutex_lock(&g_act_mu);
    snprintf(g_activity.phase, sizeof(g_activity.phase), "%s", prev);
    pthread_mutex_unlock(&g_act_mu);
}

/* ShellCore registers every new title under /user/app itself; this also refreshes the
 * home screen and fetches the PSN concept. Waits while titles keep arriving. */
/* AppInstallAll walks every folder of /system_ex/app, /system_ex/rnps/apps and /user/app
 * (about 0.7 s per PS4 game, registered or not). While it runs, /user/app shows only the
 * games being added: an empty staging folder is mounted over /user/app and each new
 * game's folder on top of it. The other games stay registered (their rows are not
 * touched) and their own mounts stay in place underneath. */
#define STAGE_DIR HOLD_DIR "/stage"

static int open_games(uint32_t *ids, char (*tids)[10], int max);

static void stage_clear(void) {
    DIR *d = opendir(STAGE_DIR);
    if (!d)
        return;
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(p, sizeof(p), STAGE_DIR "/%s", e->d_name);
            rmdir(p);
        }
    closedir(d);
}

static int user_app_masked(void) {
    struct statfs sf;
    return !statfs("/user/app", &sf) && !strcmp(sf.f_mntonname, "/user/app") && !strcmp(sf.f_mntfromname, STAGE_DIR);
}

static void unmask_user_app(char (*tids)[10], int n) {
    char target[64];
    for (int i = 0; i < n; i++) {
        snprintf(target, sizeof(target), "/user/app/%s", tids[i]);
        if (user_app_masked() && mounted_from(target, NULL, 0) && unmount_tracked(target, 0) && unmount_tracked(target, MNT_FORCE))
            printf("[reg] unmount %s: %s\n", target, strerror(errno));
    }
    /* the system keeps the folder open for a moment after AppInstallAll returned */
    for (int i = 0; i < 12 && user_app_masked(); i++) {
        if (!unmount_tracked("/user/app", 0))
            break;
        pump_events(250);
    }
    if (user_app_masked() && unmount_tracked("/user/app", MNT_FORCE))
        printf("[reg] unmount /user/app: %s\n", strerror(errno));
    if (user_app_masked())
        printf("[reg] /user/app still shows only the new games\n");
    g_masked = 0;
    stage_clear();
}

/* A daemon stopped while /user/app showed only the new games: show all of them again. */
static void unmask_leftover(void) {
    static char tids[MAX_TITLES][10];
    int n = 0;
    DIR *d = opendir(STAGE_DIR);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < MAX_TITLES)
        if (strlen(e->d_name) == 9)
            snprintf(tids[n++], 10, "%s", e->d_name);
    if (d)
        closedir(d);
    printf("[reg] /user/app showed only %d game(s) from an earlier run, restoring it\n", n);
    g_masked = 1;
    unmask_user_app(tids, n);
}

/* Returns 1 when /user/app now shows only the games of tids that are not registered. */
static int mask_user_app(char (*tids)[10], int n) {
    uint32_t ids[4];
    char open[4][10];
    if (g_reg_mode != REG_ALL || open_games(ids, open, 4))
        return 0; /* an open game keeps its folder in use: plain AppInstallAll */
    if (user_app_masked())
        unmask_leftover();
    stage_clear();
    mkdirs(STAGE_DIR, 0777);
    char new_tids[MAX_TITLES][10];
    int nn = 0;
    for (int i = 0; i < n && nn < MAX_TITLES; i++)
        if (!db_is_registered(tids[i])) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), STAGE_DIR "/%s", tids[i]);
            if (mkdir(p, 0777) && errno != EEXIST)
                return 0;
            snprintf(new_tids[nn++], 10, "%s", tids[i]);
        }
    if (!nn || nullfs_mount(STAGE_DIR, "/user/app"))
        return 0;
    for (int i = 0; i < nn; i++) {
        char hold[PATH_MAX], target[64];
        hold_path(hold, sizeof(hold), "app", new_tids[i]);
        snprintf(target, sizeof(target), "/user/app/%s", new_tids[i]);
        if (nullfs_mount(hold, target)) {
            unmask_user_app(new_tids, i);
            return 0;
        }
    }
    g_masked = 1;
    printf("[reg] /user/app shows only the %d new game(s) during AppInstallAll\n", nn);
    return 1;
}

static void install_all(char (*tids)[10], int n) {
    int need = 0;
    for (int i = 0; i < n; i++)
        need += !db_is_registered(tids[i]);
    if (!need)
        return;
    int masked = mask_user_app(tids, n);
    if (install_all_start(tids, n) && masked) {
        unmask_user_app(tids, n);
        masked = 0;
    }
    int done = n - need, idle = 0;
    while (done < n && idle < 30) {
        pump_events(1000);
        int now = 0;
        for (int i = 0; i < n; i++)
            now += db_is_registered(tids[i]);
        idle = now > done ? 0 : idle + 1;
        done = now;
        if (masked && !__atomic_load_n(&g_install_busy, __ATOMIC_ACQUIRE)) {
            unmask_user_app(tids, n);
            masked = 0;
        }
    }
    /* the rows can be there before AppInstallAll returns: the other games stay hidden
     * until it does (at most two minutes) */
    for (int i = 0; masked && __atomic_load_n(&g_install_busy, __ATOMIC_ACQUIRE) && i < 120; i++)
        pump_events(1000);
    if (masked)
        unmask_user_app(tids, n);
    if (__atomic_load_n(&g_install_busy, __ATOMIC_ACQUIRE)) {
        printf("[reg] %s still running: %d of %d title(s) registered so far\n", reg_mode_name(), done, n);
    } else {
        printf("[reg] %s=0x%08x: %d of %d title(s) registered\n", reg_mode_name(),
               __atomic_load_n(&g_install_rc, __ATOMIC_ACQUIRE), done, n);
    }
}

/* The system databases are only ever written by the system: ShellCore keeps them
 * open with its own cache, and rows written from outside make it flag app.db as
 * corrupted (and rebuild it at the next boot). A title the system did not register
 * is tried again later. */
static int register_finish(title_src_t *t) {
    const char *tid = t->app.title_id;
    if (!db_is_registered(tid)) {
        printf("[reg] %s: not registered by the system\n", tid);
        return -1;
    }
    enforce_patch_sfo(tid, t->has_patch, t->patch_path, t->patch_sfo_len, t->patch_sfo_sha);
    install_title_dlc(t); /* now the system has the game (already installed ones are skipped) */
    char marker[64];
    snprintf(marker, sizeof(marker), STATE_DIR "/%s.src", tid);
    if (write_file(marker, t->marker, strlen(t->marker), 0666))
        return -1;
    manage(t);
    return 0;
}

/* Logs a skipped root file once per daemon run (any number of files). */
static int first_time(const char *path) {
    static char **seen;
    static size_t n, cap;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    int first = 0;
    pthread_mutex_lock(&mu);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(seen[i], path)) {
            pthread_mutex_unlock(&mu);
            return 0;
        }
    if (n == cap) {
        size_t ncap = cap ? cap * 2 : 64;
        char **grown = realloc(seen, ncap * sizeof(*seen));
        if (grown) {
            seen = grown;
            cap = ncap;
        }
    }
    if (n < cap) {
        seen[n] = strdup(path);
        if (seen[n])
            n++;
    }
    first = 1;
    pthread_mutex_unlock(&mu);
    return first;
}


/* 1 when sfo lists the add-on prefix (UP0000-CUSA00000_00) as one of its add-on services. */
static int sfo_serves(const sfo_t *sfo, const char *prefix) {
    char key[32];
    for (int i = 1; i <= 7; i++) {
        snprintf(key, sizeof(key), "SERVICE_ID_ADDCONT_ADD_%d", i);
        const sfo_item_t *it = sfo_get(sfo, key);
        if (it && !it->is_int && !strncmp(it->sval, prefix, 19))
            return 1;
    }
    return 0;
}

/* The game on this drive an add-on belongs to: same title ID, or a game (or its
 * update) that names the add-on's content ID prefix in SERVICE_ID_ADDCONT_ADD_n
 * (regional versions share add-ons this way). */
static int find_dlc_game(const char *root, const pkg_t *dlc, char *tid) {
    char lib[PATH_MAX], p[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, dlc->title_id);
    if (exists(p)) {
        snprintf(tid, 10, "%s", dlc->title_id);
        return 0;
    }
    DIR *d = opendir(lib);
    if (!d)
        return -1;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (!is_game_id(e->d_name))
            continue;
        static const char *names[] = {"app.pkg", "patch.pkg"};
        for (int i = 0; i < 2 && !found; i++) {
            pkg_t pkg;
            sfo_t sfo;
            snprintf(p, sizeof(p), "%s/%s/%s", lib, e->d_name, names[i]);
            if (!exists(p) || pkg_open(&pkg, p))
                continue;
            found = !read_sfo(&pkg, &sfo) && sfo_serves(&sfo, dlc->content_id);
            pkg_close(&pkg);
        }
        if (found)
            snprintf(tid, 10, "%s", e->d_name);
    }
    closedir(d);
    return found ? 0 : -1;
}

/* Add-ons left in place during one adoption run, reported in one notification. */
static int g_dlc_orphans, g_dlc_dupes;
static uint32_t g_orphans_sig; /* the add-ons waiting in this scan of a drive */

static uint32_t str_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/* Add-ons waiting for their game: checked again only when the games of the drive
 * change (the check opens the pkg of every game). */
typedef struct {
    char *path;
    unsigned long sig;
} waiting_t;

static waiting_t *g_waiting;
static size_t g_nwaiting, g_capwaiting;
static unsigned long g_lib_sig; /* games of the drive being scanned */

/* Signature of the games in <root>/ps4: changes when a game is added or removed. */
static unsigned long lib_signature(const char *root) {
    char lib[PATH_MAX], p[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    DIR *d = opendir(lib);
    if (!d)
        return 0;
    struct dirent *e;
    unsigned long sig = 5381;
    while ((e = readdir(d))) {
        if (!is_game_id(e->d_name))
            continue;
        snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, e->d_name);
        scan_note_usb_stat(p);
        if (!exists(p))
            continue;
        unsigned long h = 0;
        for (const char *c = e->d_name; *c; c++)
            h = h * 33 + (unsigned char)*c;
        sig += h; /* order independent */
    }
    closedir(d);
    return sig;
}

static waiting_t *waiting_find(const char *path) {
    for (size_t i = 0; i < g_nwaiting; i++)
        if (!strcmp(g_waiting[i].path, path))
            return &g_waiting[i];
    return NULL;
}

static void waiting_set(const char *path, unsigned long sig) {
    waiting_t *w = waiting_find(path);
    if (w) {
        w->sig = sig;
        return;
    }
    if (g_nwaiting == g_capwaiting) {
        size_t cap = g_capwaiting ? g_capwaiting * 2 : 64;
        waiting_t *grown = realloc(g_waiting, cap * sizeof(*grown));
        if (!grown)
            return;
        g_waiting = grown;
        g_capwaiting = cap;
    }
    if ((g_waiting[g_nwaiting].path = strdup(path)))
        g_waiting[g_nwaiting++].sig = sig;
}

static void waiting_drop(const char *path) {
    waiting_t *w = waiting_find(path);
    if (!w)
        return;
    free(w->path);
    *w = g_waiting[--g_nwaiting];
}

/* Add-ons of root still waiting for their game. */
static int waiting_count(const char *root) {
    size_t l = strlen(root);
    int n = 0;
    for (size_t i = 0; i < g_nwaiting; i++)
        n += !strncmp(g_waiting[i].path, root, l) && g_waiting[i].path[l] == '/' && exists(g_waiting[i].path);
    return n;
}

/* Path of a file relative to the drive root it is on. */
static const char *drive_rel(const char *root, const char *path) {
    size_t n = strlen(root);
    return !strncmp(path, root, n) && path[n] == '/' ? path + n + 1 : path;
}

/* Records where a pkg moved into <root>/ps4/<TID>/<rel> came from. */
static void record_origin(const char *root, const char *tid, const char *rel, const char *from) {
    char game[PATH_MAX];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    if (origin_add(game, rel, drive_rel(root, from)))
        printf("[scan] %s: could not record where %s came from\n", game, rel);
}

/* Moves an add-on pkg (ac) into <root>/ps4/<GAME>/addcont/<ADD-ON TID>/<LABEL>/ac.pkg. */
static int adopt_dlc(const char *root, const char *name, const char *path, pkg_t *pkg) {
    char game[10], dst[PATH_MAX];
    if (find_dlc_game(root, pkg, game)) {
        if (first_time(path)) {
            /* a game served from another drive is mounted there too: only a real install counts */
            printf("[scan] %s: no game for add-on %s on the drive%s, waiting\n", name, pkg->content_id,
                   installed_internally(pkg->title_id) ? " (that title is installed on the console)" : "");
            g_dlc_orphans++;
        }
        g_orphans_sig += str_hash(path);
        waiting_set(path, g_lib_sig);
        return 0;
    }
    waiting_drop(path);
    snprintf(dst, sizeof(dst), "%s/ps4/%s/addcont/%s/%s", root, game, pkg->title_id, pkg->content_id + 20);
    mkdirs(dst, 0777);
    snprintf(dst, sizeof(dst), "%s/ps4/%s/addcont/%s/%s/ac.pkg", root, game, pkg->title_id, pkg->content_id + 20);
    if (exists(dst)) {
        if (first_time(path)) {
            printf("[scan] %s: %s already has this add-on, the copy was left in place\n", name, game);
            g_dlc_dupes++;
        }
        return 0;
    }
    if (rename(path, dst)) {
        printf("[scan] move %s: %s\n", path, strerror(errno));
        return 0;
    }
    printf("[scan] moved %s -> %s\n", path, dst);
    char rel[64];
    snprintf(rel, sizeof(rel), "addcont/%s/%s/ac.pkg", pkg->title_id, pkg->content_id + 20);
    record_origin(root, game, rel, path);
    return 1;
}

/* One pass over the pkgs in dir: pass 0 moves PS4 game (gd) and update (gp) pkgs into
 * <root>/ps4/<TID>/app.pkg and patch.pkg, pass 1 the add-ons (ac) next to their game. */
static int adopt_dir_pass(const char *root, const char *dir, int pass) {
    int moved = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    char path[PATH_MAX], dst[PATH_MAX];
    while ((e = readdir(d))) {
        if (scan_is_appledouble_pkg(e->d_name)) {
            if (g_scan_m.active)
                g_scan_m.adopt_appledouble++;
            continue;
        }
        if (!scan_pkg_candidate(e->d_name))
            continue;
        if (g_scan_m.active)
            g_scan_m.adopt_cand++;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat rs;
        scan_note_usb_stat(path);
        if (!stat(path, &rs) && restored_has(RESTORED_LIST, drive_rel(root, path), (uint64_t)rs.st_size)) {
            if (first_time(path))
                printf("[scan] skip %s (moved back from ps4/ on request)\n", path);
            continue;
        }
        waiting_t *w = waiting_find(path);
        if (w && w->sig == g_lib_sig)
            continue; /* waiting add-on, and no new game since the last check */
        pkg_t pkg;
        if (pkg_open(&pkg, path)) {
            scan_note_usb_pkg(path, &pkg);
            if (pass == 0 && first_time(path))
                printf("[scan] skip %s (cannot read the pkg header: not a PS4 pkg, or unreadable)\n", path);
            continue;
        }
        scan_note_usb_stat(path);
        if (!stat(path, &rs) && (uint64_t)rs.st_size < pkg.pkg_size) {
            /* still being copied (an internal folder lets the copy be moved midway) */
            scan_note_usb_pkg(path, &pkg);
            pkg_close(&pkg);
            continue;
        }
        sfo_t sfo;
        const sfo_item_t *cat = NULL;
        if (!read_sfo(&pkg, &sfo))
            cat = sfo_get(&sfo, "CATEGORY");
        const sfo_item_t *sysver = cat ? sfo_get(&sfo, "SYSTEM_VER") : NULL;
        if (pass == 0 && sysver && sysver->is_int && g_fw && !fw_runs(sysver->ival, g_fw) && first_time(path)) {
            /* added anyway: a backport makes it run */
            char need[16], have[16];
            ver_text(sysver->ival, need, sizeof(need));
            ver_text(g_fw, have, sizeof(have));
            notify_kind(NOTIFY_WARN, TR("%s is built for firmware %s (this console has %s): it only runs with a backport",
                      "%s está hecho para el firmware %s (esta consola tiene %s): solo funciona con un backport"),
                   e->d_name, need, have);
        }
        int is_app = cat && pkg_cat_is_game(cat->sval), is_patch = cat && !strcmp(cat->sval, "gp");
        int is_dlc = cat && !strcmp(cat->sval, "ac");
        if (is_dlc || pass == 1) {
            /* add-ons go in the second pass: their game may be in this folder too */
            if (pass == 1 && is_dlc && !is_game_id(pkg.title_id)) {
                if (first_time(path))
                    printf("[scan] skip %s (add-on %s is not for a PS4 title)\n", path, pkg.content_id);
            } else if (pass == 1 && is_dlc) {
                if (!g_cfg.dlc) {
                    if (first_time(path))
                        printf("[scan] skip %s (add-ons are turned off)\n", path);
                } else if (pkg_has_entry_key(&pkg))
                    moved += adopt_dlc(root, e->d_name, path, &pkg);
                else if (first_time(path))
                    printf("[scan] skip %s (unsupported add-on package)\n", path);
            }
            scan_note_usb_pkg(path, &pkg);
            pkg_close(&pkg);
            continue;
        }
        int ok = (is_app || is_patch) && is_game_id(pkg.title_id) && pkg_has_entry_key(&pkg) &&
                 !(is_app && homebrew_small(pkg.title_id, pkg.pkg_size));
        char tid[10], ver[16] = "";
        uint8_t digest[32];
        snprintf(tid, sizeof(tid), "%s", pkg.title_id);
        memcpy(digest, pkg.digest, sizeof(digest));
        const sfo_item_t *av = ok ? sfo_get(&sfo, "APP_VER") : NULL;
        if (av && !av->is_int)
            snprintf(ver, sizeof(ver), "%s", av->sval);
        scan_note_usb_pkg(path, &pkg);
        pkg_close(&pkg);
        if (!ok) {
            if (first_time(path))
                printf("[scan] skip %s (unsupported PS4 application or update package)\n", path);
            continue;
        }
        if (installed_internally(tid)) {
            if (first_time(path))
                printf("[scan] skip %s (%s is installed internally)\n", path, tid);
            continue;
        }
        snprintf(dst, sizeof(dst), "%s/ps4/%s", root, tid);
        mkdirs(dst, 0777);
        snprintf(dst, sizeof(dst), "%s/ps4/%s/%s", root, tid, is_app ? "app.pkg" : "patch.pkg");
        scan_note_usb_stat(dst);
        if (exists(dst)) {
            pkg_t cur;
            int same = !pkg_open(&cur, dst) && !memcmp(cur.digest, digest, sizeof(digest));
            scan_note_usb_pkg(dst, &cur);
            if (cur.fd > 0)
                pkg_close(&cur);
            if (same || is_app) {
                /* duplicates, and a second copy of a game, stay where they are */
                if (first_time(path))
                    notify_kind(NOTIFY_WARN,
                                same ? TR("%s is already in use for %s; the copy was left where it is",
                                     "%s ya se usa para %s; la copia se deja donde está")
                                : TR("%s: %s already has a game package; the new one was left where it is",
                                     "%s: %s ya tiene un pkg de juego; el nuevo se deja donde está"),
                           e->d_name, tid);
                continue;
            }
            /* a different update: it replaces the current one, which is kept aside */
            char old[PATH_MAX];
            snprintf(old, sizeof(old), "%s/ps4/%s/replaced", root, tid);
            mkdirs(old, 0777);
            snprintf(old, sizeof(old), "%s/ps4/%s/replaced/patch-%ld.pkg", root, tid, (long)time(NULL));
            if (rename(dst, old)) {
                printf("[scan] keep %s aside: %s\n", dst, strerror(errno));
                continue;
            }
            /* the update kept aside keeps its original name */
            char game[PATH_MAX], was[PATH_MAX];
            snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
            if (!origin_find(game, "patch.pkg", was, sizeof(was)))
                origin_add(game, old + strlen(game) + 1, was);
            notify_kind(NOTIFY_READY, TR("New update for %s (v%s): the previous one was moved to ps4/%s/replaced",
                      "Nueva actualización de %s (v%s): la anterior se movió a ps4/%s/replaced"),
                   tid, ver, tid);
        }
        if (rename(path, dst)) {
            printf("[scan] move %s: %s\n", path, strerror(errno));
        } else {
            printf("[scan] moved %s -> %s\n", path, dst);
            record_origin(root, tid, is_app ? "app.pkg" : "patch.pkg", path);
            moved++;
        }
    }
    closedir(d);
    return moved;
}

#define MAX_SCAN_DIRS 256

/* Folders of a scan folder that are never looked into: the library itself (ps4, the same
 * folder as the console's PS4 on exFAT), hidden ones and Windows' own. */
static int skip_subdir(const char *name) {
    return name[0] == '.' || !strcasecmp(name, "ps4") || !strcasecmp(name, "$RECYCLE.BIN") ||
           !strcasecmp(name, "System Volume Information");
}

/* Adds the subfolders of dir to dirs (n so far, max MAX_SCAN_DIRS). */
static void add_subdirs(const char *dir, char (*dirs)[PATH_MAX], int *n) {
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && *n < MAX_SCAN_DIRS) {
        char p[PATH_MAX];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (!skip_subdir(e->d_name) && !emu_is_dir(p) && !lstat(p, &st) && S_ISDIR(st.st_mode))
            snprintf(dirs[(*n)++], PATH_MAX, "%s", p);
    }
    if (d)
        closedir(d);
}

/* The pkgs of a scan folder and of its subfolders two levels down (Game/, Update/ and
 * DLC/ in a folder per game): games and updates of all of them first, so an add-on in
 * a sibling folder finds its game. Subfolders left empty are removed, the scan folder
 * never. */
static int adopt_dir_pkgs(const char *root, const char *dir) {
    char (*dirs)[PATH_MAX] = mem_alloc(sizeof(*dirs) * MAX_SCAN_DIRS);
    if (!dirs)
        return 0;
    int n = 0, moved = 0;
    snprintf(dirs[n++], PATH_MAX, "%s", dir);
    add_subdirs(dir, dirs, &n);
    for (int i = 1, first_level = n; i < first_level; i++)
        add_subdirs(dirs[i], dirs, &n);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1)
            g_lib_sig = lib_signature(root); /* games moved in pass 0 count */
        for (int i = 0; i < n; i++)
            moved += adopt_dir_pass(root, dirs[i], pass);
    }
    for (int i = n - 1; moved && i > 0; i--)
        rmdir(dirs[i]); /* only an empty folder goes */
    mem_free(dirs);
    return moved;
}

typedef struct {
    char tid[10], name[128];
    int state, ndlc, has_patch;
} title_t;

/* A ps4/ folder or a .pkg in the root. Other partitions (like the boot partition
 * Ventoy adds to a drive) are skipped without a notification. */
static void scan_dir(char *out, size_t n, const char *root, int i) {
    if (is_internal(root))
        /* PS4Mount's own folder: /data/pkg holds the pkgs people install the usual way */
        snprintf(out, n, "%s", i ? "" : INTERNAL_ROOT "/" DEFAULT_SCAN_PATH); /* never /data itself */
    else {
        char configured[128];
        cfg_scan_path(i, configured, sizeof(configured));
        if (configured[0])
            snprintf(out, n, "%s/%s", root, configured);
        else
            snprintf(out, n, "%s", root);
    }
}

static int has_pkg(const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        found = scan_pkg_candidate(e->d_name);
    }
    closedir(d);
    return found;
}

/* The same folders adopt_dir_pkgs reads (the folder and its subfolders two levels
 * down): a drive whose pkgs are only in subfolders must still count as a candidate,
 * or it is skipped on every scan. */
static int has_pkg_deep(const char *dir) {
    if (has_pkg(dir))
        return 1; /* the common case: no memory is allocated */
    char (*dirs)[PATH_MAX] = mem_alloc(sizeof(*dirs) * MAX_SCAN_DIRS);
    if (!dirs)
        return has_pkg(dir);
    int n = 0, found = 0;
    snprintf(dirs[n++], PATH_MAX, "%s", dir);
    add_subdirs(dir, dirs, &n);
    for (int i = 1, first_level = n; i < first_level; i++)
        add_subdirs(dirs[i], dirs, &n);
    for (int i = 0; i < n && !found; i++)
        found = has_pkg(dirs[i]);
    mem_free(dirs);
    return found;
}

/* Titles that failed to be added wait before the next try, and the failure is
 * notified once: when the system refuses registrations, every scan would retry. */
#define RETRY_SECONDS 300

typedef struct {
    char tid[10];
    time_t until;
} retry_t;

static retry_t g_retry[MAX_TITLES];

static retry_t *retry_entry(const char *tid, int create) {
    retry_t *free_slot = NULL;
    for (int i = 0; i < MAX_TITLES; i++) {
        if (!strcmp(g_retry[i].tid, tid))
            return &g_retry[i];
        if (!free_slot && !g_retry[i].tid[0])
            free_slot = &g_retry[i];
    }
    if (create && free_slot)
        snprintf(free_slot->tid, sizeof(free_slot->tid), "%s", tid);
    return create ? free_slot : NULL;
}

static int retry_waiting(const char *tid) {
    retry_t *r = retry_entry(tid, 0);
    return r && time(NULL) < r->until;
}

/* Returns 1 the first time a title fails (to notify it), 0 for repeated failures. */
static int retry_failed(const char *tid) {
    retry_t *r = retry_entry(tid, 1);
    if (!r)
        return 1;
    int first = !r->until;
    r->until = time(NULL) + RETRY_SECONDS;
    printf("[reg] %s: next try in %d seconds\n", tid, RETRY_SECONDS);
    return first;
}

static void retry_clear(const char *tid) {
    retry_t *r = retry_entry(tid, 0);
    if (r)
        memset(r, 0, sizeof(*r));
}

static int has_candidates(const char *root) {
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/ps4", root);
    if (exists(p))
        return 1;
    for (int i = 0; i < g_cfg.npaths; i++) {
        scan_dir(p, sizeof(p), root, i);
        if (has_pkg_deep(p))
            return 1;
    }
    return 0;
}

static int adopt_root_pkgs(const char *root) {
    char dir[PATH_MAX];
    int moved = 0;
    g_dlc_orphans = g_dlc_dupes = 0;
    g_orphans_sig = 0;
    g_lib_sig = lib_signature(root);
    for (int i = 0; i < g_cfg.npaths; i++) {
        scan_dir(dir, sizeof(dir), root, i);
        if (dir[0] && strcmp(dir, root))
            mkdir(dir, 0777); /* there to be seen: where the pkgs go */
        moved += adopt_dir_pkgs(root, dir);
    }
    /* said again only when the waiting add-ons change, not each time the payload starts:
     * add-ons of a game that is never on the drive (themes, for one) would repeat it */
    char seen[64], sig[16];
    snprintf(seen, sizeof(seen), STATE_DIR "/waiting.%08x", str_hash(root));
    snprintf(sig, sizeof(sig), "%08x", g_orphans_sig);
    size_t sl = 0;
    uint8_t *old = read_all(seen, &sl);
    int same = old && sl == strlen(sig) && !memcmp(old, sig, sl);
    mem_free(old);
    if (g_dlc_orphans && !same)
        notify_kind(NOTIFY_WARN, TR("%s: %d add-on(s) wait for their game, which is not on this drive (see the log)",
                  "%s: %d DLC esperan su juego, que no está en esta unidad (ver el log)"),
               root_label(root),
               waiting_count(root));
    if (g_dlc_orphans && !same)
        write_file(seen, sig, strlen(sig), 0666);
    if (g_dlc_dupes)
        notify(TR("%s: %d add-on copy(ies) were left in place because the game already has them",
                  "%s: %d copia(s) de DLC se dejan donde están porque el juego ya las tiene"),
               root_label(root),
               g_dlc_dupes);
    return moved;
}

static int titles_on_root_dirs(const char *root, char (*tids)[10], int max, fp_counts_t *fc) {
    int n = 0;
    char lib[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    DIR *d = opendir(lib);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        if (!is_game_id(e->d_name))
            continue;
        char pkg[PATH_MAX];
        snprintf(pkg, sizeof(pkg), "%s/%s/app.pkg", lib, e->d_name);
        if (fc && is_usb_storage_path(pkg))
            fc->usb_stat++;
        if (exists(pkg))
            snprintf(tids[n++], 10, "%s", e->d_name);
    }
    if (d)
        closedir(d);
    return n;
}

/* The title IDs PS4Mount serves from root: its ps4/<TID> folders when pkgs are moved
 * there, or the games the catalog of the drive found anywhere. This live form is owner state. */
static int titles_on_root(const char *root, char (*tids)[10], int max) {
    if (in_place()) {
        int n = 0;
        const catalog_t *c = drive_catalog(root);
        for (int i = 0; c && i < c->ngames && n < max; i++)
            snprintf(tids[n++], 10, "%s", c->games[i].title_id);
        return n;
    }
    return titles_on_root_dirs(root, tids, max, g_scan_m.active ? &g_scan_m.fp : NULL);
}

/* Worker form of the same enumeration: in-place games come from one immutable epoch held
 * by the caller; organized folders are already plain filesystem state. */
static int titles_on_root_snapshot(const char *root, char (*tids)[10], int max, const catsnap_t *snap,
                                   const config_t *cfg, fp_counts_t *fc) {
    if (in_place_cfg(cfg)) {
        int n = 0;
        uint32_t ng = catsnap_ngames(snap);
        const catsnap_game_t *games = catsnap_games((catsnap_t *)snap);
        for (uint32_t i = 0; i < ng && n < max; i++)
            snprintf(tids[n++], 10, "%s", games[i].title_id);
        return n;
    }
    return titles_on_root_dirs(root, tids, max, fc);
}

/* Grouped paths for artwork come from one immutable epoch reference acquired and released
 * inside the lookup; organized folders remain plain filesystem state. */
static int title_pkgs_snapshot(const char *root, const char *tid, const config_t *cfg, char *patch, size_t pn, char *app,
                               size_t an) {
    patch[0] = app[0] = 0;
    if (in_place_cfg(cfg)) {
        catsnap_t *snap = catalog_snapshot_acquire(root);
        const catsnap_game_t *g = catsnap_find(snap, tid);
        if (!g) {
            catsnap_unref(snap);
            return -1;
        }
        snprintf(app, an, "%s", catsnap_path(snap, g->app_off));
        if (g->patch_off != CATSNAP_NO_PATH)
            snprintf(patch, pn, "%s", catsnap_path(snap, g->patch_off));
        catsnap_unref(snap);
        return 0;
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/ps4/%s", root, tid);
    snprintf(app, an, "%s/app.pkg", dir);
    snprintf(patch, pn, "%s/patch.pkg", dir);
    return exists(app) ? 0 : -1;
}

/* Pkgs moved back (RESTORED_LIST) are left out of the catalog of the drive being walked. */
static const char *g_walk_root;
static int g_walk_restored;

static int walk_skip(const char *path, uint64_t size) {
    return g_walk_restored && restored_has(RESTORED_LIST, drive_rel(g_walk_root, path), size);
}

/* The console's own storage holds far more than games: only the two folders PS4Mount puts
 * pkgs in are searched there, so a scan does not walk every save and screenshot. */
static int internal_dir_skip(const char *path, const char *rel) {
    (void)path;
    size_t n = strlen(DEFAULT_SCAN_PATH);
    if (!strncmp(rel, DEFAULT_SCAN_PATH, n) && (!rel[n] || rel[n] == '/'))
        return 0;
    return strncmp(rel, "ps4", 3) || (rel[3] && rel[3] != '/');
}

static int root_dir_skip(const char *path, const char *rel) {
    (void)path;
    return is_internal(g_walk_root) && internal_dir_skip(path, rel);
}

/* Brings the catalog of the drive at root up to date: only new or changed pkgs are read,
 * also across restarts (STATE_DIR/catalog-<drive>.txt). 1 when it has games. */
static int catalog_update(catalog_t *cat, const char *root, int fresh) {
    char file[96], id[64];
    snprintf(file, sizeof(file), STATE_DIR "/catalog-%s.txt", root_id(root, id, sizeof(id)));
    int idx = (int)(cat - g_cat);
    if (!g_cat_loaded[idx]) {
        g_cat_loaded[idx] = 1;
        int n = catalog_load(cat, file);
        if (n > 0)
            printf("[scan] %s: %d pkg(s) known from the last run\n", root, n);
    }
    /* A drive with no sign of games (a music stick, the boot partition) is searched all
     * the same, but without saying so: only a ps4 folder or a pkg announces the scan. */
    int announce = fresh && g_scan_manual_active && (has_candidates(root) || has_pkg(root));
    if (announce)
        notify_kind(NOTIFY_DRIVE, TR("Scanning %s for PS4 games...", "Buscando juegos de PS4 en %s..."), root_label(root));
    activity_set("scanning", root_label(root), NULL, NULL, 0, cat->npkgs, 0, 0);
    int before = cat->npkgs;
    g_walk_root = root;
    g_walk_restored = exists(RESTORED_LIST);
    int unread = catalog_walk(cat, root, walk_skip, root_dir_skip, NULL, 0);
    if ((unread || fresh) && cat->npkgs)
        printf("[scan] %s: checking %d pkg(s), %d new or changed\n", root_label(root), cat->npkgs, unread);
    if (unread) {
        time_t t0 = time(NULL);
        int read = catalog_read(cat);
        printf("[scan] %s: %d pkg(s), %d read in %ld s\n", root_label(root), cat->npkgs, read,
               (long)(time(NULL) - t0));
    }
    if ((unread || cat->npkgs != before) && catalog_save(cat, file))
        printf("[scan] %s: could not save %s: %s\n", root, file, strerror(errno));
    activity_set("scanning", root_label(root), NULL, NULL, cat->npkgs, cat->npkgs, 0, 0);
    for (int i = 0; i < cat->npkgs; i++) {
        const cat_pkg_t *p = &cat->pkgs[i];
        if (p->state == CAT_BAD && first_time(p->path))
            printf("[scan] skip %s (unsupported PS4 application, update or add-on package)\n", p->path);
        else if (p->state == CAT_SHORT && first_time(p->path))
            printf("[scan] wait %s: incomplete, %llu of %llu bytes (still copying or cut off)\n", p->path,
                   (unsigned long long)p->size, (unsigned long long)p->full_size);
    }
    catalog_group(cat, NULL);
    if (announce && !cat->ngames) /* say so, as the ps4 folder scan does */
        notify_kind(NOTIFY_DRIVE, TR("%s: no PS4 games found", "%s: no se encontraron juegos de PS4"), root_label(root));
    activity_clear();
    return cat->ngames > 0;
}

/* Add-ons deleted on the console come back only with their own game: drive reconnected
 * with that game present, or that game added/restored through the panel. Unrelated titles
 * must not resurrect a DLC the user deleted. */
static void clear_removed_dlc_tids(char (*tids)[10], int ntids) {
    DIR *d = opendir(STATE_DIR);
    if (!d)
        return;
    struct dirent *e;
    char p[PATH_MAX];
    while ((e = readdir(d)))
        for (int i = 0; i < ntids; i++)
            if (retention_clear_dlc_for_tid(e->d_name, tids[i])) {
                snprintf(p, sizeof(p), STATE_DIR "/%s", e->d_name);
                unlink(p);
                break;
            }
    closedir(d);
}

static void clear_removed_dlc_for(const char *tid) {
    char one[1][10];
    if (!is_game_id(tid))
        return;
    memcpy(one[0], tid, sizeof(one[0]));
    clear_removed_dlc_tids(one, 1);
}

static int readd_queue(const char *tid, const char *why);

/* WEBAPI-4 boundary: upload and links threads request scans here; the scan itself
 * remains main-loop state. The mutex only covers this small intent machine, never a scan. */
static pthread_mutex_t g_scanreq_mu = PTHREAD_MUTEX_INITIALIZER;
static scanreq_t g_scanreq;

static void scan_request_why(int manual, scan_trigger_t trig) {
    pthread_mutex_lock(&g_scanreq_mu);
    scanreq_request_why(&g_scanreq, manual, trig);
    pthread_mutex_unlock(&g_scanreq_mu);
}

static void scan_request_hard(void) {
    pthread_mutex_lock(&g_scanreq_mu);
    scanreq_request_hard(&g_scanreq);
    pthread_mutex_unlock(&g_scanreq_mu);
}

static int scan_due(time_t now) {
    pthread_mutex_lock(&g_scanreq_mu);
    int due = scanreq_due(&g_scanreq, now);
    pthread_mutex_unlock(&g_scanreq_mu);
    return due;
}

static unsigned scan_begin_why(int *manual, int *hard, scan_trigger_t *trig) {
    pthread_mutex_lock(&g_scanreq_mu);
    unsigned seq = scanreq_begin_why(&g_scanreq, manual, hard, trig);
    pthread_mutex_unlock(&g_scanreq_mu);
    return seq;
}

static void scan_keep_request_why(int manual, int hard, scan_trigger_t trig) {
    pthread_mutex_lock(&g_scanreq_mu);
    scanreq_keep_request_why(&g_scanreq, manual, hard, trig);
    pthread_mutex_unlock(&g_scanreq_mu);
}

static void scan_finish(unsigned seq) {
    refresh_sandboxes();
    pthread_mutex_lock(&g_scanreq_mu);
    scanreq_finish(&g_scanreq, seq, time(NULL), scanreq_period(g_nsandboxes > 0));
    pthread_mutex_unlock(&g_scanreq_mu);
}

static int g_stop; /* one-way atomic stop intent: the long loops end before the daemon does */
static int stop_requested(void) { return __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE); }

/* PS4Mount installed or in AutoRun (P4N-32). Published from the main thread;
 * the panel threads only load these. */
static int g_cx_installed, g_cx_autorun, g_cx_state, g_cx_block;

static int auto_register_blocked(void) { return __atomic_load_n(&g_cx_block, __ATOMIC_ACQUIRE); }

static void coexist_publish(void) {
    coexist_evidence_t e;
    coexist_probe(coexist_console_paths(), &e);
    int inst = coexist_ps4mount_installed(&e);
    int block = coexist_block_auto_register(&e);
    int prev = __atomic_load_n(&g_cx_block, __ATOMIC_RELAXED);
    __atomic_store_n(&g_cx_installed, inst, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cx_autorun, e.autorun, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cx_state, e.state_dir, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cx_block, block, __ATOMIC_RELEASE);
    if (block && !prev)
        printf("[coexist] PS4Mount %s%s%s: auto-register is off\n", inst ? "installed" : "",
               inst && e.autorun ? " and " : "", e.autorun ? "in AutoRun" : (inst ? "" : "present"));
}

static void coexist_warn_once(void) {
    static int said;
    if (said)
        return;
    said = 1;
    notify_kind(NOTIFY_WARN, "%s",
                TR("PS4Mount is installed or set to start with the console. Lodestar is not adding games on its own. Disable PS4Mount in GoldHEN AutoRun, uninstall the PS4Mount app, then restart and load only Lodestar.",
                   "PS4Mount está instalado o arranca con la consola. Lodestar no añade juegos por su cuenta. Desactiva PS4Mount en el AutoRun de GoldHEN, desinstala la app de PS4Mount, reinicia y carga solo Lodestar."));
}

static void coexist_json(buf_t *out) {
    char js[256];
    coexist_format_json(__atomic_load_n(&g_cx_installed, __ATOMIC_ACQUIRE),
                        __atomic_load_n(&g_cx_autorun, __ATOMIC_ACQUIRE),
                        __atomic_load_n(&g_cx_state, __ATOMIC_ACQUIRE),
                        __atomic_load_n(&g_cx_block, __ATOMIC_ACQUIRE), js, sizeof(js));
    buf_put(out, js, strlen(js));
}

static void scan_root(const char *root, int fresh, int reconnected) {
    const char *label = root_label(root);
    int have = in_place() ? catalog_has_games(root) : has_candidates(root);
    if (!have) {
        if (fresh)
            printf("[scan] %s: no ps4 folder or pkg, skipped\n", root);
        return;
    }
    activity_set("scanning", label, NULL, NULL, 0, 0, 0, 0);
    if (fresh && g_scan_manual_active && !in_place())
        notify_kind(NOTIFY_DRIVE, TR("Scanning %s for PS4 games...", "Buscando juegos de PS4 en %s..."), label);
    int moved = in_place() ? 0 : adopt_root_pkgs(root);
    if (moved && !fresh)
        notify_kind(NOTIFY_DRIVE, TR("%s: %d new package(s) found", "%s: %d pkg(s) nuevo(s) encontrado(s)"), label, moved);
    static title_t titles[MAX_TITLES];
    static char tids[MAX_TITLES][10];
    static uint64_t fps[MAX_TITLES];
    int count = 0, pending = 0;
    int nt = titles_on_root(root, tids, MAX_TITLES);
    if (reconnected)
        clear_removed_dlc_tids(tids, nt);
    if (nt) {
        bgftscan_load(&g_bgftscan, BGFT_DIR, BGFT_FIRST, BGFT_LAST);
        if (g_scan_m.active) {
            g_scan_m.bgft_dirs += g_bgftscan.dirs_scanned;
            g_scan_m.bgft_pdb_reads += g_bgftscan.pdb_reads;
            g_scan_m.bgft_pdb_bytes += g_bgftscan.pdb_bytes;
        }
    }
    for (int k = 0; k < nt && count < MAX_TITLES; k++) {
        if (stop_requested())
            break; /* the panel asked to stop */
        if (!(k % 8))
            pump_now(); /* the panel answers while a library is walked */
        activity_set("scanning", label, NULL, NULL, k, nt, 0, 0);
        const char *tid = tids[k];
        uint64_t fp = title_files_fp(root, tid, &g_cfg);
        title_info_copy_t icopy;
        if (title_info_fp_copy(root, tid, fp, &g_cfg, NULL, &icopy, g_scan_m.active ? &g_scan_m.fp : NULL))
            continue;
        const title_info_t *info = &icopy.info;
        if (reconnected) {
            char rm[64];
            snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", tid);
            unlink(rm); /* reconnecting the USB drive brings removed games back */
        }
        fps[count] = fp;
        title_t *t = &titles[count++];
        snprintf(t->tid, sizeof(t->tid), "%s", tid);
        snprintf(t->name, sizeof(t->name), "%s", info->name);
        t->ndlc = info->ndlc;
        t->has_patch = info->has_patch;
        t->state = title_state(info);
        if (t->state == 1 && retry_waiting(t->tid))
            t->state = -1; /* failed recently: tried again later */
        pending += t->state == 1;
    }
    /* PS4Mount already owns this library, or it is set to start with the console.
     * A title it registered still looks new here (its task and .src are not ours),
     * and registering it again is what corrupts the menu. Leave those titles. */
    if (auto_register_blocked()) {
        int held = 0;
        for (int i = 0; i < count; i++) {
            if (titles[i].state == 1) {
                titles[i].state = -1;
                held++;
            }
        }
        if (held) {
            pending = 0;
            coexist_warn_once();
            printf("[coexist] %s: %d title(s) not auto-registered while PS4Mount is present\n", label, held);
        }
    }
    /* the totals for a scan asked in the panel; otherwise only what changed */
    if (fresh && g_scan_manual_active && !count)
        notify_kind(NOTIFY_DRIVE, TR("%s: no PS4 games found", "%s: no se encontraron juegos de PS4"), label);
    else if (fresh && g_scan_manual_active)
        notify_kind(NOTIFY_DRIVE,
                    TR("%s: %d PS4 game(s) found, %d new or updated",
                       "%s: %d juego(s) de PS4 encontrado(s), %d nuevo(s) o actualizado(s)"),
                    label, count, pending);
    else if (pending)
        notify_kind(NOTIFY_DRIVE, TR("%s: %d new or updated PS4 game(s)", "%s: %d juego(s) de PS4 nuevo(s) o actualizado(s)"),
                    label, pending);
    /* games already in the library: keep them mounted */
    for (int i = 0; i < count; i++) {
        if (stop_requested())
            break;
        if (!(i % 8))
            pump_now();
        title_t *t = &titles[i];
        title_info_copy_t icopy;
        if (t->state != 0 ||
            title_info_fp_copy(root, t->tid, fps[i], &g_cfg, NULL, &icopy, g_scan_m.active ? &g_scan_m.fp : NULL))
            continue;
        const title_info_t *info = &icopy.info;
        enforce_patch_sfo(info->title_id, info->has_patch, info->patch_path, info->sfo_len, info->sfo_sha);
        manage_info(info);
        /* PS4: the system registered another version (the update came later) */
        const char *rv = db_app_ver(t->tid);
        if (rv && *rv && strcmp(rv, info->ver)) {
            if (auto_register_blocked())
                printf("[coexist] %s: version differs, not re-registered while PS4Mount is present\n", t->tid);
            else {
                char why[64];
                snprintf(why, sizeof(why), TR("registered as %s, the update is %s", "registrado como %s, la actualización es %s"), rv,
                         info->ver);
                if (readd_queue(t->tid, why))
                    printf("[reg] %s: %s\n", t->tid, why);
            }
        }
        managed_t *m = managed_by(t->tid, NULL);
        int running = title_running(t->tid);
        if (m && running && !m->running) {
            m->running = 1; /* open before this daemon started (restart or crash) */
            runtime_publish(t->tid, 1);
        } else if (m && !running && m->running && time(NULL) - m->running_since > 60) {
            m->running = 0; /* closed without a klog end event */
            runtime_publish(t->tid, 0);
        }
        if (mount_info(info)) {
            if (!m || !m->mount_warned)
                notify_kind(NOTIFY_WARN, TR("Could not mount %s (%s). It is tried again on the next scan",
                                           "No se pudo montar %s (%s). Se vuelve a intentar en la próxima búsqueda"), info->name, t->tid);
            if (m)
                m->mount_warned = 1;
        } else if (m) {
            m->mount_warned = 0;
        }
    }
    if (!pending) {
        bgftscan_free(&g_bgftscan);
        activity_clear();
        return;
    }

    /* New or changed games, in groups: one AppInstallAll per group and a quiet
     * period between groups, so the system is never flooded with registrations. */
    int size = g_cfg.batch && g_cfg.batch_size > 0 ? g_cfg.batch_size : pending;
    int groups = (pending + size - 1) / size, grouped = groups > 1, done = 0, ok = 0, group = 0;
    int restart = 0; /* added games that wait for a restart (also when an earlier load marked it) */
    if (grouped)
        notify_detail(TR("%s: adding %d PS4 games in %d groups of up to %d", "%s: añadiendo %d juego(s) de PS4 en %d grupo(s) de hasta %d"),
                      label, pending, groups, size);
    int *members = malloc(sizeof(int) * (size_t)size);
    char (*ids)[10] = malloc(10 * (size_t)size);
    int *prepared = malloc(sizeof(int) * (size_t)size);
    if (!members || !ids || !prepared) {
        free(members);
        free(ids);
        free(prepared);
        bgftscan_free(&g_bgftscan);
        activity_clear();
        return;
    }
    memset(prepared, 0, sizeof(int) * (size_t)size);
    for (int i = 0; i < count;) {
        if (stop_requested())
            break;
        int n = 0;
        for (; i < count && n < size; i++)
            if (titles[i].state == 1 && !delete_pending_is(titles[i].tid))
                members[n++] = i;
        if (!n)
            break;
        if (group++)
            wait_db_quiet(BATCH_QUIET, 120);
        if (grouped) {
            buf_t names = {0};
            for (int k = 0; k < n; k++)
                buf_printf(&names, "%s%s", k ? ", " : "", titles[members[k]].name);
            notify_detail(TR("Group %d/%d, adding %d-%d of %d: %.400s", "Grupo %d/%d, añadiendo %d-%d de %d: %.400s"), group, groups,
                          done + 1, done + n, pending,
                   names.data ? (const char *)names.data : "");
            buf_free(&names);
        }
        int np = 0;
        for (int k = 0; k < n; k++) {
            if (stop_requested())
                break;
            if (!(k % 4))
                pump_now(); /* the panel answers while the files of a group are prepared */
            title_t *t = &titles[members[k]];
            title_src_t src;
            prepared[k] = 0;
            if (delete_pending_is(t->tid))
                continue;
            activity_set("registering", label, t->name, t->tid, done + k + 1, pending, group, groups);
            if (!grouped) {
                char extra[64] = "";
                if (t->ndlc)
                    snprintf(extra, sizeof(extra), TR(" + %d add-on(s)", " + %d DLC"), t->ndlc);
                notify_detail(TR("Adding %d/%d: %s%s%s (%s)", "Añadiendo %d/%d: %s%s%s (%s)"), done + k + 1, pending, t->name,
                              t->has_patch ? TR(" with update", " con actualización") : "", extra, t->tid);
            }
            if (src_open(&src, root, t->tid, &g_cfg) || register_prepare(&src)) {
                if (retry_failed(t->tid))
                    notify_kind(NOTIFY_WARN, TR("Failed to add %s (%s), it is tried again every few minutes",
                                                "No se pudo añadir %s (%s), se vuelve a intentar cada pocos minutos"),
                                t->name, t->tid);
            } else {
                prepared[k] = 1;
                snprintf(ids[np++], 10, "%s", t->tid);
            }
            if (src.app.fd > 0)
                src_close(&src);
        }
        printf("[reg] group %d/%d: %d of %d ready, registering\n", group, groups, np, n);
        install_all(ids, np);
        /* own rows only after ShellCore stopped writing to its databases */
        wait_db_quiet(3, 30);
        for (int k = 0; k < n; k++) {
            title_t *t = &titles[members[k]];
            title_src_t src;
            if (!prepared[k] || delete_pending_is(t->tid) || src_open(&src, root, t->tid, &g_cfg))
                continue;
            if (register_finish(&src)) {
                int first = retry_failed(t->tid);
                if (!first && src.has_patch) {
                    /* twice without the system taking it: the update is what it turns down
                     * (CheckCumulativePatch); left out, it is tried again now */
                    refuse_patch(t->tid, src.patch_path, t->name);
                    retry_clear(t->tid);
                } else if (first) {
                    notify_kind(NOTIFY_WARN, TR("Failed to add %s (%s), it is tried again every few minutes",
                                                "No se pudo añadir %s (%s), se vuelve a intentar cada pocos minutos"),
                                t->name, t->tid);
                }
            } else {
                retry_clear(t->tid);
                ok++;
                int reboot = needs_reboot(t->tid);
                restart += reboot;
                if (!grouped && reboot) {
                    if (pending == 1)
                        notify_kind(NOTIFY_WARN, TR("%s added. Restart the console once to play it.%s",
                                                    "%s añadido. Reinicia la consola una vez para jugarlo.%s"),
                                    t->name, after_restart());
                    else
                        notify_detail_kind(NOTIFY_WARN, TR("%s added. Restart the console once to play it.%s",
                                                           "%s añadido. Reinicia la consola una vez para jugarlo.%s"),
                                           t->name, after_restart());
                } else if (!grouped) {
                    if (pending == 1)
            notify_kind(NOTIFY_READY, TR("%s ready to play", "%s listo para jugar"), t->name);
                    else
                        notify_detail_kind(NOTIFY_READY, TR("%s ready to play", "%s listo para jugar"), t->name);
                }
            }
            src_close(&src);
        }
        done += n;
    }
    free(members);
    free(ids);
    free(prepared);
    bgftscan_free(&g_bgftscan); /* the epoch ends after registration mutations are reconciled */
    if (pending > 1)
        notify_kind(NOTIFY_READY, TR("%s: %d of %d game(s) added%s%s", "%s: %d de %d juego(s) añadido(s)%s%s"), label, ok,
                    pending,
                    restart           ? TR(". Restart the console once to play the new ones.",
                                           ". Reinicia la consola una vez para jugar los nuevos.")
                    : ok == pending ? TR(", ready to play", ", listos para jugar")
                                    : "",
                    restart ? after_restart() : "");
    activity_clear();
}

/* Releases mounts over /user/app/<TID> of older versions, which mounted the drive
 * folder itself, once that folder is gone. The internal folders mounted now stay
 * while the drive is away (rest mode powers drives off): their pkg links simply
 * point to nothing until the drive is back. */
static void release_stale(void) {
    DIR *d = opendir(STATE_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n != 13 || strcmp(e->d_name + 9, ".src"))
            continue;
        char tid[10], target[64], from[PATH_MAX], pkg_path[PATH_MAX + 16];
        snprintf(tid, sizeof(tid), "%.9s", e->d_name);
        snprintf(target, sizeof(target), "/user/app/%s", tid);
        if (!mounted_from(target, from, sizeof(from)) || strncmp(from, "/mnt/", 5))
            continue;
        snprintf(pkg_path, sizeof(pkg_path), "%s/app.pkg", from);
        if (!exists(pkg_path)) {
            printf("[scan] releasing %s (source gone)\n", target);
            managed_t *m = managed_by(tid, NULL);
            if (m) {
                unmount_title(tid, m->dlc_tids, m->ndlc_tids);
                managed_mark_unused(m);
            } else {
                unmount_title(tid, NULL, 0);
            }
        }
    }
    closedir(d);
}

/* What each game of the drive is, as the last run left it: a reload of the payload does
 * not read the whole library again. */
static int g_titles_loaded[NROOTS];

static void titles_file_name(const char *root, char *out, size_t n) {
    char id[64];
    root_id(root, id, sizeof(id));
    snprintf(out, n, STATE_DIR "/titles-%s.txt", id);
}

static void titles_file_load(int i, const char *root) {
    if (g_titles_loaded[i])
        return;
    g_titles_loaded[i] = 1;
    char p[PATH_MAX];
    titles_file_name(root, p, sizeof(p));
    pthread_mutex_lock(&g_titlecache_mu);
    title_cache_load(p);
    pthread_mutex_unlock(&g_titlecache_mu);
}

static void titles_file_save(const char *root) {
    char p[PATH_MAX], key[64];
    titles_file_name(root, p, sizeof(p));
    pthread_mutex_lock(&g_titlecache_mu);
    title_cache_save(p, root_id(root, key, sizeof(key)));
    pthread_mutex_unlock(&g_titlecache_mu);
}

/* --- emulators ---
 * <drive>/Lodestar/emulators/<App>/<Folder> is shown as /data/<App>/<Folder> (emu.h), where
 * an emulator app looks for its ROMs and BIOS, so they stay on the drive. Nothing is
 * configured: the folders are the setting. Nothing of the console's is replaced: only an
 * empty folder (or one made here) is covered, and only when the app's own folder is there
 * already, so an app that was never started is not given a folder. */

static int emu_is_dir_now(const char *path) {
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode);
}

static int emu_dir_empty(const char *path) {
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *e;
    int empty = 1;
    while (empty && (e = readdir(d)))
        empty = !strcmp(e->d_name, ".") || !strcmp(e->d_name, "..");
    closedir(d);
    return empty;
}

/* A reason for not mounting is said once: the scan comes back every few seconds. */
static void emu_said(const char *target, const char *why) {
    static uint32_t said[64];
    static int n;
    char key[PATH_MAX + 64];
    snprintf(key, sizeof(key), "%s|%s", target, why);
    uint32_t h = str_hash(key);
    for (int i = 0; i < n; i++)
        if (said[i] == h)
            return;
    if (n < 64)
        said[n++] = h;
    printf("[emu] %s not mounted: %s\n", target, why);
}

static int emu_mount_one(const char *src, const char *app, const char *folder) {
    char target[PATH_MAX], appdir[PATH_MAX];
    if (emu_target(app, folder, target, sizeof(target))) {
        emu_said(src, TR("the name is not one Lodestar mounts", "el nombre no es de los que Lodestar monta"));
        return 0;
    }
    snprintf(appdir, sizeof(appdir), "/data/%s", app);
    int appdir_exists = emu_is_dir_now(appdir);
    int top = top_layer(target, src);
    struct stat st;
    int got = !lstat(target, &st), missing = !got && errno == ENOENT, stat_error = !got && !missing;
    emu_step_t step = emu_mount_step(1, appdir_exists, top, missing, stat_error, got && S_ISDIR(st.st_mode),
                                     got && S_ISDIR(st.st_mode) && emu_dir_empty(target));
    if (step == EMU_STEP_ALREADY)
        return 0;
    if (step == EMU_STEP_REFUSE) {
        if (!appdir_exists)
            emu_said(target, TR("the app has no folder in /data yet: start it once", "la app aún no tiene carpeta en /data: ábrela una vez"));
        else if (top < 0)
            emu_said(target, TR("something else is mounted there", "ya hay otra cosa montada ahí"));
        else if (stat_error)
            emu_said(target, TR("the console's folder could not be read", "la carpeta de la consola no se pudo leer"));
        else if (got && !S_ISDIR(st.st_mode))
            emu_said(target, TR("it is not a folder", "no es una carpeta"));
        else
            emu_said(target, TR("the console's folder already has files (it is left as it is)", "la carpeta de la consola ya tiene archivos (se deja como está)"));
        return 0;
    }
    if (step == EMU_STEP_MKDIR_MOUNT && mkdir(target, 0777)) {
        printf("[emu] %s: %s\n", target, strerror(errno));
        return 0;
    }
    if (nullfs_mount(src, target))
        return 0;
    if (top_layer(target, src) != 1) {
        printf("[emu] %s: mount did not verify as the exact top layer; left for the next scan\n", target);
        return 0;
    }
    printf("[emu] %s is now %s\n", src, target);
    return 1;
}

static int emu_mount_matches(const char *from, const char *on) {
    for (int i = 0; i < 10; i++) {
        char root[32];
        root_path(i, root, sizeof(root));
        if (emu_mount_owned(root, from, on))
            return 1;
    }
    return 0;
}

/* Drop stale mounts and fill the free slots. Keep at most 32 across scans. */
static void emu_sync(void) {
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return;
    size_t len = sizeof(struct statfs) * (size_t)(n + 16);
    struct statfs *list = mem_alloc(len);
    if (!list)
        return;
    scan_note_mount_read();
    n = getfsstat(list, (long)len, MNT_NOWAIT);
    int mounted = 0;
    for (int i = 0; i < n; i++) {
        const char *on = list[i].f_mntonname, *from = list[i].f_mntfromname;
        if (strcmp(list[i].f_fstypename, "nullfs") || !emu_mount_matches(from, on))
            continue;
        mounted++;
        int dead = dead_layer(on);
        if (!emu_stale_unmount_ok(emu_is_dir_now(from), dead, layers_on(on)))
            continue;
        if (unmount_tracked(on, MNT_FORCE))
            printf("[emu] %s: could not unmount %s: %s\n", on, from, strerror(errno));
        else {
            printf("[emu] %s: %s is gone, taken off\n", on, from);
            mounted--;
        }
    }
    mem_free(list);
    if (n < 0)
        return;
    for (int i = 0; i < 10 && mounted < 32; i++) { /* the USB drives and the extended storage */
        char root[32], parent[64], base[PATH_MAX];
        root_path(i, root, sizeof(root));
        if (!is_mountpoint(root))
            continue;
        snprintf(parent, sizeof(parent), "%s/Lodestar", root);
        snprintf(base, sizeof(base), "%s/" EMU_DIR, root);
        if (!emu_is_dir_now(parent) || !emu_is_dir_now(base))
            continue;
        DIR *d = opendir(base);
        struct dirent *e;
        while (d && (e = readdir(d)) && mounted < 32) {
            char appsrc[PATH_MAX];
            if (e->d_name[0] == '.' || snprintf(appsrc, sizeof(appsrc), "%s/%s", base, e->d_name) >= (int)sizeof(appsrc) ||
                !emu_is_dir_now(appsrc))
                continue;
            DIR *a = opendir(appsrc);
            struct dirent *f;
            while (a && (f = readdir(a)) && mounted < 32) {
                char src[PATH_MAX];
                if (f->d_name[0] == '.' || snprintf(src, sizeof(src), "%s/%s", appsrc, f->d_name) >= (int)sizeof(src) ||
                    !emu_is_dir_now(src))
                    continue;
                mounted += emu_mount_one(src, e->d_name, f->d_name);
            }
            if (a)
                closedir(a);
        }
        if (d)
            closedir(d);
    }
}

/* Mounted copies can outlive a daemon reload. Detach only PS4Mount's layer, and
 * wait for a game in progress to close before removing any of its add-ons. */
static void release_dlc_mounts(void) {
    if (g_cfg.dlc_mode == DLC_EXTERNAL)
        return;
    refresh_sandboxes();
    if (g_nsandboxes)
        return;
    static const char prefix[] = "/user/addcont/";
    int n = getfsstat(NULL, 0, MNT_NOWAIT);
    if (n <= 0)
        return;
    size_t len = sizeof(struct statfs) * (size_t)(n + 16);
    struct statfs *list = mem_alloc(len);
    if (!list)
        return;
    scan_note_mount_read();
    n = getfsstat(list, (long)len, MNT_NOWAIT);
    for (int i = 0; i < n; i++) {
        const char *on = list[i].f_mntonname, *from = list[i].f_mntfromname;
        if (strncmp(on, prefix, sizeof(prefix) - 1) || strcmp(list[i].f_fstypename, "nullfs"))
            continue;
        const char *tid = on + sizeof(prefix) - 1;
        char expected[64];
        snprintf(expected, sizeof(expected), HOLD_DIR "/addcont/%.9s", tid);
        if (!is_game_id(tid) || strcmp(from, expected) || top_layer(on, from) != 1)
            continue;
        if (unmount_tracked(on, 0))
            printf("[dlc] could not unmount %s: %s\n", on, strerror(errno));
        else
            printf("[dlc] %s unmounted for mode %s\n", tid, dlc_mode_name(g_cfg.dlc_mode));
    }
    mem_free(list);
}

static void rescan_invalidate(void);

static int scan_all(int hard) {
    static int seen[NROOTS], first = 1;
    char root[PATH_MAX];
    emu_sync();
    release_dlc_mounts();
    if (!db_ready()) {
        /* every game would look unregistered and be added again: wait for a good read */
        printf("[scan] the system's app.db could not be read: the scan waits\n");
        return 0;
    }
    if (hard)
        rescan_invalidate();
    release_stale();
    if (!g_cfg.organize) {
        /* Pkgs are used where they are: bring every catalog up to date first, then group
         * the pkgs of each drive into games (catalog_link). */
        int ready[NROOTS] = {0};
        time_t now = time(NULL);
        pthread_mutex_lock(&g_cat_lock);
        catalog_unlink(g_cat, NROOTS); /* pkgs move when a drive is walked: drop cross refs */
        for (int i = 0; i < NROOTS; i++) {
            root_path(i, root, sizeof(root));
            if (root_present(root)) {
                int due = g_scan_manual_active || !g_cat_loaded[i] ||
                          now - g_cat_walked[i] >= CATALOG_SECONDS;
                if (due) {
                    ready[i] = catalog_update(&g_cat[i], root, !seen[i] || g_scan_manual_active);
                    g_cat_walked[i] = now;
                } else {
                    ready[i] = catalog_has_games(root);
                }
            } else if (g_cat_loaded[i]) {
                catalog_free(&g_cat[i]); /* checked again when the drive is back: it may have changed */
                g_cat_loaded[i] = 0;
                g_cat_walked[i] = 0;
            }
        }
        /* Games are grouped per drive: an update or add-on of another drive is not used,
         * so unplugging one drive never breaks a game of another. */
        catalog_link(g_cat, NROOTS, NULL, 0);
        catalog_epoch_publish_locked();
        pthread_mutex_unlock(&g_cat_lock);
        for (int i = 0; i < NROOTS; i++) {
            root_path(i, root, sizeof(root));
            int present = root_present(root);
            int fresh = !seen[i] || g_scan_manual_active;
            if (ready[i]) {
                titles_file_load(i, root);
                scan_root(root, fresh, !seen[i] && !first);
                titles_file_save(root);
            }
            seen[i] = present;
        }
    } else {
        /* Back to moving pkgs: the catalogs are not used any more. */
        int any = 0;
        for (int i = 0; i < NROOTS; i++)
            any |= g_cat_loaded[i];
        if (any) {
            pthread_mutex_lock(&g_cat_lock);
            for (int i = 0; i < NROOTS; i++) {
                catalog_free(&g_cat[i]);
                g_cat_loaded[i] = 0;
                g_cat_walked[i] = 0;
            }
            catalog_epoch_publish_locked();
            pthread_mutex_unlock(&g_cat_lock);
        }
        for (int i = 0; i < NROOTS; i++) {
            root_path(i, root, sizeof(root));
            int present = root_present(root);
            if (present) {
                titles_file_load(i, root);
                scan_root(root, !seen[i] || g_scan_manual_active, !seen[i] && !first);
                titles_file_save(root);
            }
            seen[i] = present;
        }
    }
    first = 0;
    return 1;
}

/* --- klog events --- */

static int klog_connect(void) {
    int s;
    if (!g_cfg.klog) {
        printf("[klog] off: the log is left to another payload\n");
        return -1;
    }
    s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(3232);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a))) {
        printf("[klog] klogsrv\n");
        return s;
    }
    if (s >= 0)
        close(s);
    int fd = open("/dev/klog", O_RDONLY);
    printf("[klog] /dev/klog fd=%d\n", fd);
    return fd;
}

static const char *after(const char *line, const char *tag) {
    const char *p = strstr(line, tag);
    return p ? p + strlen(tag) : NULL;
}

static void end_delete_window(void) {
    g_deleting = 0;
    g_deleting_close_at = 0;
    for (int i = 0; i < MAX_TITLES; i++)
        if (g_managed[i].used)
            mount_paths(g_managed[i].tid, g_managed[i].has_patch, g_managed[i].dlc_tids, g_managed[i].ndlc_tids);
    printf("[delete] window closed, game folders mounted again\n");
}

/* The delete-screen classifier is the host-tested pure contract in delete.h. */
static int delete_scene(const char *name) { return delete_scene_kind(name); }

static void start_delete_window(void) {
    g_deleting_since = time(NULL);
    g_deleting_close_at = 0;
    if (g_deleting)
        return;
    g_deleting = 1;
    for (int i = 0; i < MAX_TITLES; i++) {
        managed_t *m = &g_managed[i];
        if (!m->used)
            continue;
        if (m->running || title_running(m->tid))
            continue; /* still open: unmounted when it closes (see the End event) */
        unmount_title(m->tid, m->dlc_tids, m->ndlc_tids);
    }
    printf("[delete] delete screen open, game folders unmounted\n");
}

/* Copies klog lines about our titles (and crash/mount errors) into the log. Exact repeats
 * are counted for a minute and summarized, so a kernel loop cannot drown the evidence that
 * came before it. STATE_DIR/klog_all is the diagnosis switch: it copies everything whole. */
static void trace_klog(const char *line) {
    static time_t checked;
    static int all;
    if (time(NULL) != checked) {
        checked = time(NULL);
        all = exists(STATE_DIR "/klog_all");
    }
    if (all && !strstr(line, "[REGMGR]")) {
        printf("[klog] %.300s\n", line);
        return;
    }
    int copy = 0;
    static const char *keys[] = {"CRASH", "Trophy:Register", "_pfs_mount", "mountpfs", "PrepareProcessLaunch",
                                 "preLaunchCheck", "scePlayGoCore", "process_mount", "process_check", "sqlite",
                                 "SQLITE", "corrupt", "Corrupt", "CORRUPT", "dbr", "DBR", NULL};
    for (int i = 0; keys[i] && !copy; i++)
        copy = strstr(line, keys[i]) != NULL;
    for (int i = 0; !copy && i < MAX_TITLES; i++)
        copy = g_managed[i].used && strstr(line, g_managed[i].tid) && !strstr(line, "BGLayer") && !strstr(line, "PSM.UI");
    if (!copy)
        return;
    static kdup_t dedup;
    char note[256];
    time_t now = time(NULL);
    int action = kdup_line(&dedup, line, now, note, sizeof(note));
    if (action & KDUP_NOTE)
        printf("[klog] %s\n", note);
    if (action & KDUP_WRITE)
        printf("[klog] %.300s\n", line);
    while (kdup_flush(&dedup, now, note, sizeof(note)))
        printf("[klog] %s\n", note);
}

/* "system file disagree [<TID>]": the system refused to start a game with an update.
 * Removing the game from the library and adding it again made it start; this is done
 * once per title and daemon run, so a game that still fails is not removed again. */
static char g_readd_tid[10], g_readd_why[64], g_readd_active[10];
/* The readd of a game whose update was refused: removing it clears the mark (clean_holds),
 * so it is written again before the game is added back. */
static int g_readd_badpatch;
static char g_readd_patch[PATH_MAX];
static char g_readd_done[MAX_TITLES][10];
static int g_nreadd_done;
static char g_last_launch[10]; /* the PS4's "APP_VER disagree" line names no title */

/* Asks the main loop to remove tid from the library and add it again (once per title
 * and daemon run). Returns 1 when queued. */
static int readd_queue(const char *tid, const char *why) {
    for (int i = 0; i < g_nreadd_done; i++)
        if (!strcmp(g_readd_done[i], tid))
            return 0; /* once per title and daemon run: the scan runs again and again */
    if (g_readd_tid[0])
        return 0;
    snprintf(g_readd_tid, sizeof(g_readd_tid), "%s", tid);
    snprintf(g_readd_why, sizeof(g_readd_why), "%s", why);
    return 1;
}

/* A game added from a drive that is not connected now: 1 with its folder (from its
 * marker), the drive's name and the name the system registered. */
static int offline_game(const char *tid, char *dir, size_t dn, char *drive, size_t vn, char *name, size_t nn) {
    char p[96];
    size_t len = 0;
    snprintf(p, sizeof(p), STATE_DIR "/%s.src", tid);
    uint8_t *m = read_all(p, &len);
    if (!m)
        return 0;
    snprintf(dir, dn, "%.*s", (int)strcspn((char *)m, "\n"), (char *)m);
    mem_free(m);
    /* only drives: /mnt/usb0/ps4/<TID>, or the pkg itself when it is used where it is */
    if (strncmp(dir, "/mnt/", 5) || !db_is_registered(tid))
        return 0;
    size_t l = strlen(dir);
    int file = l > 4 && !strcasecmp(dir + l - 4, ".pkg");
    if (file)
        snprintf(p, sizeof(p), "%s", dir);
    else
        snprintf(p, sizeof(p), "%s/app.pkg", dir);
    if (exists(p))
        return 0;
    snprintf(drive, vn, "%.*s", (int)strcspn(dir + 5, "/"), dir + 5);
    snprintf(name, nn, "%s", tid);
    snprintf(p, sizeof(p), "/system_data/priv/appmeta/%s/param.sfo", tid);
    uint8_t *d = read_all(p, &len);
    sfo_t *sfo = mem_alloc(sizeof(*sfo));
    if (d && sfo && !sfo_parse(d, len, sfo)) {
        const sfo_item_t *t = sfo_get(sfo, "TITLE");
        if (t && !t->is_int && t->sval[0])
            snprintf(name, nn, "%s", t->sval);
    }
    mem_free(sfo);
    mem_free(d);
    return 1;
}

/* The system cannot open the pkg of a game being started: when its drive is away, say
 * so (the system's own error names no reason). */
static void open_failed(const char *tid) {
    char dir[PATH_MAX], drive[32], name[128];
    if (offline_game(tid, dir, sizeof(dir), drive, sizeof(drive), name, sizeof(name)))
        notify_kind(NOTIFY_WARN, TR("%s is on %s, which is not connected. Connect it and start the game again",
                  "%s está en %s, que no está conectado. Conéctalo y vuelve a abrir el juego"),
               name, drive);
}

static void disagree_seen(const char *tid) {
    managed_t *m = managed_by(tid, NULL);
    if (!m)
        return;
    m->running = 0; /* the launch was refused */
    runtime_publish(tid, 0);
    if (!m->has_patch)
        return;
    /* The system holds one version of the game and the update is not it (it refuses the
     * launch with CheckCumulativePatch): the game is added again without the update. */
    char root[PATH_MAX], patch[PATH_MAX] = "";
    if (!find_root(tid, root, sizeof(root)))
        title_patch_path(root, tid, patch, sizeof(patch));
    /* A mismatch whose repair is already queued/running is circular evidence: the launch
     * disagrees because the console still holds the old registered version, not because the
     * update itself is known bad. Let the re-add try the update. */
    int repair_pending = !strcmp(g_readd_tid, tid) || !strcmp(g_readd_active, tid);
    if (patch[0] && !repair_pending)
        refuse_patch(tid, patch, m->name);
    if (readd_queue(tid, TR("the system found another version", "el sistema encontró otra versión"))) {
        g_readd_badpatch = patch[0] != 0;
        snprintf(g_readd_patch, sizeof(g_readd_patch), "%s", patch);
    }
}

/* PS4Mount's own folders of a game that left the library (the pkgs are not in them).
 * The mark of a refused update is kept: it names the pkg, and the pkg did not change (it
 * also has to survive taking the game out and adding it again). */
static void clean_holds(const char *tid, char (*dtids)[10], int ndtids) {
    char hold[PATH_MAX], link[PATH_MAX];
    snprintf(link, sizeof(link), STATE_DIR "/l/%s-app.pkg", tid);
    unlink(link);
    snprintf(link, sizeof(link), STATE_DIR "/l/%s-patch.pkg", tid);
    unlink(link);
    /* one link per add-on, <add-on TID>-ac-<label>.pkg (and -ac.pkg of the 0.7.4 test builds);
     * always include tid itself so removing an offline or not-yet-scanned game cleans its links */
    DIR *ld = opendir(STATE_DIR "/l");
    struct dirent *le;
    while (ld && (le = readdir(ld))) {
        if (strlen(le->d_name) < 12)
            continue;
        int match = !strncmp(le->d_name, tid, 9) && !strncmp(le->d_name + 9, "-ac", 3);
        for (int i = 0; !match && i < ndtids; i++)
            match = !strncmp(le->d_name, dtids[i], 9) && !strncmp(le->d_name + 9, "-ac", 3);
        if (match) {
            snprintf(link, sizeof(link), STATE_DIR "/l/%s", le->d_name);
            unlink(link);
        }
    }
    if (ld)
        closedir(ld);
    hold_path(hold, sizeof(hold), "app", tid);
    rm_tree(hold);
    hold_path(hold, sizeof(hold), "patch", tid);
    rm_tree(hold);
    snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%s", tid);
    rm_tree(hold);
    for (int i = 0; i < ndtids; i++) {
        snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%s", dtids[i]);
        rm_tree(hold);
    }
    if (!installed_internally(tid) && db_confirms_unregistered(tid)) {
        snprintf(hold, sizeof(hold), "/system_data/playgo/%s", tid);
        struct stat st;
        if (!lstat(hold, &st) && S_ISDIR(st.st_mode) && layers_on(hold) == 0) {
            rm_tree(hold);
            if (lstat(hold, &st) && errno == ENOENT)
                printf("[clean] %s: its PlayGo records were left over (not installed, not registered), deleted\n", tid);
        }
    }
}

/* Bytes of the files under path. */
static unsigned long long tree_size(const char *path) {
    struct stat st;
    if (lstat(path, &st))
        return 0;
    if (!S_ISDIR(st.st_mode))
        return (unsigned long long)st.st_size;
    /* Iterative, not recursive: panel workers have a bounded stack. A path cannot have more
     * one-byte components than this depth, and the frames are a few bytes rather than PATH_MAX each. */
    enum { TREE_MAX_DEPTH = PATH_MAX / 2 };
    DIR *ds[TREE_MAX_DEPTH];
    size_t lens[TREE_MAX_DEPTH];
    char p[PATH_MAX];
    size_t len = (size_t)snprintf(p, sizeof(p), "%s", path);
    if (len >= sizeof(p))
        return 0;
    DIR *first = opendir(p);
    if (!first)
        return 0;
    unsigned long long total = 0;
    int depth = 0;
    ds[0] = first;
    lens[0] = len;
    while (depth >= 0) {
        struct dirent *e = readdir(ds[depth]);
        if (!e) {
            closedir(ds[depth]);
            depth--;
            if (depth >= 0)
                p[lens[depth]] = 0;
            continue;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        size_t base = lens[depth], nl = strlen(e->d_name);
        if (base + 1 + nl >= sizeof(p))
            continue;
        memcpy(p + base, "/", 1);
        memcpy(p + base + 1, e->d_name, nl + 1);
        len = base + 1 + nl;
        if (lstat(p, &st)) {
            p[base] = 0;
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            total += (unsigned long long)st.st_size;
            p[base] = 0;
            continue;
        }
        if (depth + 1 >= TREE_MAX_DEPTH) {
            p[base] = 0;
            continue;
        }
        DIR *next = opendir(p);
        if (!next) {
            p[base] = 0;
            continue;
        }
        depth++;
        ds[depth] = next;
        lens[depth] = len;
    }
    return total;
}

/* Deletes the library folder of tid, <root>/ps4/<tid> (its game, update and add-ons) and
 * nothing else: any other path is refused. The system only reaches the pkgs read-only;
 * deleting them is PS4Mount's own job. 1 deleted, 0 refused. */
static int delete_game_files(const char *dir, const char *tid, const char *name) {
    for (int i = 0; i < NROOTS; i++) {
        char root[PATH_MAX];
        root_path(i, root, sizeof(root));
        if (!delete_target_ok(root, tid, dir))
            continue;
        unsigned long long n = tree_size(dir);
        rm_tree(dir);
        printf("[delete] %s: %s deleted (%llu bytes)\n", tid, dir, n);
        /* GB of 1024 MB, as the panel and the console's own screens count */
        notify_kind(NOTIFY_READY, TR("%s deleted: %.1f GB freed on %s", "%s borrado: %.1f GB liberados en %s"), name,
               (double)n / 1073741824.0, root_label(root));
        return 1;
    }
    printf("[delete] %s: %s is not its library folder, nothing deleted\n", tid, dir);
    return 0;
}

/* A game of ours deleted on the console's delete screens: deleted for real, as an
 * installed game would be, files included (Remove from library on the panel is the way
 * to take a game off the console and keep its pkgs). In-place pkgs are the user's files:
 * the game leaves the library and they stay. */
static void title_deleted(managed_t *m) {
    char path[64];
    snprintf(path, sizeof(path), STATE_DIR "/%s.src", m->tid);
    unlink(path);
    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", m->tid);
    unlink(path);
    clean_holds(m->tid, m->dlc_tids, m->ndlc_tids);
    bgft_remove(m->tid);
    int kept = !delete_game_files(m->dir, m->tid, m->name);
    if (kept) {
        write_file(path, m->tid, strlen(m->tid), 0666); /* not added again until asked */
        printf("[delete] %s: its pkg stays where it is: %s\n", m->tid, m->dir);
    }
    managed_mark_unused(m);
}

/* PS4: "GameWillDelete(<TID>)" is the start of the system's uninstall, not proof that it
 * finished. User files and ownership state therefore stay intact until delete_pending_step
 * sees positive commit evidence; failure or age can only drop the pending suppression. */
static void delete_pending_clear(void) {
    g_will_delete[0] = 0;
    g_will_since = 0;
    g_will_report_at = 0;
    g_will_report = -1;
    g_will_seen_registered = 0;
    g_will_mount_warned = 0;
}

static void delete_pending_begin(managed_t *m) {
    if (delete_pending_is(m->tid))
        return;
    if (g_will_delete[0]) {
        printf("[delete] %s: delete of %s is still pending, not tracking another one yet\n", m->tid, g_will_delete);
        return;
    }
    snprintf(g_will_delete, sizeof(g_will_delete), "%s", m->tid);
    g_will_since = time(NULL);
    g_will_report_at = 0;
    g_will_report = -1;
    g_will_seen_registered = db_confirms_registered(m->tid);
    g_will_mount_warned = 0;
    printf("[delete] %s: user delete pending system outcome\n", m->tid);
}

static void delete_pending_rollback(const char *why) {
    printf("[delete] %s: user data kept (%s)\n", g_will_delete, why);
    delete_pending_clear();
}

static int delete_pending_commit(void) {
    managed_t *m = managed_by(g_will_delete, NULL);
    if (!m) {
        delete_pending_rollback("its ownership evidence was lost");
        return 0;
    }
    unmount_title(m->tid, m->dlc_tids, m->ndlc_tids);
    if (!title_detached(m->tid, m->dlc_tids, m->ndlc_tids)) {
        if (!g_will_mount_warned) {
            g_will_mount_warned = 1;
            printf("[delete] %s: uninstall confirmed, but its mounts are still busy; local delete waits\n", m->tid);
        }
        return 0;
    }
    printf("[delete] %s: system uninstall confirmed, committing local delete\n", m->tid);
    title_deleted(m);
    delete_pending_clear();
    return 1;
}

static void delete_pending_step(void) {
    if (!g_will_delete[0])
        return;
    time_t now = time(NULL);
    int registered = db_confirms_registered(g_will_delete);
    if (registered)
        g_will_seen_registered = 1;
    int db_state = db_confirms_unregistered(g_will_delete) ? 2 : registered ? 1 : 0;
    long since_report = g_will_report >= 0 ? (long)(now - g_will_report_at) : 0;
    int action = delete_pending_decision(g_will_report, db_state, g_will_seen_registered, since_report,
                                         (long)(now - g_will_since));
    if (action > 0)
        delete_pending_commit();
    else if (action < 0)
        delete_pending_rollback(g_will_report == 1 ? "the system reported an uninstall error" : "the outcome timed out");
}

static void handle_klog_line(const char *line) {
    const char *p;
    char id[40];
    trace_klog(line);
    if ((p = after(line, "OnFocusActiveSceneChanged ["))) {
        /* "[from : type] -> [to : type]": follow the delete screens */
        const char *to = strstr(p, "] -> [");
        if (g_deleting)
            printf("[delete] screen %.200s\n", p);
        if (to) {
            to += 6;
            int action = delete_window_transition(delete_scene(to), g_deleting);
            if (action > 0)
                start_delete_window();
            else if (action < 0)
                end_delete_window();
        }
    } else if ((p = after(line, "launchApp("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        snprintf(g_last_launch, sizeof(g_last_launch), "%s", id);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 1;
            m->running_since = time(NULL);
            runtime_publish(id, 1);
        }
    } else if ((p = after(line, "PrepareProcessLaunchPkg() ret = ")) && strtoul(p, NULL, 16) && g_last_launch[0]) {
        /* the system turned the launch down (80990019: no game task yet): it never ran */
        managed_t *m = managed_by(g_last_launch, NULL);
        if (m) {
            m->running = 0;
            runtime_publish(g_last_launch, 0);
        }
    } else if ((p = after(line, "notifyAppCrash() titleId={"))) {
        /* a crash, or a game the system killed because it did not suspend in time: it
         * ends without the PlayGo end event */
        snprintf(id, sizeof(id), "%.9s", p);
        printf("[game] %s ended with an application error\n", id);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 0;
            runtime_publish(id, 0);
        }
    } else if ((p = after(line, "Suspending the app has been timed out. ("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        printf("[game] %s did not suspend in time: the system closes it (unsaved progress is lost)\n", id);
    } else if ((p = after(line, "sceKernelOpen(/user/app/")) && strstr(p, "/app.pkg) failed")) {
        snprintf(id, sizeof(id), "%.9s", p);
        open_failed(id);
    } else if ((p = after(line, "GameWillDelete("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *found = g_deleting ? managed_by(id, NULL) : NULL;
        managed_t *m = delete_user_event_ok(g_deleting, found != NULL) ? found : NULL;
        if (m)
            delete_pending_begin(m);
    } else if ((p = after(line, "AppUnInstallMainExecute ret = [")) && g_will_delete[0]) {
        unsigned long rc = strtoul(p, NULL, 16);
        printf("[delete] %s: the system's delete returned 0x%08lx\n", g_will_delete, rc);
        if (g_will_report < 0) {
            g_will_report = rc != 0;
            g_will_report_at = time(NULL);
        }
        if (g_deleting)
            g_deleting_close_at = time(NULL) + 3;
    } else if ((p = after(line, "system file disagree ["))) {
        snprintf(id, sizeof(id), "%.9s", p);
        disagree_seen(id);
    } else if (after(line, "APP_VER disagree [") && g_last_launch[0]) {
        disagree_seen(g_last_launch); /* PS4: registered and update versions differ */
    } else if ((p = after(line, "[PlayGoService][End] end ("))) {
        snprintf(id, sizeof(id), "%.9s", p);
        managed_t *m = managed_by(id, NULL);
        if (m) {
            m->running = 0;
            runtime_publish(id, 0);
            if (g_deleting) {
                /* closed by a delete: take the folder away before the uninstall runs */
                unmount_title(m->tid, m->dlc_tids, m->ndlc_tids);
                printf("[delete] %s closed, game folder unmounted\n", m->tid);
            }
        }
    } else if ((p = after(line, "[PlayGoCore][Uninstall] end ("))) {
        snprintf(id, sizeof(id), "%.36s", p);
        managed_t *m = managed_by(NULL, id);
        if (m && delete_playgo_event_kind(g_deleting, 1, 0) == 2) {
            delete_pending_begin(m);
        } else if (strlen(id) == 36 && g_deleting) {
            /* an add-on of one of our games deleted on a delete screen: keep it out until the
             * drive is reconnected. Outside those screens the uninstall is the system's own:
             * registering a game again uninstalls its add-ons' PlayGo data, and taking that
             * for a delete removed the add-ons of the game every time a game was added. */
            for (int i = 0; i < MAX_TITLES; i++) {
                managed_t *g = &g_managed[i];
                for (int j = 0; g->used && j < g->ndlc_tids; j++) {
                    if (strncmp(id + 7, g->dlc_tids[j], 9))
                        continue;
                    /* the log line carries pkg-controlled metadata: only a plain
                     * content-id charset may reach a path (no '/', '\\', '.', '..') */
                    if (!delete_content_id_plain(id))
                        break;
                    char path[96], hold[PATH_MAX];
                    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", id);
                    write_file(path, id, strlen(id), 0666);
                    snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%.9s/%s", id + 7, id + 20);
                    rm_tree(hold);
                    printf("[delete] add-on %s removed\n", id);
                    i = MAX_TITLES;
                    break;
                }
            }
        }
        /* the delete is done: mount the other games again shortly after */
        if (g_deleting)
            g_deleting_close_at = time(NULL) + 3;
    }
}

/* --- web panel --- */

void app_rescan(void) { scan_request_why(1, SCAN_TRIG_MANUAL); }

/* Owner-side invalidation of a hard scan epoch: every pkg is read again, even when its file
 * kept the same size and time (a pkg replaced in place looks unchanged otherwise). */
static void rescan_invalidate(void) {
    pthread_mutex_lock(&g_titlecache_mu);
    title_cache_forget();
    pthread_mutex_unlock(&g_titlecache_mu);
    /* the updates left out are tried again, too */
    DIR *d = opendir(STATE_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (badpatch_name(e->d_name)) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), STATE_DIR "/%s", e->d_name);
            unlink(p);
        }
    }
    if (d)
        closedir(d);
}

/* The panel's own "scan for games" is an intent: the owner invalidates inside the scan epoch. */
void app_rescan_hard(void) { scan_request_hard(); }
void app_stop(void) { __atomic_store_n(&g_stop, 1, __ATOMIC_RELEASE); }

static void json_str(buf_t *b, const char *s) {
    buf_put(b, "\"", 1);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            buf_put(b, "\\", 1);
        if ((unsigned char)*s >= 0x20)
            buf_put(b, s, 1);
    }
    buf_put(b, "\"", 1);
}

/* Finds the drive tid is served from: the ps4/<TID> folder of older versions, or the
 * game the catalog found where it is. Panel workers use the completed-epoch presence
 * snapshot; owner actions keep the live catalog they already serialize with the scan. */
static int find_root_impl(const char *tid, char *root, size_t n, int presence_snapshot, const config_t *cfg) {
    for (int i = 0; i < NROOTS; i++) {
        char r[PATH_MAX], p[PATH_MAX];
        root_path(i, r, sizeof(r));
        int present = cfg ? root_present_cfg(r, cfg) : root_present(r);
        if (!present)
            continue;
        int inplace = cfg ? in_place_cfg(cfg) : in_place();
        if (inplace) {
            int found = presence_snapshot ? catalog_presence_has(r, tid) : 0;
            if (!presence_snapshot) {
                const catalog_t *c = drive_catalog(r);
                found = c && catalog_game(c, tid);
            }
            if (found) {
                snprintf(root, n, "%s", r);
                return 0;
            }
            continue;
        }
        snprintf(p, sizeof(p), "%s/ps4/%s/app.pkg", r, tid);
        if (exists(p)) {
            snprintf(root, n, "%s", r);
            return 0;
        }
    }
    return -1;
}

static int find_root(const char *tid, char *root, size_t n) { return find_root_impl(tid, root, n, 0, NULL); }

static int find_root_snapshot(const char *tid, char *root, size_t n, const config_t *cfg) {
    return find_root_impl(tid, root, n, 1, cfg);
}

/* `"notices":[...]`, newest first, capped at max entries. The whole array is copied under
 * the notification mutex and formatted after it, so a web worker never shares the live ring. */
static void notices_json(buf_t *out, int max) {
    notice_entry_t nsnap[NOTICE_MAX];
    int nn;
    pthread_mutex_lock(&g_notice_mu);
    nn = g_nnotices;
    memcpy(nsnap, g_notices, sizeof(notice_entry_t) * (size_t)nn);
    pthread_mutex_unlock(&g_notice_mu);
    if (nn > max)
        nn = max;
    buf_put(out, "\"notices\":[", 11);
    for (int i = 0; i < nn; i++) {
        buf_printf(out, "%s{\"ts\":%lld,\"kind\":\"%s\",\"text\":", i ? "," : "", (long long)nsnap[i].ts,
                   notify_kind_name(nsnap[i].kind));
        json_str(out, nsnap[i].text);
        buf_printf(out, "}");
    }
    buf_put(out, "]", 1);
}

/* `"activity":{...}` exactly as the library response exposes it, but without touching the
 * catalog or title cache: one mutex-protected snapshot of the owner state. */
static void activity_json(buf_t *out) {
    dlc_pending_t dpsnap[MAX_DLC_PENDING];
    activity_t act;
    int ndp;
    pthread_mutex_lock(&g_act_mu);
    act = g_activity;
    ndp = g_ndlc_pending;
    memcpy(dpsnap, g_dlc_pending, sizeof(dlc_pending_t) * (size_t)ndp);
    pthread_mutex_unlock(&g_act_mu);
    buf_printf(out, "\"activity\":{\"busy\":%s,\"phase\":\"%s\",\"drive\":", (act.phase[0] || ndp) ? "true" : "false",
               act.phase);
    json_str(out, act.drive);
    buf_printf(out, ",\"title\":");
    json_str(out, act.title);
    buf_printf(out, ",\"title_id\":");
    json_str(out, act.title_id);
    buf_printf(out, ",\"done\":%d,\"total\":%d,\"group\":%d,\"groups\":%d,\"dlc_pending\":[", act.done, act.total,
               act.group, act.groups);
    time_t now = time(NULL);
    for (int i = 0; i < ndp; i++) {
        buf_printf(out, "%s{\"content_id\":", i ? "," : "");
        json_str(out, dpsnap[i].content_id);
        buf_printf(out, ",\"name\":");
        json_str(out, dpsnap[i].name);
        buf_printf(out, ",\"size\":%llu,\"elapsed\":%lld}", (unsigned long long)dpsnap[i].size,
                   (long long)(now > dpsnap[i].since ? now - dpsnap[i].since : 0));
    }
    buf_printf(out, "]}");
}

/* The application-owned fields of /api/status. It is observational: unlike a library
 * request it does not mark the first setup as seen, and it never enumerates games. */
static void app_status_state_json(buf_t *out) {
    buf_printf(out, "\"setup\":%s,", setup_active() ? "true" : "false");
    coexist_json(out);
    buf_put(out, ",", 1);
    activity_json(out);
    buf_put(out, ",", 1);
    notices_json(out, 5);
}

void app_games_json(buf_t *out) {
    uint64_t t0 = mono_ms();
    api_games_metrics_t am;
    memset(&am, 0, sizeof(am));
    config_t cfg;
    cfg_snapshot(&cfg);
    if (setup_active())
        setup_seen_touch(); /* someone is at the panel: the defaults wait */
    buf_printf(out, "{\"lang\":\"%s\",\"setup\":%s,\"version\":", lang_tag(lang_current()), setup_active() ? "true" : "false");
    json_str(out, VERSION);
    /* the drives in use, with room left: where the pkgs are and what a send needs */
    buf_printf(out, ",\"drives\":[");
    int dfirst = 1;
    for (int i = 0; i < NROOTS; i++) {
        char root[PATH_MAX];
        root_path(i, root, sizeof(root));
        struct statfs sf;
        if (!root_present_cfg(root, &cfg) || statfs(root, &sf))
            continue;
        buf_printf(out, "%s{\"label\":", dfirst ? "" : ",");
        dfirst = 0;
        json_str(out, root_label(root));
        buf_printf(out, ",\"free\":%llu,\"total\":%llu}",
                   (unsigned long long)((uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize),
                   (unsigned long long)((uint64_t)sf.f_blocks * (uint64_t)sf.f_bsize));
    }
    buf_printf(out, "],\"games\":[");
    int first = 1, nlisted = 0, noffline = 0;
    char listed[MAX_TITLES][10];
    for (int i = 0; i < NROOTS; i++) {
        char root[PATH_MAX];
        root_path(i, root, sizeof(root));
        if (!root_present_cfg(root, &cfg))
            continue;
        catsnap_t *snap = in_place_cfg(&cfg) ? catalog_snapshot_acquire(root) : NULL;
        char tids[MAX_TITLES][10];
        int nt = titles_on_root_snapshot(root, tids, MAX_TITLES, snap, &cfg, &am.fp);
        for (int k = 0; k < nt; k++) {
            const char *tid = tids[k];
            title_info_copy_t tcopy;
            if (title_info_snapshot_copy(root, tid, &cfg, snap, &tcopy, &am.fp))
                continue;
            const title_info_t *t = &tcopy.info;
            const char *state, *mount = "";
            char target[64], from[PATH_MAX];
            snprintf(target, sizeof(target), "/user/app/%s", tid);
            int mounted = mounted_from(target, from, sizeof(from));
            if (mounted) {
                struct statfs sf;
                statfs(target, &sf);
                mount = sf.f_flags & MNT_RDONLY ? "ro" : "rw";
            }
            if (installed_internally(tid))
                state = "internal";
            else if (served_elsewhere(t))
                state = "duplicate";
            else if (removed(tid))
                state = "removed";
            else if (runtime_is_running(tid))
                state = "running";
            else if (mounted && db_is_registered(tid))
                state = needs_reboot(tid) ? "restart" : "ready";
            else
                state = "pending";
            if (nlisted < MAX_TITLES)
                snprintf(listed[nlisted++], 10, "%s", tid);
            buf_printf(out, "%s{\"title_id\":\"%s\",\"platform\":\"%s\",\"name\":", first ? "" : ",", tid,
                       game_platform(tid));
            json_str(out, t->name);
            buf_printf(out, ",\"version\":");
            json_str(out, t->ver);
            buf_printf(out, ",\"has_patch\":%s,\"size\":%llu,\"state\":\"%s\",\"mount\":\"%s\",\"source\":",
                       t->has_patch ? "true" : "false",
                       (unsigned long long)(t->app_size + (t->has_patch ? t->patch_size : 0)), state, mount);
            json_str(out, t->dir);
            buf_printf(out, ",\"dlc\":%d,\"update_issue\":", t->ndlc);
            json_str(out, t->update_issue);
            /* used where they are: nothing was moved or renamed, so there are no original
             * files to move back (also for games found in the old ps4/<TID> folders) */
            int inpl = !cfg.organize;
            buf_printf(out, ",\"in_place\":%s", inpl ? "true" : "false");
            char svt[16] = "";
            if (t->system_ver)
                ver_text(t->system_ver, svt, sizeof(svt));
            buf_printf(out, ",\"content_id\":");
            json_str(out, t->content_id);
            buf_printf(out, ",\"game_version\":");
            json_str(out, t->game_ver);
            buf_printf(out, ",\"system_ver\":");
            json_str(out, svt);
            buf_printf(out, ",\"storage\":");
            json_str(out, root_label(root));
            buf_printf(out, "}");
            first = 0;
        }
        catsnap_unref(snap);
    }
    /* games of drives that are not connected: still on the home screen, not playable */
    DIR *sd = opendir(STATE_DIR);
    struct dirent *e;
    while (sd && (e = readdir(sd))) {
        size_t n = strlen(e->d_name);
        if (n != 13 || strcmp(e->d_name + 9, ".src") || !game_id_at(e->d_name))
            continue;
        char tid[10], dir[PATH_MAX], drive[32], name[128];
        snprintf(tid, sizeof(tid), "%.9s", e->d_name);
        int seen = 0;
        for (int i = 0; i < nlisted && !seen; i++)
            seen = !strcmp(listed[i], tid);
        if (seen || !offline_game(tid, dir, sizeof(dir), drive, sizeof(drive), name, sizeof(name)))
            continue;
        buf_printf(out, "%s{\"title_id\":\"%s\",\"platform\":\"%s\",\"name\":", first ? "" : ",", tid,
                   game_platform(tid));
        json_str(out, name);
        buf_printf(out, ",\"version\":\"\",\"has_patch\":false,\"size\":0,\"state\":\"offline\",\"mount\":\"\","
                        "\"source\":");
        json_str(out, dir);
        buf_printf(out, ",\"dlc\":0,\"update_issue\":\"\",\"in_place\":false,\"content_id\":\"\",\"game_version\":\"\","
                        "\"system_ver\":\"\",\"storage\":");
        json_str(out, drive);
        buf_printf(out, "}");
        first = 0;
        noffline++;
    }
    if (sd)
        closedir(sd);
    buf_printf(out, "],\"dlc_mode\":\"%s\"", dlc_mode_name(cfg.dlc_mode));
    /* the last notifications the console was sent, newest first */
    buf_put(out, ",", 1);
    notices_json(out, NOTICE_MAX);
    /* what the daemon is doing now, and the add-ons the system is still installing */
    buf_put(out, ",", 1);
    activity_json(out);
    buf_put(out, ",", 1);
    coexist_json(out);
    buf_put(out, "}", 1);
    am.titles = (uint32_t)(nlisted + noffline);
    am.wall_ms = mono_ms() - t0;
    char tline[256];
    api_games_metrics_format(&am, tline, sizeof(tline));
    printf("%s\n", tline);
}

static int app_art(const char *tid, const char *name, uint32_t id, buf_t *out);

int app_icon(const char *tid, buf_t *out) { return app_art(tid, "icon0.png", 0x1200, out); }

int app_pic(const char *tid, buf_t *out) { return app_art(tid, "pic0.png", 0x1220, out); }

/* Artwork of a game: the copy written into /user/appmeta when it was added, or the entry
 * id of its pkgs (the update's first: its artwork wins, as with a real install). */
static int app_art(const char *tid, const char *name, uint32_t id, buf_t *out) {
    char p[PATH_MAX], root[PATH_MAX];
    snprintf(p, sizeof(p), "/user/appmeta/%s/%s", tid, name);
    pthread_mutex_lock(&g_meta_file_mu);
    int fd = open(p, O_RDONLY);
    if (fd >= 0) {
        char tmp[65536];
        ssize_t n;
        while ((n = read(fd, tmp, sizeof(tmp))) > 0)
            buf_put(out, tmp, (size_t)n);
        close(fd);
    }
    pthread_mutex_unlock(&g_meta_file_mu);
    if (fd >= 0)
        return out->len ? 0 : -1;
    config_t cfg;
    cfg_snapshot(&cfg);
    if (find_root_snapshot(tid, root, sizeof(root), &cfg))
        return -1;
    char patch[PATH_MAX], app[PATH_MAX];
    if (title_pkgs_snapshot(root, tid, &cfg, patch, sizeof(patch), app, sizeof(app)))
        return -1;
    const char *files[] = {patch, app}; /* the update's artwork wins, as with a real install */
    for (int i = 0; i < 2; i++) {
        if (!files[i][0])
            continue;
        pkg_t *pkg = mem_alloc(sizeof(*pkg));
        if (!pkg || pkg_open(pkg, files[i])) {
            mem_free(pkg);
            continue;
        }
        size_t len;
        uint8_t *png = pkg_entry(pkg, id, &len);
        pkg_close(pkg);
        mem_free(pkg);
        if (png) {
            buf_put(out, png, len);
            mem_free(png);
            return 0;
        }
    }
    return -1;
}

int app_add(const char *tid, char *msg, size_t cap) {
    char rm[64], root[PATH_MAX];
    if (find_root(tid, root, sizeof(root))) {
        snprintf(msg, cap, TR("%s is not on any connected drive", "%s no está en ninguna unidad conectada"), tid);
        return -1;
    }
    snprintf(rm, sizeof(rm), STATE_DIR "/%s.removed", tid);
    unlink(rm);
    clear_removed_dlc_for(tid);
    scan_request_why(0, SCAN_TRIG_ADD);
    snprintf(msg, cap, TR("Adding %s to the library", "Añadiendo %s a la biblioteca"), tid);
    return 0;
}

/* sceAppInstUtilAppUnInstall in its own thread, waited for at most UNINSTALL_WAIT seconds:
 * a call that never returns (as InstallAll did after rest mode) must not freeze PS4Mount.
 * The wait does not serve the panel (this also runs from a panel request). */
#define UNINSTALL_WAIT 60

static int g_uninstall_busy, g_uninstall_rc;
static char g_uninstall_tid[10];

static void *uninstall_thread(void *arg) {
    (void)arg;
    __atomic_store_n(&g_uninstall_rc, g_uninstall_call(g_uninstall_tid), __ATOMIC_RELAXED);
    __atomic_store_n(&g_uninstall_busy, 0, __ATOMIC_RELEASE); /* result before completion */
    return NULL;
}

/* 0 or the system error; -2 while an earlier call is still running or this one timed out. */
static int uninstall_title(const char *tid) {
    if (!g_uninstall_call) {
        printf("[remove] %s: sceAppInstUtilAppUnInstall is not in this firmware\n", tid);
        return -3;
    }
    if (__atomic_load_n(&g_uninstall_busy, __ATOMIC_ACQUIRE)) {
        printf("[remove] %s: an earlier uninstall (%s) is still running\n", tid, g_uninstall_tid);
        return -2;
    }
    snprintf(g_uninstall_tid, sizeof(g_uninstall_tid), "%s", tid);
    __atomic_store_n(&g_uninstall_busy, 1, __ATOMIC_RELEASE);
    pthread_t th;
    if (pthread_create(&th, NULL, uninstall_thread, NULL)) {
        uninstall_thread(NULL);
        return __atomic_load_n(&g_uninstall_rc, __ATOMIC_ACQUIRE);
    }
    pthread_detach(th);
    for (int i = 0; i < UNINSTALL_WAIT * 10 && __atomic_load_n(&g_uninstall_busy, __ATOMIC_ACQUIRE); i++)
        usleep(100000);
    if (__atomic_load_n(&g_uninstall_busy, __ATOMIC_ACQUIRE)) {
        printf("[remove] %s: uninstall did not return within %d s\n", tid, UNINSTALL_WAIT);
        return -2;
    }
    return __atomic_load_n(&g_uninstall_rc, __ATOMIC_ACQUIRE);
}

static int folder_dlc_tids(const char *game, char (*tids)[10], int max);

/* Add-on mount targets known for a removal, even when the managed record is absent after
 * a restart. In-place catalogs name add-ons wherever they are; organized folders keep them
 * under the game's own addcont directory. */
static int removal_dlc_tids(const char *root, const char *tid, const managed_t *m, char (*out)[10]) {
    if (m) {
        memcpy(out, m->dlc_tids, sizeof(m->dlc_tids));
        return m->ndlc_tids;
    }
    if (in_place()) {
        const catalog_t *c = drive_catalog(root);
        const cat_game_t *g = c ? catalog_game(c, tid) : NULL;
        const cat_pkg_t *list[CAT_MAX_DLC];
        int nlist = g ? catalog_dlc_of(g_cat, c, g, list, CAT_MAX_DLC) : 0, n = 0;
        for (int i = 0; i < nlist && n < MAX_DLC_TIDS; i++) {
            int known = 0;
            for (int k = 0; k < n; k++)
                known |= !strcmp(out[k], list[i]->title_id);
            if (!known)
                snprintf(out[n++], 10, "%s", list[i]->title_id);
        }
        return n;
    }
    char game[PATH_MAX];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    return folder_dlc_tids(game, out, MAX_DLC_TIDS);
}

int app_remove(const char *tid, char *msg, size_t cap) {
    managed_t *m = managed_by(tid, NULL);
    char root[PATH_MAX], srcmark[64];
    snprintf(srcmark, sizeof(srcmark), STATE_DIR "/%s.src", tid);
    if (!remove_owned_ok(installed_internally(tid), m != NULL, exists(srcmark), !find_root(tid, root, sizeof(root)))) {
        printf("[web] remove %s refused: no Lodestar ownership evidence\n", tid);
        snprintf(msg, cap, TR("%s is not managed by Lodestar", "%s no está gestionado por Lodestar"), tid);
        return -1;
    }
    if ((m && m->running) || title_running(tid)) {
        printf("[web] remove %s refused: game running\n", tid);
        snprintf(msg, cap, "%s", TR("Close the game before removing it", "Cierra el juego antes de quitarlo"));
        return -1;
    }
    char dtids[MAX_DLC_TIDS][10];
    int ndtids = removal_dlc_tids(root, tid, m, dtids);
    unmount_title(tid, dtids, ndtids);
    if (!title_detached(tid, dtids, ndtids)) {
        /* never let the system uninstall run while the USB content is reachable */
        snprintf(msg, cap, TR("Could not unmount %s; nothing was removed", "No se pudo desmontar %s; no se quitó nada"), tid);
        return -1;
    }
    char path[64], srcpath[64];
    snprintf(srcpath, sizeof(srcpath), STATE_DIR "/%s.src", tid);
    size_t srclen = 0;
    uint8_t *savedsrc = read_all(srcpath, &srclen);
    snprintf(path, sizeof(path), STATE_DIR "/%s.removed", tid);
    write_file(path, tid, strlen(tid), 0666);
    unlink(srcpath);
    int rc = db_is_registered(tid) ? uninstall_title(tid) : 0;
    if (!rc) {
        clean_holds(tid, dtids, ndtids);
        /* the task goes with it: BGFT keeps the state of an id for the whole boot, and a
         * game added again under the same one could not start */
        bgft_remove(tid);
    } else {
        /* a failed uninstall is not a removal: put back the scan ownership evidence */
        unlink(path);
        if (savedsrc && write_file(srcpath, savedsrc, srclen, 0666))
            printf("[web] remove %s: its source state could not be restored\n", tid);
    }
    mem_free(savedsrc);
    if (!rc && m)
        managed_mark_unused(m);
    printf("[web] remove %s: uninstall=0x%08x\n", tid, rc);
    if (rc == -2)
        snprintf(msg, cap,
                 TR("The system did not finish removing %s yet; try again in a moment",
                    "El sistema aún no terminó de quitar %s; inténtalo de nuevo en un momento"),
                 tid);
    else
        snprintf(msg, cap,
                 rc ? TR("Failed to remove %s (0x%08x)", "No se pudo quitar %s (0x%08x)")
                    : TR("%s removed from the library. Its pkgs stay where they are.",
                         "%s quitado de la biblioteca. Sus pkgs se quedan donde están."),
                 tid, rc);
    return rc ? -1 : 0;
}

/* Removes a game refused with "system file disagree" from the library and adds it again. */
static void readd_title(const char *tid) {
    char msg[256], name[128];
    snprintf(g_readd_active, sizeof(g_readd_active), "%s", tid);
    managed_t *m = managed_by(tid, NULL);
    snprintf(name, sizeof(name), "%s", m ? m->name : tid);
    if (g_nreadd_done < MAX_TITLES)
        snprintf(g_readd_done[g_nreadd_done++], 10, "%s", tid);
    pump_events(5000); /* let the system finish the refused launch */
    notify(TR("%s (%s): adding it to the library again", "%s (%s): volviendo a añadirlo a la biblioteca"), name,
           g_readd_why);
    int rc = app_remove(tid, msg, sizeof(msg));
    printf("[reg] %s: removed to add it again: %s\n", tid, msg);
    if (g_readd_badpatch) {
        /* the removal cleared it: the game comes back without the update */
        g_readd_badpatch = 0;
        write_badpatch(tid, g_readd_patch);
        g_readd_patch[0] = 0;
    }
    if (!rc)
        wait_db_quiet(3, 30);
    app_add(tid, msg, sizeof(msg));
    g_readd_active[0] = 0;
}

/* --- moving pkgs back where they came from --- */

static void clean_name(char *s) {
    for (; *s; s++)
        if (strchr("\\:*?\"<>|", *s) || (unsigned char)*s < 0x20)
            *s = '_';
}

/* Moves <game_dir>/<rel> to its recorded original path (or fallback, relative to the
 * drive root, for pkgs moved before origins were recorded) and adds it to the list of
 * restored pkgs when remember is set (moved back on request: the scan leaves them alone).
 * 1 moved, 0 nothing there, -1 failed. */
static int restore_pkg(const char *root, const char *game_dir, const char *rel, const char *fallback, int remember) {
    char src[PATH_MAX], orig[PATH_MAX], dst[PATH_MAX], dir[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s", game_dir, rel);
    struct stat st;
    if (stat(src, &st))
        return 0;
    if (origin_find(game_dir, rel, orig, sizeof(orig))) {
        snprintf(orig, sizeof(orig), "%s", fallback);
        clean_name(orig);
    }
    if (!restore_orig_path_ok(orig) || snprintf(dir, sizeof(dir), "%s/%s", root, orig) >= (int)sizeof(dir)) {
        printf("[restore] %s: unsafe original path %s\n", src, orig);
        return -1;
    }
    for (char *slash = strchr(dir + strlen(root) + 1, '/'); slash; slash = strchr(slash + 1, '/')) {
        *slash = 0;
        struct stat parent;
        if (lstat(dir, &parent) ? errno != ENOENT || mkdir(dir, 0777) : !S_ISDIR(parent.st_mode)) {
            printf("[restore] %s: could not use the original folder %s\n", src, dir);
            return -1;
        }
        *slash = '/';
    }
    if (origin_free_path(root, orig, dst, sizeof(dst)) || rename(src, dst)) {
        printf("[restore] %s: %s\n", src, strerror(errno));
        return -1;
    }
    if (remember)
        restored_add(RESTORED_LIST, drive_rel(root, dst), (uint64_t)st.st_size);
    printf("[restore] %s -> %s\n", src, dst);
    return 1;
}

static void unlink_in(const char *dir, const char *const *names) {
    char p[PATH_MAX];
    for (; *names; names++) {
        snprintf(p, sizeof(p), "%s/%s", dir, *names);
        unlink(p);
    }
}

/* Moves every pkg of one game folder back and removes what PS4Mount created for it.
 * remember: they were moved back on request, so the scan leaves them alone; clear for
 * the move back that organize off does on its own, whose pkgs are used where they are.
 * Folders are only removed when empty, so nothing else is ever deleted. */
static void restore_files(const char *root, const char *tid, const char *name, int *moved, int *failed, int remember) {
    static const char *const generated[] = {"app.json", "app.pbm", "app.pbm.backup", "app.xml", "patch.json",
                                            "patch.pbm", "patch.pbm.backup", "patch.xml", NULL};
    static const char *const ac_generated[] = {"ac.json", "ac.pbm", "ac.pbm.backup", NULL};
    char game[PATH_MAX], rel[PATH_MAX], fb[PATH_MAX], sub[PATH_MAX];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    int rc;
#define RESTORE(r, f)                                                                                                  \
    do {                                                                                                               \
        rc = restore_pkg(root, game, r, f, remember);                                                                  \
        *moved += rc > 0;                                                                                              \
        *failed += rc < 0;                                                                                             \
    } while (0)
    snprintf(fb, sizeof(fb), "%s - %s - game.pkg", tid, name);
    RESTORE("app.pkg", fb);
    snprintf(fb, sizeof(fb), "%s - %s - update.pkg", tid, name);
    RESTORE("patch.pkg", fb);
    snprintf(sub, sizeof(sub), "%s/replaced", game);
    DIR *d = opendir(sub);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(rel, sizeof(rel), "replaced/%s", e->d_name);
        snprintf(fb, sizeof(fb), "%s - %s - older update %s", tid, name, e->d_name);
        RESTORE(rel, fb);
    }
    if (d)
        closedir(d);
    rmdir(sub);
    snprintf(sub, sizeof(sub), "%s/addcont", game);
    d = opendir(sub);
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char tdir[PATH_MAX], hold[PATH_MAX];
        snprintf(tdir, sizeof(tdir), "%s/%s", sub, e->d_name);
        DIR *l = opendir(tdir);
        struct dirent *f;
        while (l && (f = readdir(l))) {
            if (f->d_name[0] == '.')
                continue;
            char ldir[PATH_MAX + 64];
            snprintf(ldir, sizeof(ldir), "%s/%s", tdir, f->d_name);
            snprintf(rel, sizeof(rel), "addcont/%s/%s/ac.pkg", e->d_name, f->d_name);
            snprintf(fb, sizeof(fb), "%s - %s - %s.pkg", e->d_name, name, f->d_name);
            RESTORE(rel, fb);
            unlink_in(ldir, ac_generated);
            rmdir(ldir);
            snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%s/%s", e->d_name, f->d_name);
            rm_tree(hold);
        }
        if (l)
            closedir(l);
        rmdir(tdir);
        snprintf(hold, sizeof(hold), HOLD_DIR "/addcont/%s", e->d_name);
        rmdir(hold); /* kept while other games still use add-ons of this title */
    }
    if (d)
        closedir(d);
#undef RESTORE
    rmdir(sub);
    unlink_in(game, generated); /* written next to the pkgs by versions before 0.3.2 */
    char marker[PATH_MAX];
    snprintf(marker, sizeof(marker), "%s/" ORIGIN_FILE, game);
    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/app.pkg", game);
    if (!exists(probe)) /* keep the record while a pkg could not be moved */
        unlink(marker);
    rmdir(game);
    snprintf(probe, sizeof(probe), HOLD_DIR "/app/%s", tid);
    rm_tree(probe);
    snprintf(probe, sizeof(probe), HOLD_DIR "/patch/%s", tid);
    rm_tree(probe);
}

/* Add-on title IDs of a game folder (its /user/addcont mounts). */
static int folder_dlc_tids(const char *game, char (*tids)[10], int max) {
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/addcont", game);
    DIR *d = opendir(sub);
    int n = 0;
    struct dirent *e;
    while (d && n < max && (e = readdir(d)))
        if (is_game_id(e->d_name))
            snprintf(tids[n++], 10, "%s", e->d_name);
    if (d)
        closedir(d);
    return n;
}

/* Takes one game out of the library (through the system) and unmounts it. */
static int restore_unregister(const char *root, const char *tid) {
    char game[PATH_MAX], dtids[MAX_DLC_TIDS][10];
    snprintf(game, sizeof(game), "%s/ps4/%s", root, tid);
    int nd = folder_dlc_tids(game, dtids, MAX_DLC_TIDS);
    unmount_title(tid, dtids, nd);
    if (!title_detached(tid, dtids, nd)) {
        printf("[restore] %s kept: still mounted\n", tid);
        return -1;
    }
    char msg[256];
    if (!installed_internally(tid) && app_remove(tid, msg, sizeof(msg))) {
        printf("[restore] %s kept: %s\n", tid, msg);
        return -1;
    }
    return 0;
}

static void restore_clear_state(const char *tid) {
    char p[64];
    snprintf(p, sizeof(p), STATE_DIR "/%s.removed", tid);
    unlink(p);
    clear_removed_dlc_for(tid);
    snprintf(p, sizeof(p), STATE_DIR "/%s.src", tid);
    unlink(p);
    retry_clear(tid);
    bgft_remove(tid);
}

/* Requests from the panel, run by the main loop: some titles, or all of them. */
static char g_restore_tids[MAX_TITLES][10];
static int g_restore_n, g_restore_all;

/* organize off: the games of each drive PS4Mount had moved into ps4/ are sent back to
 * where they were once (and then used there). */
static int g_migrated[NROOTS];

/* --- closing the open game --- */

typedef struct {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[10];
    uint32_t app_type;
    char unknown2[0x36];
} app_info_t;

int sceKernelGetAppInfo(pid_t pid, app_info_t *info);
int sceSystemServiceKillApp(uint32_t app_id, int how, int reason, int core_dump);

/* The panel app (panel-app/): a home screen tile that shows the panel. It is no game. */
#define PANEL_APP_TID "LODS00001"

/* Open apps: processes named eboot.bin (PS4 and PS5 games alike), the games or only the
 * panel app. Fills up to max app ids and title ids, returns how many are open. */
static int open_apps(uint32_t *ids, char (*tids)[10], int max, int panel) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0}, n = 0;
    size_t len = 0;
    if (sysctl(mib, 4, NULL, &len, NULL, 0) || !len)
        return 0;
    len += len / 4; /* processes started meanwhile */
    uint8_t *buf = mem_alloc(len); /* ~100 KB: off the libc heap */
    if (!buf || sysctl(mib, 4, buf, &len, NULL, 0)) {
        mem_free(buf);
        return 0;
    }
    for (size_t off = 0; off + sizeof(struct kinfo_proc) <= len;) {
        struct kinfo_proc *ki = (struct kinfo_proc *)(buf + off);
        if (ki->ki_structsize < (int)sizeof(*ki) || (size_t)ki->ki_structsize > len - off)
            break;
        app_info_t info;
        memset(&info, 0, sizeof(info));
        if (!strcmp(ki->ki_comm, "eboot.bin") && !sceKernelGetAppInfo(ki->ki_pid, &info) && info.app_id &&
            (strncmp(info.title_id, PANEL_APP_TID, 9) == 0) == panel) {
            if (n < max) {
                ids[n] = info.app_id;
                snprintf(tids[n], 10, "%.9s", info.title_id);
            }
            n++;
        }
        off += (size_t)ki->ki_structsize;
    }
    mem_free(buf);
    return n;
}

static int open_games(uint32_t *ids, char (*tids)[10], int max) {
    return open_apps(ids, tids, max, 0);
}

/* Back on the panel's first page inside the panel app: the app cannot close itself while
 * it shows the page. */
void app_panel_close(void) {
    uint32_t ids[2];
    char tids[2][10];
    int n = open_apps(ids, tids, 2, 1);
    for (int i = 0; i < n && i < 2; i++)
        printf("[main] panel app closed: 0x%x\n", (unsigned)sceSystemServiceKillApp(ids[i], -1, 0, 0));
}

/* Every pkg operation that takes games out of the library closes the open game first,
 * whichever it is: a game left open keeps reading pkgs that are being moved. 0 once no
 * game is open; -1 when one did not close within 30 seconds. */
static int close_open_games(void) {
    uint32_t ids[8];
    char tids[8][10];
    int n = open_games(ids, tids, 8);
    if (!n)
        return 0;
    for (int i = 0; i < n && i < 8; i++) {
        managed_t *m = managed_by(tids[i], NULL);
        notify(TR("Closing %s to restore its original files", "Cerrando %s para restaurar sus archivos originales"), m ? m->name : tids[i]);
        int rc = sceSystemServiceKillApp(ids[i], -1, 0, 0);
        printf("[restore] close %s (app 0x%08x): 0x%08x\n", tids[i], ids[i], rc);
    }
    char closing[8][10];
    int nc = n < 8 ? n : 8;
    memcpy(closing, tids, sizeof(closing[0]) * (size_t)nc);
    for (int s = 0; s < 30; s++) {
        pump_events(1000);
        int mounted = 0;
        for (int i = 0; i < nc; i++)
            mounted |= title_running(closing[i]);
        if (!open_games(ids, tids, 8) && !mounted) {
            pump_events(2000); /* let the system finish unmounting the game */
            for (int i = 0; i < MAX_TITLES; i++)
                g_managed[i].running = 0;
            runtime_clear();
            return 0;
        }
    }
    return -1;
}

/* Folders of games on every connected drive (or only tid), with their name. */
typedef struct {
    char root[PATH_MAX], tid[10], name[128];
} restore_item_t;

/* only: n title ids to keep, or NULL for every game. */
static int restore_collect(char (*only)[10], int nonly, restore_item_t *items, int max) {
    int n = 0;
    for (int i = 0; i < NROOTS && n < max; i++) {
        char root[PATH_MAX], lib[PATH_MAX];
        root_path(i, root, sizeof(root));
        if (!root_present(root))
            continue;
        snprintf(lib, sizeof(lib), "%s/ps4", root);
        DIR *d = opendir(lib);
        struct dirent *e;
        while (d && n < max && (e = readdir(d))) {
            if (!is_game_id(e->d_name))
                continue;
            int wanted = !only;
            for (int k = 0; k < nonly && !wanted; k++)
                wanted = !strcmp(only[k], e->d_name);
            if (!wanted)
                continue;
            restore_item_t *it = &items[n++];
            snprintf(it->root, sizeof(it->root), "%s", root);
            snprintf(it->tid, sizeof(it->tid), "%s", e->d_name);
            snprintf(it->name, sizeof(it->name), "%s", e->d_name);
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/%s/app.pkg", lib, e->d_name);
            pkg_t pkg;
            sfo_t sfo;
            if (exists(p) && !pkg_open(&pkg, p)) {
                const sfo_item_t *t = read_sfo(&pkg, &sfo) ? NULL : sfo_get(&sfo, "TITLE");
                if (t) {
                    snprintf(it->name, sizeof(it->name), "%s", t->sval);
                    clean_name(it->name);
                }
                pkg_close(&pkg);
            }
        }
        if (d)
            closedir(d);
    }
    return n;
}

/* Moves the pkgs of the requested games (only, or all when NULL) back, in groups of the
 * restore setting: the games of a group leave the library one at a time, their pkgs are
 * moved, and the next group waits until the system databases are idle. remember: see
 * restore_files (0 for the move back organize off does by itself). */
static void run_restore(char (*only)[10], int nonly, int remember) {
    static restore_item_t items[MAX_TITLES];
    static unsigned char unregistered[MAX_TITLES], move_failed[MAX_TITLES];
    memset(unregistered, 0, sizeof(unregistered));
    memset(move_failed, 0, sizeof(move_failed));
    int n = restore_collect(only, nonly, items, MAX_TITLES), moved = 0, failed = 0, done = 0, kept = 0;
    if (!n) {
        notify("%s", TR("No Lodestar games to restore", "No hay juegos de Lodestar que restaurar"));
        return;
    }
    if (close_open_games()) {
        notify_kind(NOTIFY_WARN, "%s",
                    TR("The open game did not close, so no files were restored. Close it and try again",
                                           "El juego abierto no se cerró y no se restauró ningún archivo. Ciérralo e inténtalo otra vez"));
        return;
    }
    int size = g_cfg.restore_batch;
    activity_set("restoring", NULL, NULL, NULL, 0, n, 0, 0);
    if (n == 1)
        notify(TR("Restoring the original files of %s...", "Restaurando los archivos originales de %s..."), items[0].name);
    else
        notify(TR("Restoring the original files of %d game(s), %d at a time...",
                  "Restaurando los archivos originales de %d juego(s), en grupos de %d..."),
               n, size);
    for (int i = 0; i < n; i += size) {
        if (i)
            wait_db_quiet(BATCH_QUIET, 120);
        int end = i + size < n ? i + size : n;
        for (int k = i; k < end; k++) {
            activity_set("restoring", root_label(items[k].root), items[k].name, items[k].tid, done, n, 0, 0);
            if (restore_unregister(items[k].root, items[k].tid)) {
                kept++;
                continue;
            }
            unregistered[k] = 1;
            /* one removal at a time: the next waits until the system databases are idle */
            wait_db_quiet(3, 30);
            int failed_before = failed;
            restore_files(items[k].root, items[k].tid, items[k].name, &moved, &failed, remember);
            move_failed[k] = failed > failed_before;
            done++;
        }
    }
    /* the uninstall events arrive late and mark the games as removed: clear that, so a
     * game added again later is not taken for one removed on the console */
    pump_events(3000);
    for (int i = 0; i < n; i++)
        if (restore_clear_state_ok(unregistered[i], move_failed[i]))
            restore_clear_state(items[i].tid);
    scan_request_why(0, SCAN_TRIG_RESTORE);
    if (n == 1 && done)
        notify_kind(NOTIFY_READY, TR("%s: %d pkg file(s) restored%s", "%s: %d pkg(s) restaurado(s)%s"), items[0].name,
                    moved,
                    failed ? TR(", some could not be moved (see the log)", ", algunos no se pudieron mover (ver el log)")
                           : "");
    else if (n > 1)
        notify_kind(NOTIFY_READY,
                    TR("%d pkg file(s) of %d game(s) restored%s%s", "%d pkg(s) de %d juego(s) restaurado(s)%s%s"), moved,
                    done,
                    failed ? TR(", some could not be moved (see the log)", ", algunos no se pudieron mover (ver el log)")
                           : "",
                    kept   ? TR(", some games could not leave the library (see the log)",
                                 ", algunos juegos no pudieron salir de la biblioteca (ver el log)")
                           : "");
    activity_clear();
}

/* Games on root that PS4Mount moved into ps4/<TITLE_ID>/: their app.pkg has a recorded
 * origin, so they go back to where they were and are used there. Games removed on the
 * console and titles installed internally are left alone. */
static int organized_on_root(const char *root, char (*tids)[10], int max) {
    char lib[PATH_MAX];
    snprintf(lib, sizeof(lib), "%s/ps4", root);
    DIR *d = opendir(lib);
    if (!d)
        return 0;
    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d))) {
        if (!is_game_id(e->d_name) || removed(e->d_name) ||
            installed_internally(e->d_name))
            continue;
        char game[PATH_MAX], app[PATH_MAX + 8], orig[PATH_MAX];
        snprintf(game, sizeof(game), "%s/%s", lib, e->d_name);
        snprintf(app, sizeof(app), "%s/app.pkg", game);
        if (exists(app) && !origin_find(game, "app.pkg", orig, sizeof(orig)))
            snprintf(tids[n++], 10, "%s", e->d_name);
    }
    closedir(d);
    return n;
}

/* organize off: gives back the pkgs of the games PS4Mount had organized, once per drive.
 * A game left open is not closed behind the user's back: the next scan tries again. */
static void migrate_organized(void) {
    if (g_cfg.organize)
        return;
    int pending = 0;
    for (int i = 0; i < NROOTS && !pending; i++) {
        char root[PATH_MAX];
        root_path(i, root, sizeof(root));
        pending = !g_migrated[i] && root_present(root);
    }
    if (!pending)
        return;
    uint32_t ids[8];
    char opened[8][10];
    if (open_games(ids, opened, 8))
        return;
    char tids[MAX_TITLES][10];
    int n = 0;
    for (int i = 0; i < NROOTS && n < MAX_TITLES; i++) {
        if (g_migrated[i])
            continue;
        char root[PATH_MAX];
        root_path(i, root, sizeof(root));
        if (!root_present(root))
            continue; /* plugged in later: tried then */
        g_migrated[i] = 1;
        n += organized_on_root(root, tids + n, MAX_TITLES - n);
    }
    if (!n)
        return;
    notify(TR("Organize is off: moving the pkgs of %d game(s) back where they were",
              "Organizar está apagado: devolviendo los pkgs de %d juego(s) a donde estaban"),
           n);
    run_restore(tids, n, 0);
}

int app_restore(const char *tids, char *msg, size_t cap) {
    if (!g_cfg.organize) {
        /* Nothing was moved or renamed: the pkgs are the user's, where they are. */
        snprintf(msg, cap, "%s",
                 TR("Pkgs are used where they are, with their own names: there are no original files to restore",
                    "Los pkg se usan donde están, con sus propios nombres: no hay archivos originales que restaurar"));
        return -1;
    }
    if (g_restore_all || g_restore_n) {
        snprintf(msg, cap, "%s", TR("Already restoring files, wait for it to finish", "Ya se están restaurando archivos, espera a que termine"));
        return -1;
    }
    if (!tids) {
        g_restore_all = 1;
        snprintf(msg, cap, "%s",
                 TR("Closing the open game and restoring the original files of all games",
                    "Cerrando el juego abierto y restaurando los archivos originales de todos los juegos"));
        return 0;
    }
    int n = 0;
    for (const char *p = tids; *p;) {
        size_t len = strcspn(p, ",");
        char tid[10], root[PATH_MAX];
        if (len != 9 || !game_id_at(p)) {
            snprintf(msg, cap, "invalid title id: %.20s", p);
            return -1;
        }
        snprintf(tid, sizeof(tid), "%.9s", p);
        if (find_root(tid, root, sizeof(root))) {
            snprintf(msg, cap, TR("%s is not on any connected drive", "%s no está en ninguna unidad conectada"), tid);
            return -1;
        }
        int dup = 0;
        for (int i = 0; i < n; i++)
            dup |= !strcmp(g_restore_tids[i], tid);
        if (!dup && n < MAX_TITLES)
            snprintf(g_restore_tids[n++], 10, "%s", tid);
        p += len;
        if (*p == ',')
            p++;
    }
    if (!n) {
        snprintf(msg, cap, "%s", TR("No games selected", "No hay juegos seleccionados"));
        return -1;
    }
    g_restore_n = n;
    if (n == 1)
        snprintf(msg, cap,
                 TR("Closing the open game and restoring the original files of %s",
                    "Cerrando el juego abierto y restaurando los archivos originales de %s"),
                 g_restore_tids[0]);
    else
        snprintf(msg, cap,
                 TR("Closing the open game and restoring the original files of %d games, %d at a time",
                    "Cerrando el juego abierto y restaurando los archivos originales de %d juego(s), en grupos de %d"),
                 n, g_cfg.restore_batch);
    return 0;
}

int app_restored_forget(char *msg, size_t cap) {
    unlink(RESTORED_LIST);
    scan_request_why(0, SCAN_TRIG_RESTORE);
    snprintf(msg, cap, "%s", TR("Restored pkgs will be used again on the next scan", "Los pkgs restaurados se volverán a usar en la próxima búsqueda"));
    return 0;
}

/* --- settings --- */

/* A scan folder: relative to the drive, "" for the root. Folders inside ps4/ (where
 * PS4Mount keeps the games) and anything leaving the drive are refused. */
static int clean_scan_path(const char *in, char *out, size_t n, char *why, size_t wn) {
    while (*in == ' ' || *in == '/')
        in++;
    snprintf(out, n, "%s", in);
    size_t l = strlen(out);
    while (l && (out[l - 1] == ' ' || out[l - 1] == '/' || out[l - 1] == '\r'))
        out[--l] = 0;
    if (l >= 120) {
        snprintf(why, wn, TR("folder name too long: %.40s...", "nombre de carpeta demasiado largo: %.40s..."), out);
        return -1;
    }
    if (strchr(out, '\\') || strchr(out, ':') || strstr(out, "//")) {
        snprintf(why, wn, TR("invalid folder: %s", "carpeta no válida: %s"), out);
        return -1;
    }
    for (const char *c = out; *c; c = strchr(c, '/') ? strchr(c, '/') + 1 : c + strlen(c))
        if (!strncmp(c, "..", 2) && (c[2] == '/' || !c[2])) {
            snprintf(why, wn, TR("folders outside the drive are not allowed: %s", "no se permiten carpetas fuera de la unidad: %s"), out);
            return -1;
        }
    if (!strncasecmp(out, "ps4", 3) && (!out[3] || out[3] == '/')) {
        snprintf(why, wn, "%s",
                 TR("the ps4 folder holds the games Lodestar already moved; choose another folder",
                    "la carpeta ps4 guarda los juegos que Lodestar ya movió; elige otra carpeta"));
        return -1;
    }
    return 0;
}

/* A first start: nothing of PS4Mount on the console yet. No game is added until the settings
 * are saved once, so a drive plugged in already is not added whole with the defaults. A
 * console that ran an older version without ever saving them has catalogs or games. */
static int first_start(void) {
    if (exists(CONFIG_PATH))
        return 0;
    DIR *d = opendir(STATE_DIR);
    struct dirent *e;
    int found = 0;
    while (d && !found && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        found = !strncmp(e->d_name, "catalog-", 8) || (n > 4 && !strcmp(e->d_name + n - 4, ".src"));
    }
    if (d)
        closedir(d);
    return !found;
}

static void config_save(void) {
    setup_set(0); /* the settings were chosen: games are added from now on */
    buf_t b = {0};
    buf_printf(&b, "ver=3\nbatch=%d\nbatch_size=%d\ndlc=%d\ndlc_mode=%s\ndlc_big=%d\nrestore_batch=%d\ninternal=%d\norganize=%d\nlang=%s\n",
               g_cfg.batch, g_cfg.batch_size, g_cfg.dlc, dlc_mode_name(g_cfg.dlc_mode), g_cfg.dlc_big, g_cfg.restore_batch,
               g_cfg.internal, g_cfg.organize, g_cfg.lang >= 0 ? lang_tag(g_cfg.lang) : "auto");
    buf_printf(&b, "klog=%d\nautorun=%d\n", g_cfg.klog, g_cfg.autorun);
    for (int i = 0; i < g_cfg.npaths; i++)
        buf_printf(&b, "path=%s\n", g_cfg.paths[i]);
    write_file(CONFIG_PATH, b.data, b.len, 0666);
    buf_free(&b);
}

static void config_load(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f)
        return;
    config_t c = g_cfg;
    c.npaths = 0;
    c.klog = 1; /* unless the settings say otherwise (a console that never had the key) */
    char line[768], why[160], path[128];
    int ver = 0, dlc_mode_seen = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "ver=", 4)) {
            ver = atoi(line + 4);
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "batch=", 6))
            c.batch = atoi(line + 6) != 0;
        else if (!strncmp(line, "batch_size=", 11))
            c.batch_size = atoi(line + 11);
        else if (!strncmp(line, "dlc=", 4))
            c.dlc = atoi(line + 4) != 0;
        else if (!strncmp(line, "dlc_mode=", 9)) {
            int mode = dlc_mode_parse(line + 9);
            if (mode >= 0)
                c.dlc_mode = mode, dlc_mode_seen = 1;
        }
        else if (!strncmp(line, "dlc_big=", 8))
            c.dlc_big = atoi(line + 8) != 0;
        else if (!strncmp(line, "restore_batch=", 14))
            c.restore_batch = atoi(line + 14);
        else if (!strncmp(line, "internal=", 9))
            c.internal = atoi(line + 9) != 0;
        else if (!strncmp(line, "organize=", 9))
            c.organize = atoi(line + 9) != 0;
        else if (!strncmp(line, "klog=", 5))
            c.klog = atoi(line + 5) != 0;
        else if (!strncmp(line, "autorun=", 8))
            c.autorun = atoi(line + 8) != 0;
        else if (!strncmp(line, "lang=", 5))
            c.lang = lang_from_tag(line + 5);
        else if (!strncmp(line, "path=", 5) && c.npaths < MAX_SCAN_PATHS &&
                 !clean_scan_path(line + 5, path, sizeof(path), why, sizeof(why)))
            snprintf(c.paths[c.npaths++], sizeof(c.paths[0]), "%s", path);
    }
    fclose(f);
    if (c.batch_size < 1 || c.batch_size > 50)
        c.batch_size = 5;
    if (c.restore_batch < 1 || c.restore_batch > MAX_RESTORE_BATCH)
        c.restore_batch = 5;
    if (!c.npaths) {
        snprintf(c.paths[0], sizeof(c.paths[0]), DEFAULT_SCAN_PATH);
        c.npaths = 1;
    }
    /* Before 0.7.5 the settings had no version and organize was off: the pkgs stayed where
     * they were, the console's installer found them again and a game could end up installed
     * on both drives (issue #4). The ps4 folder is the default again; turning it off is kept
     * from the next save on. */
    if (ver < 2)
        c.organize = 1;
    /* ver 3 adds dlc_mode: a file from before only has dlc=, where on meant only that
     * add-ons were served. It is not consent to duplicate today's very large add-ons
     * internally, so on maps to external mounts; off stays off. */
    if (ver < 3 && !dlc_mode_seen)
        c.dlc_mode = c.dlc ? DLC_EXTERNAL : DLC_OFF;
    c.dlc = c.dlc_mode != DLC_OFF;
    config_publish(&c);
    printf("[config] batch=%d batch_size=%d dlc=%d dlc_mode=%s restore_batch=%d organize=%d folders=%d\n",
           g_cfg.batch, g_cfg.batch_size, g_cfg.dlc, dlc_mode_name(g_cfg.dlc_mode), g_cfg.restore_batch,
           g_cfg.organize, g_cfg.npaths);
}

static const char *payload_file(char *out, size_t n);
static int autorun_apply(int on);

void app_config_json(buf_t *out) {
    config_t c;
    cfg_snapshot(&c);
    char pfb[PATH_MAX];
    const char *pf = payload_file(pfb, sizeof(pfb));
    buf_printf(out,
               "{\"batch\":%s,\"batch_size\":%d,\"dlc\":%s,\"dlc_mode\":\"%s\",\"dlc_big\":%s,\"internal\":%s,\"organize\":%s,"
               "\"restore_batch\":%d,\"max_restore_batch\":%d,"
               "\"lang\":\"%s\",\"ui_lang\":\"%s\",\"klog\":%s,\"autorun\":%s,\"autorun_ok\":%s,\"autorun_path\":",
               c.batch ? "true" : "false", c.batch_size, c.dlc ? "true" : "false",
               dlc_mode_name(c.dlc_mode), c.dlc_big ? "true" : "false", c.internal ? "true" : "false",
               c.organize ? "true" : "false", c.restore_batch, MAX_RESTORE_BATCH,
               c.lang >= 0 ? lang_tag(c.lang) : "auto", lang_tag(lang_current()), c.klog ? "true" : "false",
               c.autorun ? "true" : "false", pf ? "true" : "false");
    json_str(out, pf ? pf : "");
    buf_printf(out, ",\"paths\":[");
    for (int i = 0; i < c.npaths; i++) {
        char shown[130];
        snprintf(shown, sizeof(shown), "/%s", c.paths[i]);
        buf_printf(out, "%s", i ? "," : "");
        json_str(out, shown);
    }
    buf_printf(out, "]}");
}

static void url_decode(const char *in, size_t len, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        if (in[i] == '+')
            out[o++] = ' ';
        else if (in[i] == '%' && i + 2 < len) {
            char hex[3] = {in[i + 1], in[i + 2], 0};
            out[o++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else
            out[o++] = in[i];
    }
    out[o] = 0;
}

/* body: batch=0|1&batch_size=N&dlc=0|1&restore_batch=N&paths=<folders, one per line> (form
 * encoded); keys left out keep their value. */
int app_config_set(const char *body, char *msg, size_t cap) {
    config_t c = g_cfg;
    int was_organize = c.organize;
    int dlc_mode_seen = 0, dlc_seen = -1;
    char value[1200], why[160] = "";
    for (const char *p = body; p && *p;) {
        const char *amp = strchr(p, '&'), *eq = strchr(p, '=');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (eq && eq < p + len) {
            url_decode(eq + 1, (size_t)(p + len - eq - 1), value, sizeof(value));
            size_t k = (size_t)(eq - p);
            if (k == 5 && !strncmp(p, "batch", 5))
                c.batch = atoi(value) != 0;
            else if (k == 10 && !strncmp(p, "batch_size", 10))
                c.batch_size = atoi(value);
            else if (k == 3 && !strncmp(p, "dlc", 3)) {
                c.dlc = atoi(value) != 0;
                dlc_seen = c.dlc;
            }
            else if (k == 8 && !strncmp(p, "dlc_mode", 8)) {
                c.dlc_mode = dlc_mode_parse(value);
                if (c.dlc_mode < 0) {
                    snprintf(msg, cap, "%s", TR("Not saved: unknown add-on mode", "No se guardó: modo de DLC desconocido"));
                    return -1;
                }
                dlc_mode_seen = 1;
            }
            else if (k == 7 && !strncmp(p, "dlc_big", 7))
                c.dlc_big = atoi(value) != 0;
            else if (k == 8 && !strncmp(p, "internal", 8))
                c.internal = atoi(value) != 0;
            else if (k == 8 && !strncmp(p, "organize", 8))
                c.organize = atoi(value) != 0;
            else if (k == 4 && !strncmp(p, "klog", 4))
                c.klog = atoi(value) != 0;
            else if (k == 7 && !strncmp(p, "autorun", 7))
                c.autorun = atoi(value) != 0;
            else if (k == 4 && !strncmp(p, "lang", 4))
                c.lang = lang_from_tag(value); /* "auto" and anything else: the console's */
            else if (k == 13 && !strncmp(p, "restore_batch", 13)) {
                char *end;
                long v = strtol(value, &end, 10);
                c.restore_batch = *value && !*end ? (int)v : 0;
            } else if (k == 5 && !strncmp(p, "paths", 5)) {
                c.npaths = 0;
                for (char *line = strtok(value, "\n"); line; line = strtok(NULL, "\n")) {
                    char path[128];
                    if (clean_scan_path(line, path, sizeof(path), why, sizeof(why))) {
                        snprintf(msg, cap, TR("Not saved: %s", "No se guardó: %s"), why);
                        return -1;
                    }
                    int dup = 0;
                    for (int i = 0; i < c.npaths; i++)
                        dup |= !strcmp(c.paths[i], path);
                    if (dup)
                        continue;
                    if (c.npaths == MAX_SCAN_PATHS) {
                        snprintf(msg, cap, TR("Not saved: at most %d folders", "No se guardó: como máximo %d carpetas"), MAX_SCAN_PATHS);
                        return -1;
                    }
                    snprintf(c.paths[c.npaths++], sizeof(c.paths[0]), "%s", path);
                }
            }
        }
        p = amp ? amp + 1 : NULL;
    }
    /* dlc_mode wins over the legacy dlc flag; dlc alone maps to off/external. */
    if (!dlc_mode_seen && dlc_seen >= 0) {
        if (!dlc_seen)
            c.dlc_mode = DLC_OFF;
        else if (c.dlc_mode == DLC_OFF)
            c.dlc_mode = DLC_EXTERNAL;
    }
    c.dlc = c.dlc_mode != DLC_OFF;
    if (c.batch_size < 1 || c.batch_size > 50) {
        snprintf(msg, cap, "%s", TR("Not saved: games per group must be between 1 and 50", "No se guardó: los juegos por grupo deben estar entre 1 y 50"));
        return -1;
    }
    if (c.restore_batch < 1 || c.restore_batch > MAX_RESTORE_BATCH) {
        snprintf(msg, cap,
                 TR("Not saved: games per group when moving back must be a number from 1 to %d",
                    "No se guardó: los juegos por grupo al devolver deben ser un número de 1 a %d"),
                 MAX_RESTORE_BATCH);
        return -1;
    }
    if (!c.npaths) {
        snprintf(msg, cap, "%s", TR("Not saved: add at least one folder (/ is the drive root)", "No se guardó: añade al menos una carpeta (/ es la raíz de la unidad)"));
        return -1;
    }
    int autorun_changed = c.autorun != g_cfg.autorun;
    if (autorun_changed && autorun_apply(c.autorun)) {
        printf("[config] autorun %s: could not update payloads.ini\n", c.autorun ? "on" : "off");
        snprintf(msg, cap, "%s", TR("Not saved: could not update GoldHEN's AutoRun list",
                                    "No se guardó: no se pudo actualizar la lista de AutoRun de GoldHEN"));
        return -1;
    }
    config_publish(&c);
    apply_lang();
    config_save();
    printf("[config] saved: batch=%d batch_size=%d dlc=%d restore_batch=%d organize=%d folders=%d\n", c.batch,
           c.batch_size, c.dlc, c.restore_batch, c.organize, c.npaths);
    if (was_organize && !c.organize) /* turned off: send the organized games back to where they were */
        for (int i = 0; i < NROOTS; i++)
            g_migrated[i] = 0;
    scan_request_why(0, SCAN_TRIG_CONFIG);
    snprintf(msg, cap, "%s", TR("Settings saved", "Ajustes guardados"));
    return 0;
}

/* --- pkgs sent from the panel --- */

/* Free bytes for a new file in dir (or its drive when dir does not exist yet). */
static uint64_t free_bytes(const char *dir) {
    struct statfs sf;
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s", dir);
    while (statfs(p, &sf)) {
        char *slash = strrchr(p, '/');
        if (!slash || slash == p)
            return 0;
        *slash = 0;
    }
    return (uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize;
}

/* The folder a pkg sent to dest ("usb0".."usb7", "ext0", "ext1", "internal") goes to: the
 * first scan folder of the drive, or /data/Lodestar. -1 when dest is not usable now. */
static int upload_dir(const char *dest, char *dir, size_t n) {
    char root[PATH_MAX], id[64];
    for (int i = 0; i < NROOTS; i++) {
        root_path(i, root, sizeof(root));
        if (!strcmp(dest, root_id(root, id, sizeof(id))) && root_present(root)) {
            scan_dir(dir, n, root, 0);
            return 0;
        }
    }
    return -1;
}

static void dests_json(buf_t *out, uint64_t need) {
    char root[PATH_MAX], dir[PATH_MAX];
    int first = 1;
    buf_printf(out, "[");
    for (int i = 0; i < NROOTS; i++) {
        char id[64];
        root_path(i, root, sizeof(root));
        if (!root_present(root))
            continue;
        scan_dir(dir, sizeof(dir), root, 0);
        uint64_t fb = free_bytes(dir);
        buf_printf(out, "%s{\"id\":", first ? "" : ",");
        json_str(out, root_id(root, id, sizeof(id)));
        buf_printf(out, ",\"label\":");
        json_str(out, root_label(root));
        buf_printf(out, ",\"folder\":");
        json_str(out, dir);
        buf_printf(out, ",\"free\":%llu,\"fits\":%s}", (unsigned long long)fb, fb > need ? "true" : "false");
        first = 0;
    }
    buf_printf(out, "]");
}

/* Why PS4Mount cannot use a checked pkg of size bytes (0: not known); empty when it can. */
static void check_problem(const pkg_check_t *c, uint64_t size, char *problem, size_t n) {
    snprintf(problem, n, "%s", c->problem);
    if (!problem[0] && size && size != c->pkg_size)
        snprintf(problem, n,
                 TR("the file is %llu bytes but its header says %llu: it is incomplete or damaged",
                    "el archivo tiene %llu bytes pero su cabecera dice %llu: está incompleto o dañado"),
                 (unsigned long long)size, (unsigned long long)c->pkg_size);
    if (!problem[0] && c->kind != PKG_KIND_ADDON && installed_internally(c->title_id))
        snprintf(problem, n,
                 TR("%s is installed on the console the normal way: Lodestar leaves it alone",
                    "%s está instalado en la consola de la forma normal: Lodestar no lo toca"),
                 c->title_id);
}

static unsigned g_check_tmp_seq;

void app_check(const void *data, size_t len, uint64_t size, buf_t *out) {
    char tmp[PATH_MAX];
    unsigned seq = __atomic_add_fetch(&g_check_tmp_seq, 1, __ATOMIC_RELAXED);
    snprintf(tmp, sizeof(tmp), STATE_DIR "/check.%08x.part", seq);
    pkg_check_t c;
    config_t cfg;
    cfg_snapshot(&cfg);
    mkdirs(STATE_DIR, 0777);
    int rc = write_file(tmp, data, len, 0666) ? -1 : pkg_check(tmp, g_fw, &c);
    unlink(tmp);
    if (rc) {
        buf_printf(out, "{\"ok\":false,\"problem\":\"%s\"}", TR("not a PS4 pkg", "no es un pkg de PS4"));
        return;
    }
    char problem[256], notes[3][200], sv[16], fw[16], where[32];
    int nnotes = 0;
    check_problem(&c, size, problem, sizeof(problem));
    if (c.warning[0])
        snprintf(notes[nnotes++], sizeof(notes[0]), "%s", c.warning);
    if (c.kind == PKG_KIND_ADDON && !cfg.dlc)
        snprintf(notes[nnotes++], sizeof(notes[0]), "%s",
                 TR("add-ons are turned off in the settings: it will wait in the folder",
                    "los DLC están desactivados en los ajustes: esperará en la carpeta"));
    if (c.kind == PKG_KIND_UPDATE && find_root_snapshot(c.title_id, where, sizeof(where), &cfg) && !db_is_registered(c.title_id))
        snprintf(notes[nnotes++], sizeof(notes[0]), "%s",
                 TR("the game of this update is not on a drive yet: send it too",
                    "el juego de esta actualización aún no está en ninguna unidad: envíalo también"));
    if (c.kind == PKG_KIND_GAME && !find_root_snapshot(c.title_id, where, sizeof(where), &cfg))
        snprintf(notes[nnotes++], sizeof(notes[0]),
                 TR("%s already has a game in %s: this copy will be left where it is",
                    "%s ya tiene un juego en %s: esta copia se dejará donde está"),
                 c.title_id, root_label(where));
    ver_text(c.system_ver, sv, sizeof(sv));
    ver_text(g_fw, fw, sizeof(fw));
    buf_printf(out, "{\"ok\":%s,\"install\":%s,\"problem\":", problem[0] ? "false" : "true",
               c.installable && !installed_internally(c.title_id) ? "true" : "false");
    json_str(out, problem);
    buf_printf(out, ",\"kind\":\"%s\",\"title\":", pkg_kind_name(c.kind));
    json_str(out, c.title);
    buf_printf(out, ",\"title_id\":");
    json_str(out, c.title_id);
    buf_printf(out, ",\"content_id\":");
    json_str(out, c.content_id);
    buf_printf(out, ",\"app_ver\":");
    json_str(out, c.app_ver);
    buf_printf(out, ",\"version\":");
    json_str(out, c.version);
    buf_printf(out, ",\"system_ver\":");
    json_str(out, c.system_ver ? sv : "");
    buf_printf(out, ",\"console_fw\":");
    json_str(out, fw);
    buf_printf(out, ",\"pkg_size\":%llu,\"notes\":[", (unsigned long long)c.pkg_size);
    for (int i = 0; i < nnotes; i++) {
        buf_printf(out, "%s", i ? "," : "");
        json_str(out, notes[i]);
    }
    buf_printf(out, "],\"dests\":");
    dests_json(out, c.pkg_size);
    buf_printf(out, "}");
}

/* Leftovers of a load that stopped mid-check. Names are unique per request now; all of
 * them are PS4Mount-owned bounded copies of bytes already sent by the panel client. */
static void check_temp_cleanup(void) {
    unlink(STATE_DIR "/check.part"); /* the fixed name older versions used */
    DIR *d = opendir(STATE_DIR);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n > 10 && !strncmp(e->d_name, "check.", 6) && !strcmp(e->d_name + n - 5, ".part")) {
            char p[PATH_MAX];
            snprintf(p, sizeof(p), STATE_DIR "/%s", e->d_name);
            unlink(p);
        }
    }
    if (d)
        closedir(d);
}

static void upload_done(const char *path) {
    (void)path;
    scan_request_why(1, SCAN_TRIG_UPLOAD); /* the scan picks it up now */
}

/* --- homebrew apps sent to be installed --- */
/* A small homebrew app is not used from its pkg: sent from the panel with "Send and
 * install", it lands in INSTALL_DIR and goes to the system's installer, like a pkg
 * installed by hand. The copy is deleted once the system has the app. */
#define INSTALL_DIR STATE_DIR "/install"
/* (when its files may go: stage.h) */
static int g_panel_fetching; /* 1 while the panel package is downloading, 2 after it finishes */

static pthread_mutex_t g_inst_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_inst_path[PATH_MAX]; /* received, for the main loop to install */

static int install_waiting(void) {
    pthread_mutex_lock(&g_inst_mu);
    int waiting = g_inst_path[0] != 0;
    pthread_mutex_unlock(&g_inst_mu);
    return waiting;
}

static void upload_install_done(const char *path) {
    pthread_mutex_lock(&g_inst_mu);
    snprintf(g_inst_path, sizeof(g_inst_path), "%s", path);
    pthread_mutex_unlock(&g_inst_mu);
}

/* The title ID and title of the pkg at path; 0 when read. */
static int pkg_id_title(const char *path, char *tid, char *title, size_t n) {
    pkg_t *pkg = mem_alloc(sizeof(*pkg));
    sfo_t *sfo = mem_alloc(sizeof(*sfo));
    int rc = -1;
    if (pkg && sfo && !pkg_open(pkg, path)) {
        snprintf(tid, 10, "%s", pkg->title_id);
        const sfo_item_t *t = read_sfo(pkg, sfo) ? NULL : sfo_get(sfo, "TITLE");
        snprintf(title, n, "%s", t && !t->is_int ? t->sval : pkg->title_id);
        pkg_close(pkg);
        rc = 0;
    }
    mem_free(pkg);
    mem_free(sfo);
    return rc;
}

/* The installer service is used from the main loop only (the upload has a thread of its own). */
static void install_received(void) {
    char path[PATH_MAX];
    pthread_mutex_lock(&g_inst_mu);
    snprintf(path, sizeof(path), "%s", g_inst_path);
    g_inst_path[0] = 0;
    pthread_mutex_unlock(&g_inst_mu);
    if (!path[0])
        return;
    char tid[10] = "", title[128] = "", url[PATH_MAX + 8], msg[PATH_MAX + 64];
    if (pkg_id_title(path, tid, title, sizeof(title)))
        snprintf(title, sizeof(title), "%s", path);
    int32_t user = 0;
    sceUserServiceGetForegroundUser(&user);
    int task = install_pkg(pkg_url(tid[0] ? tid : "install", "up", path, url, sizeof(url)), title, user, msg,
                           sizeof(msg));
    printf("[install] %s: %s\n", path, msg);
    if (task < 0) {
        /* refused by BGFT: the copy is PS4Mount's (the pkg stays where the user has it), and it
         * would sit on the console's drive for days */
        struct stat rst;
        unsigned long long rbytes = stat(path, &rst) ? 0 : (unsigned long long)rst.st_size;
        if (!unlink(path))
            printf("[clean] %s: the system's installer refused it, the copy was deleted (%llu bytes)\n", path, rbytes);
        else
            printf("[install] could not delete %s: %s\n", path, strerror(errno));
        notify_kind(NOTIFY_WARN, TR("%s could not be installed: the system's installer turned it down (see the log)",
                  "%s no se pudo instalar: el instalador del sistema lo rechazó (mira el log)"),
               title);
    } else
        notify(TR("Installing %s: it shows on the home screen when it is done", "Instalando %s: aparece en la pantalla de inicio cuando termine"),
               title);
}

/* Copies whose app the system has installed by now, and the leftovers nobody will finish
 * (an old unfinished transfer, a pkg the system never took), are deleted: this folder is on
 * the console's own drive, and every one of its files is a copy PS4Mount made. */
static void install_cleanup(void) {
    DIR *d = opendir(INSTALL_DIR);
    struct dirent *e;
    time_t now = time(NULL);
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char path[PATH_MAX], tid[10] = "", app[PATH_MAX];
        struct stat st, ast;
        snprintf(path, sizeof(path), INSTALL_DIR "/%s", e->d_name);
        if (lstat(path, &st) || !S_ISREG(st.st_mode))
            continue;
        int receiving = upload_active() || __atomic_load_n(&g_panel_fetching, __ATOMIC_ACQUIRE) == 1;
        switch (stage_action(e->d_name, (long)(now - st.st_mtime), receiving)) {
        case STAGE_KEEP:
            continue;
        case STAGE_DELETE_PART:
            if (!unlink(path))
                printf("[clean] %s: unfinished transfer nobody is writing, deleted (%llu bytes)\n", path,
                       (unsigned long long)st.st_size);
            continue;
        case STAGE_DELETE_ABANDONED:
            if (!unlink(path))
                printf("[clean] %s: abandoned copy, deleted after %ld days (%llu bytes)\n", path,
                       (long)((now - st.st_mtime) / 86400), (unsigned long long)st.st_size);
            continue;
        case STAGE_VERIFY:
            break;
        }
        pkg_check_t c;
        if (pkg_check(path, g_fw, &c))
            continue;
        snprintf(tid, sizeof(tid), "%s", c.title_id);
        char root[96];
        if (stage_install_paths(c.kind, c.title_id, c.content_id, root, sizeof(root), app, sizeof(app)))
            continue;
        int target_stat = !lstat(app, &ast);
        if (!stage_installed_ok(is_game_id(tid), !layers_on_failclosed(root), db_confirms_registered(tid),
                                target_stat && S_ISREG(ast.st_mode), target_stat ? (uint64_t)ast.st_size : 0,
                                (uint64_t)st.st_size))
            continue;
        if (!unlink(path))
            printf("[clean] %s: %s installed, copy deleted (%llu bytes)\n", path, tid, (unsigned long long)st.st_size);
    }
    if (d)
        closedir(d);
}

/* --- the panel app --- */
/* A home screen tile that shows the panel (panel-app/): few find the panel from a notice.
 * When the console does not have it, its pkg is downloaded from this version's release and
 * installed like an app sent with "Send and install"; a first start opens it. Once it was
 * installed, a tile the player deleted is not put back (PANEL_APP_DONE). */
#define PANEL_APP_URL "https://github.com/felipebasurto/Lodestar-PS4/releases/download/v%s/lodestar-panel.pkg"
#define PANEL_APP_DONE STATE_DIR "/panel-app"
#define PANEL_APP_DELAY 15 /* seconds after the start: the network is up by then */

typedef struct { uint32_t sz, user_id, app_opt; uint64_t crash_report, check_flag; } lnc_param_t;
int sceLncUtilLaunchApp(const char *tid, const char *argv[], lnc_param_t *param);

static int panel_app_sink(void *ctx, const uint8_t *data, size_t len) {
    return fwrite(data, 1, len, ctx) != len;
}

static void *panel_app_fetch(void *arg) {
    (void)arg;
    char url[2048], err[192] = "";
    const char *part = INSTALL_DIR "/" PANEL_PART_NAME, *pkg = INSTALL_DIR "/lodestar-panel.pkg";
    snprintf(url, sizeof(url), PANEL_APP_URL, &VERSION[sizeof("Lodestar ") - 1]);
    mkdir(INSTALL_DIR, 0777);
    FILE *f = fopen(part, "wb");
    int status = 0;
    for (int hop = 0; f && hop < 5; hop++) { /* GitHub answers with a redirect to its file server */
        http_req_t q = {.method = "GET", .url = url, .sink = panel_app_sink, .sink_ctx = f};
        http_resp_t r;
        memset(&r, 0, sizeof(r));
        if (http_request(&q, &r, err, sizeof(err))) {
            http_resp_free(&r);
            break;
        }
        status = r.status;
        if (status / 100 == 3 && r.location[0])
            snprintf(url, sizeof(url), "%s", r.location);
        http_resp_free(&r);
        if (status / 100 != 3)
            break;
        rewind(f); /* the redirect's own body is not the file */
        if (ftruncate(fileno(f), 0))
            break;
    }
    int ok = f && status == 200 && !fflush(f) && ftell(f) > 0;
    if (f)
        fclose(f);
    if (ok && !rename(part, pkg)) {
        printf("[panel-app] downloaded %s\n", pkg);
        upload_install_done(pkg);
    } else {
        unlink(part);
        printf("[panel-app] not downloaded (%d %s): %s\n", status, err, url);
    }
    __atomic_store_n(&g_panel_fetching, 2, __ATOMIC_RELEASE);
    return NULL;
}

/* The tile, from the main loop: downloaded once per load when missing, and opened by a
 * first start (not over a game). */
static void panel_app_step(void) {
    static time_t start;
    static int opened;
    if (!start)
        start = time(NULL);
    int have = installed_internally(PANEL_APP_TID);
    if (have && !exists(PANEL_APP_DONE)) {
        int fd = open(PANEL_APP_DONE, O_WRONLY | O_CREAT, 0666);
        if (fd >= 0)
            close(fd);
    }
    if (have && setup_active() && !opened && db_is_registered(PANEL_APP_TID)) {
        uint32_t ids[1];
        char tids[1][10];
        opened = 1;
        if (!open_games(ids, tids, 1)) {
            int32_t user = 0;
            sceUserServiceGetForegroundUser(&user);
            lnc_param_t lp = {sizeof(lp), (uint32_t)user, 0, 0, 0};
            printf("[panel-app] opened for the first start: 0x%x\n", (unsigned)sceLncUtilLaunchApp(PANEL_APP_TID, NULL, &lp));
        }
    }
    if (have || exists(PANEL_APP_DONE) || __atomic_load_n(&g_panel_fetching, __ATOMIC_ACQUIRE) ||
        time(NULL) - start < PANEL_APP_DELAY || upload_active() || install_waiting())
        return;
    pthread_t th;
    __atomic_store_n(&g_panel_fetching, 1, __ATOMIC_RELEASE);
    if (pthread_create(&th, NULL, panel_app_fetch, NULL))
        __atomic_store_n(&g_panel_fetching, 0, __ATOMIC_RELEASE);
    else
        pthread_detach(th);
}

int app_upload(int fd, const char *query, const void *pre, size_t pre_len, uint64_t length, char *msg, size_t cap) {
    char dest[16] = "", raw[512] = "", name[256], dir[PATH_MAX], inst[4] = "";
    const char *q = strchr(query, '?');
    for (const char *p = q ? q + 1 : ""; *p;) {
        const char *amp = strchr(p, '&'), *eq = strchr(p, '=');
        size_t l = amp ? (size_t)(amp - p) : strlen(p);
        if (eq && eq < p + l) {
            size_t k = (size_t)(eq - p);
            if (k == 4 && !strncmp(p, "dest", 4))
                url_decode(eq + 1, (size_t)(p + l - eq - 1), dest, sizeof(dest));
            else if (k == 4 && !strncmp(p, "name", 4))
                url_decode(eq + 1, (size_t)(p + l - eq - 1), raw, sizeof(raw));
            else if (k == 7 && !strncmp(p, "install", 7))
                url_decode(eq + 1, (size_t)(p + l - eq - 1), inst, sizeof(inst));
        }
        p = amp ? amp + 1 : p + l;
    }
    int install = !strcmp(inst, "1");
    if (install && (upload_active() || __atomic_load_n(&g_panel_fetching, __ATOMIC_ACQUIRE) == 1 || install_waiting())) {
        snprintf(msg, cap, "%s", TR("An install is being prepared. Try again when it starts.",
                                    "Se está preparando una instalación. Vuelve a intentarlo cuando empiece."));
        return -1;
    }
    if (upload_clean_name(raw, name, sizeof(name))) {
        snprintf(msg, cap, "%s", TR("no file name", "falta el nombre del archivo"));
        return -1;
    }
    if (install)
        snprintf(dir, sizeof(dir), "%s", INSTALL_DIR); /* on the console: installed from there */
    else if (upload_dir(dest, dir, sizeof(dir))) {
        snprintf(msg, cap, TR("%s is not available", "%s no está disponible"), dest[0] ? dest : TR("the destination", "el destino"));
        return -1;
    }
    mkdirs(dir, 0777);
    uint64_t fb = free_bytes(dir), margin = 64ull << 20;
    /* length + margin can wrap: compare without adding */
    if (length > fb || fb - length < margin) {
        snprintf(msg, cap, TR("not enough space in %s: %llu MB free, %llu MB needed", "no hay espacio en %s: %llu MB libres, %llu MB necesarios"), dir,
                 (unsigned long long)(fb >> 20), (unsigned long long)((length >> 20) + (margin >> 20)));
        return -1;
    }
    if (upload_start(fd, dir, name, length, pre, pre_len, g_fw, install, install ? upload_install_done : upload_done)) {
        snprintf(msg, cap, "%s", TR("another pkg is being received: wait for it to finish", "ya se está recibiendo otro pkg: espera a que termine"));
        return -1;
    }
    return 0;
}

void app_uploads_json(buf_t *out) { upload_status_json(out); }

void app_dests_json(buf_t *out) {
    buf_printf(out, "{\"dests\":");
    dests_json(out, 0);
    buf_printf(out, "}");
}

/* /api/storage: what PS4Mount keeps on the console's own drive, in bytes, and how much the
 * drive has left. Only reads: it measures with lstat() (a link counts as a link, so a pkg on
 * a drive is never counted or touched) and changes nothing. The games' folders are counted
 * for the titles PS4Mount serves, not the system's own installs. */
void app_storage_json(buf_t *out) {
    unsigned long long state = tree_size(STATE_DIR), install = tree_size(INSTALL_DIR), logs = 0, meta = 0, playgo = 0;
    struct stat st;
    if (!lstat(STATE_DIR "/log.txt", &st))
        logs += (unsigned long long)st.st_size;
    if (!lstat(STATE_DIR "/log.txt.old", &st))
        logs += (unsigned long long)st.st_size;
    char tids[MAX_TITLES][10];
    int n = 0;
    pthread_mutex_lock(&g_managed_id_mu);
    for (int i = 0; i < MAX_TITLES; i++) {
        if (!g_managed[i].used)
            continue;
        snprintf(tids[n++], sizeof(tids[0]), "%s", g_managed[i].tid);
    }
    pthread_mutex_unlock(&g_managed_id_mu);
    for (int i = 0; i < n; i++) {
        char p[64];
        snprintf(p, sizeof(p), "/user/appmeta/%s", tids[i]);
        meta += tree_size(p);
        snprintf(p, sizeof(p), "/system_data/playgo/%s", tids[i]);
        playgo += tree_size(p);
    }
    struct statfs sf;
    unsigned long long free_b = 0, total = 0;
    if (!statfs("/data", &sf)) {
        free_b = (unsigned long long)((uint64_t)sf.f_bavail * (uint64_t)sf.f_bsize);
        total = (unsigned long long)((uint64_t)sf.f_blocks * (uint64_t)sf.f_bsize);
    }
    buf_printf(out,
               "{\"free\":%llu,\"total\":%llu,\"games\":%d,\"items\":["
               "{\"id\":\"install\",\"bytes\":%llu},"
               "{\"id\":\"log\",\"bytes\":%llu},"
               "{\"id\":\"state\",\"bytes\":%llu},"
               "{\"id\":\"holds\",\"bytes\":%llu},"
               "{\"id\":\"playgo\",\"bytes\":%llu},"
               "{\"id\":\"appmeta\",\"bytes\":%llu},"
               "{\"id\":\"library\",\"bytes\":%llu}]}",
               free_b, total, n, install, logs, state > install + logs ? state - install - logs : 0,
               tree_size(HOLD_DIR), playgo, meta, tree_size(INTERNAL_ROOT "/ps4"));
}

/* --- downloads from links --- */

/* The folder a download to dest goes to: the first scan folder of that drive. */
static int links_dest_dir(const char *dest, char *dir, size_t n) {
    if (upload_dir(dest, dir, n))
        return -1;
    mkdirs(dir, 0777);
    return 0;
}

static int links_check(const char *path, uint64_t size, char *problem, size_t n) {
    pkg_check_t c;
    if (pkg_check(path, g_fw, &c)) {
        snprintf(problem, n, "%s", TR("not a PS4 pkg", "no es un pkg de PS4"));
        return -1;
    }
    check_problem(&c, size, problem, n);
    return problem[0] ? -1 : 0;
}

static void links_arrived(const char *dir) {
    (void)dir;
    scan_request_why(1, SCAN_TRIG_LINKS); /* the scan adds them now */
}

static void links_notify(const char *msg) { notify_kind(NOTIFY_DOWNLOAD, "%s", msg); }

static void links_begin(void) {
    static const links_host_t host = {links_dest_dir, free_bytes, links_check, links_arrived, links_notify};
    links_start(STATE_DIR, &host);
}

static int same_file(const char *path, const void *data, size_t size) {
    struct stat st;
    if (stat(path, &st) || (size_t)st.st_size != size)
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    uint8_t *tmp = mem_alloc(size);
    int same = tmp && read(fd, tmp, size) == (ssize_t)size && !memcmp(tmp, data, size);
    mem_free(tmp);
    close(fd);
    return same;
}

static int g_klog = -1, g_web = -1;

/* Closes the ports and the log before main returns: the process stays (it is the
 * loader's) and anything left open in it would keep the next PS4Mount out. */
static void shutdown_daemon(int lock) {
    if (g_web >= 0)
        close(g_web);
    web_stop(); /* before subsystems and log: no worker may still call links or hold a queued fd */
    links_stop();
    install_close();
    bgftscan_free(&g_bgftscan);
    if (g_klog >= 0)
        close(g_klog);
    if (lock >= 0)
        close(lock);
    g_web = g_klog = -1;
    log_stop();
}

static int single_instance(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(LOCK_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 1)) {
        if (s >= 0)
            close(s);
        return -1;
    }
    return s;
}

/* ---- GoldHEN's AutoRun ---- */

/* A Lodestar payload file that the console's loader takes: the wrapped .bin (or the ELF).
 * PS4Mount-named files are foreign and are neither launch targets nor AutoRun cleanup targets
 * (P4N-19). Only the first bytes are read: the file is 3 MB and this runs where the heap is small. */
static const char *payload_file(char *out, size_t n) {
    static const char *cand[] = {
        "/data/payloads/lodestar-ps4.bin",       "/data/payloads/Lodestar-PS4.bin",
        "/data/GoldHEN/payloads/lodestar-ps4.bin", "/data/GoldHEN/payloads/Lodestar-PS4.bin",
        "/user/data/payloads/lodestar-ps4.bin",  "/user/data/payloads/Lodestar-PS4.bin",
        "/data/payloads/lodestar-ps4.elf",       "/data/payloads/Lodestar-PS4.elf",
        "/data/GoldHEN/payloads/lodestar-ps4.elf", "/data/GoldHEN/payloads/Lodestar-PS4.elf",
        "/user/data/payloads/lodestar-ps4.elf",  "/user/data/payloads/Lodestar-PS4.elf",
        NULL};
    out[0] = 0;
    for (int i = 0; cand[i]; i++) {
        struct stat st;
        if (stat(cand[i], &st) || st.st_size < 4096)
            continue;
        uint8_t d[8] = {0};
        int fd = open(cand[i], O_RDONLY);
        if (fd < 0)
            continue;
        ssize_t rn = read(fd, d, sizeof(d));
        close(fd);
        if (rn < 4)
            continue;
        if (!memcmp(d, "\177ELF", 4) || (d[0] == 'U' && d[1] == 'H')) { /* UH: jumps to main */
            snprintf(out, n, "%s", cand[i]);
            return out;
        }
    }
    return NULL;
}

/* GoldHEN's AutoRun list: "/data/GoldHEN/payloads.ini" (the same file as the /user/data
 * one). Entries are "<path> = 1" lines under [AutoRun]; everything else is kept as is. */
#define AUTORUN_INI "/data/GoldHEN/payloads.ini"
#define AUTORUN_INI_ALT "/user/data/GoldHEN/payloads.ini"

/* Makes payloads.ini list (on) or not list (off) the payload under [AutoRun].
 * 0 when it says what the setting says, -1 when there is no payload file or it fails. */
static int autorun_apply(int on) {
    char pfb[PATH_MAX];
    const char *me = on ? payload_file(pfb, sizeof(pfb)) : NULL;
    if (on && !me)
        return -1;
    mkdirs("/data/GoldHEN", 0777);
    const char *ini = AUTORUN_INI;
    struct stat st;
    int found = lstat(ini, &st) == 0;
    if (!found) {
        if (errno != ENOENT)
            return -1;
        if (lstat(AUTORUN_INI_ALT, &st) == 0)
            ini = AUTORUN_INI_ALT, found = 1;
        else if (errno != ENOENT)
            return -1;
    }
    if (!found && !on)
        return 0; /* nothing was listed: nothing to take off */
    if (found && !S_ISREG(st.st_mode))
        return -1;
    size_t len = 0;
    uint8_t *raw = found && st.st_size > 0 ? read_all(ini, &len) : NULL;
    if (found && st.st_size > 0 && (!raw || len != (size_t)st.st_size)) {
        mem_free(raw);
        return -1; /* an unreadable file is not an empty file */
    }
    buf_t out = {0};
    if (autorun_edit_ini(raw, len, me, on, &out)) {
        buf_free(&out);
        mem_free(raw);
        return -1;
    }
    int unchanged = out.len == len && (!len || !memcmp(raw, out.data, len));
    int rc = unchanged ? 0 : autorun_write_atomic(ini, out.data, out.len);
    buf_free(&out);
    mem_free(raw);
    return rc;
}

/* The console's own address, to reach a loader that is not bound to loopback. */
static int lan_ip(struct in_addr *out) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0}, me;
    socklen_t n = sizeof(me);
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    a.sin_addr.s_addr = htonl(0x08080808);
    int ok = s >= 0 && !connect(s, (struct sockaddr *)&a, sizeof(a)) &&
             !getsockname(s, (struct sockaddr *)&me, &n);
    if (s >= 0)
        close(s);
    if (ok)
        *out = me.sin_addr;
    return ok;
}

/* Rest mode closes the network sockets of payloads: a listening socket that stopped
 * listening is found here so it can be opened again. */
static int still_listening(int s) {
    int on = 0;
    socklen_t n = sizeof(on);
    return s >= 0 && !getsockopt(s, SOL_SOCKET, SO_ACCEPTCONN, &on, &n) && on;
}

static int g_live; /* klogsrv replays its backlog first: live after the first quiet 100 ms */

/* Rest mode would cut a download (and close the payload's sockets): while one runs,
 * the system is told every few seconds that the console is in use, as when a pad is
 * pressed. Once it ends the usual rest settings apply again. */
static void keep_awake(void) {
    static time_t last;
    static int on;
    int active = links_active();
    if (active != on) {
        on = active;
        printf("[power] %s\n", on ? "keeping the console out of rest mode while it transfers" : "rest mode allowed again");
    }
    if (on && time(NULL) - last >= 10) {
        last = time(NULL);
        sceSystemServicePowerTick();
    }
}

/* Serves the panel and handles klog lines for up to ms milliseconds. */
static void pump_events(int ms) {
    static char buf[16384], line[4096];
    static size_t ll;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += ms / 1000;
    end.tv_nsec += (long)(ms % 1000) * 1000000;
    for (;;) {
        if (!g_masked)
            web_owner_pump();
        if (g_klog < 0) {
            g_klog = klog_connect();
            g_live = 0;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long left = (end.tv_sec - now.tv_sec) * 1000 + (end.tv_nsec - now.tv_nsec) / 1000000;
        if (left <= 0)
            return;
        long wait = g_live || g_klog < 0 ? left : (left < 100 ? left : 100);
        struct timeval tv = {wait / 1000, (wait % 1000) * 1000};
        fd_set fs;
        FD_ZERO(&fs);
        int maxfd = -1;
        if (g_klog >= 0) {
            FD_SET(g_klog, &fs);
            maxfd = g_klog;
        }
        if (g_web >= 0 && !g_masked) { /* panel requests wait until all games show again */
            FD_SET(g_web, &fs);
            if (g_web > maxfd)
                maxfd = g_web;
        }
        int owner_wake = g_masked ? -1 : web_owner_wake_fd();
        if (owner_wake >= 0) {
            FD_SET(owner_wake, &fs);
            if (owner_wake > maxfd)
                maxfd = owner_wake;
        }
        if (maxfd < 0) {
            usleep((useconds_t)left * 1000);
            return;
        }
        int ready = select(maxfd + 1, &fs, NULL, NULL, &tv);
        if (ready == 0 && !g_live && g_klog >= 0) {
            g_live = 1;
            printf("[klog] backlog done, following live\n");
        }
        if (ready <= 0)
            continue;
        if (owner_wake >= 0 && FD_ISSET(owner_wake, &fs))
            web_owner_pump();
        if (g_web >= 0 && FD_ISSET(g_web, &fs))
            web_serve(g_web);
        if (g_klog < 0 || !FD_ISSET(g_klog, &fs))
            continue;
        ssize_t n = read(g_klog, buf, sizeof(buf));
        if (n <= 0) {
            close(g_klog);
            g_klog = -1;
            continue;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (ll < sizeof(line) - 1)
                    line[ll++] = buf[i];
                continue;
            }
            line[ll] = 0;
            ll = 0;
            if (g_live)
                handle_klog_line(line);
        }
    }
}

/* Serves the panel and the klog for a moment, from inside a long scan: the daemon has no
 * thread of its own for them (catalog.h cat_pump and the scan loops call this). */
static void pump_now(void) { pump_events(1); }

/* The address of the panel as the console's notice shows it: with the console's own
 * address when it has one (no network yet: the placeholder of the README). */
static const char *panel_url(char *out, size_t n) {
    struct in_addr ip;
    char t[16];
    if (lan_ip(&ip) && inet_ntop(AF_INET, &ip, t, sizeof(t)))
        snprintf(out, n, "http://%s:%d", t, WEB_PORT);
    else
        snprintf(out, n, "%s", TR("http://<PS4 IP>:8780", "http://<IP de la PS4>:8780"));
    return out;
}

/* The start, as the console shows it. */
static void notify_running(void) {
    char url[48];
    if (auto_register_blocked()) {
        /* The two-minute promise would be a lie: nothing is registered while PS4Mount is there. */
        printf("[coexist] panel at %s; auto-register is off\n", panel_url(url, sizeof(url)));
        coexist_warn_once();
        return;
    }
    if (setup_active() && g_reg_mode != REG_NONE && g_web >= 0)
        notify(TR(VERSION ": first start. Choose the settings in the Lodestar app or at %s and save them. Without it, "
                          "the games are added with the default settings in 2 minutes.",
                  VERSION ": primera vez. Elige los ajustes en la app Lodestar o en %s y guárdalos. Si no, en 2 minutos "
                          "se añaden los juegos con los ajustes por defecto."),
               panel_url(url, sizeof(url)));
    else if (g_reg_mode == REG_ALL && g_web >= 0)
        notify(TR(VERSION " running. Panel at %s", VERSION " en marcha. Panel en %s"), panel_url(url, sizeof(url)));
    else if (g_reg_mode == REG_ALL)
        notify_kind(NOTIFY_WARN, "%s", TR(VERSION " running (panel unavailable)", VERSION " en marcha (panel no disponible)"));
    else if (g_reg_mode == REG_TITLEDIR)
        notify(TR(VERSION " running (firmware %s: games are added one at a time). Panel at %s",
                  VERSION " en marcha (firmware %s: los juegos se añaden de uno en uno). Panel en %s"),
               g_fw_text, panel_url(url, sizeof(url)));
    else
        notify_kind(NOTIFY_WARN, TR(VERSION ": firmware %s is not supported yet. To protect the system database, no games are added.",
                  VERSION ": el firmware %s aún no es compatible. Para proteger la base de datos del sistema, no se "
                          "añaden juegos."),
               g_fw_text);
}

int main(void) {
    mkdirs(STATE_DIR, 0777);
    /* the log goes to a file, every line with its time; when it cannot be opened, stdout
     * stays as it was. Opened first of all: a load that ends before the panel is up is
     * only seen here */
    log_start(STATE_DIR "/log.txt");
    setvbuf(stdout, NULL, _IOLBF, 0);
    int lock = single_instance();
    if (lock < 0) {
        apply_lang(); /* the running daemon may be either product: use the console's language */
        printf("[main] another mount daemon is running: this load ends here\n");
        notify_kind(NOTIFY_WARN, "%s",
                    TR(VERSION " not started: PS4Mount or another mount daemon is running. Stop it first.",
                       VERSION " no arrancó: PS4Mount u otro gestor de montaje está en marcha. Deténlo antes."));
        log_stop();
        return 0;
    }
    sceUserServiceInitialize(0);
    sceAppInstUtilInitialize();
    /* the PS4 loader runs the payload in a process of its own and returning from main
     * goes back to it: no exit, no atexit. What the payload opened is closed by
     * shutdown() before main returns */
    printf("[main] start (lock %d, pid %d)\n", lock, (int)getpid());
    cat_pump = pump_now; /* the panel answers during a catalog walk */
    setup_set(first_start());
    g_setup_start = time(NULL);
    config_load();
    apply_lang();
    if (g_cfg.autorun && autorun_apply(1))
        printf("[config] autorun on: could not update payloads.ini\n");
    coexist_publish();
    if (setup_active())
        printf("[main] first start: no game is added until the settings are saved\n");
    signal(SIGPIPE, SIG_IGN);
    check_firmware();
    if (user_app_masked())
        unmask_leftover();
    /* left behind if an earlier load stopped mid-check: removed before the panel can ask for one */
    check_temp_cleanup();
    web_status_provider(app_status_state_json);
    g_web = web_listen();
    links_begin();
    notify_running();
    unlink(STATE_DIR "/stop");
    for (;;) {
        if (stop_requested() || exists(STATE_DIR "/stop")) {
            unlink(STATE_DIR "/stop");
            notify("%s", TR(VERSION " stopped", VERSION " detenido"));
            shutdown_daemon(lock);
            return 0;
        }
        if (!still_listening(lock)) {
            /* lost in rest mode: take it again, or stop if another PS4Mount has it now */
            close(lock);
            if ((lock = single_instance()) < 0) {
                notify_kind(NOTIFY_WARN, "%s",
                            TR(VERSION " stopped: another mount daemon is running. Stop it and load Lodestar again",
                               VERSION " detenido: hay otro gestor de montaje en marcha. Deténlo y vuelve a cargar Lodestar"));
                shutdown_daemon(-1);
                return 0;
            }
            printf("[main] instance lock opened again\n");
        }
        if (!still_listening(g_web)) {
            int was = g_web >= 0;
            if (g_web >= 0)
                close(g_web);
            g_web = web_listen();
            if (was || g_web >= 0) /* log changes only, not every retry */
                printf("[main] web panel %s\n", g_web >= 0 ? "listening again" : "unavailable");
        }
        if (g_deleting && g_deleting_close_at && time(NULL) >= g_deleting_close_at)
            end_delete_window();
        if (g_deleting && time(NULL) - g_deleting_since > 600)
            end_delete_window(); /* the line of the screen change was missed */
        if (g_restore_n || g_restore_all) {
            run_restore(g_restore_all ? NULL : g_restore_tids, g_restore_n, 1);
            g_restore_n = 0;
            g_restore_all = 0;
            continue;
        }
        delete_pending_step();
        if (g_readd_tid[0] && !g_deleting) {
            char tid[10];
            snprintf(tid, sizeof(tid), "%s", g_readd_tid);
            g_readd_tid[0] = 0;
            readd_title(tid);
            continue;
        }
        install_received(); /* an app sent with "Send and install" */
        panel_app_step();
        coexist_publish();
        if (scan_due(time(NULL)) && !g_deleting) {
            time_t seen = setup_seen_at();
            if (setup_active() && coexist_setup_timeout_due(g_setup_start, seen, time(NULL), SETUP_WAIT)) {
                if (auto_register_blocked()) {
                    static int held_setup;
                    if (!held_setup) {
                        held_setup = 1;
                        printf("[coexist] first start: PS4Mount is present, defaults are not saved\n");
                        coexist_warn_once();
                    }
                } else {
                    printf("[main] first start: nobody at the panel, the default settings are saved\n");
                    config_save();
                }
            }
            int manual = 0, hard = 0;
            scan_trigger_t trig = SCAN_TRIG_ROUTINE;
            unsigned scan_seq = scan_begin_why(&manual, &hard, &trig);
            g_scan_manual_active = manual;
            int scanned = 1, ran_scan = 0;
            uint64_t scan_t0 = 0;
            if (setup_active()) {
                if (manual)
                    notify("%s", TR("Save the settings in the app or the panel first: no game is added until then",
                                    "Guarda primero los ajustes en la app o en el panel: hasta entonces no se añade ningún juego"));
            } else if (g_reg_mode != REG_NONE) {
                memset(&g_scan_m, 0, sizeof(g_scan_m));
                g_scan_m.active = 1;
                g_scan_m.seq = scan_seq;
                g_scan_m.trigger = trig;
                scan_t0 = mono_ms();
                ran_scan = 1;
                migrate_organized(); /* organize off: the games it had organized, back in place */
                scanned = scan_all(hard);
                /* After a boot the games are mounted by this first scan: one started before
                 * says its data is corrupted. Saying when they are there tells when to start. */
                static int ready_said;
                if (!ready_said) {
                    ready_said = 1;
                    int playable = 0;
                    for (int i = 0; i < MAX_TITLES; i++)
                        playable += g_managed[i].used && !needs_reboot(g_managed[i].tid);
                    if (playable)
                        notify_kind(NOTIFY_READY, TR("%d game(s) ready to play", "%d juego(s) listo(s) para jugar"), playable);
                }
            } else if (manual)
                notify_kind(NOTIFY_WARN, TR("Firmware %s is not supported yet: no games are added",
                          "El firmware %s aún no es compatible: no se añaden juegos"),
                       g_fw_text);
            if (!scanned)
                scan_keep_request_why(manual, hard, trig);
            g_scan_manual_active = 0;
            scan_finish(scan_seq);
            if (ran_scan) {
                g_scan_m.gameplay_running = g_nsandboxes > 0;
                g_scan_m.wall_ms = mono_ms() - scan_t0;
                g_scan_m.active = 0;
                char sline[384];
                scan_metrics_format(&g_scan_m, sline, sizeof(sline));
                printf("%s\n", sline);
            }
            install_cleanup();
            dlc_pending_check();
        }
        keep_awake();
        pump_events(1000);
    }
}
