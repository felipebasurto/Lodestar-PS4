/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "panel_host.h"

#include "panel_fakes/fake_freetype.h"
#include "panel_fakes/orbis/NetCtl.h"
#include "panel_fakes/orbis/Sysmodule.h"
#include "panel_fakes/orbis/SystemService.h"
#include "panel_fakes/orbis/UserService.h"
#include "panel_fakes/orbis/VideoOut.h"
#include "panel_fakes/orbis/libkernel.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------------- Metrics & Virtual Clock ---------------- */

static panel_metrics_t g_metrics;
static int g_in_loop;
static int64_t g_clock_ms = 1000;

void panel_metrics_reset(void) { memset(&g_metrics, 0, sizeof(g_metrics)); }
panel_metrics_t panel_metrics_get(void) { return g_metrics; }
void panel_metrics_on_full_frame_copy(size_t bytes) {
    (void)bytes;
    g_metrics.full_frame_copies++;
}
void panel_metrics_on_full_frame_shade(void) { g_metrics.full_frame_shades++; }
void panel_metrics_on_fb_write(size_t bytes) { g_metrics.fb_bytes_written += bytes; }
void panel_metrics_on_redraw(void) { g_metrics.redraws++; }
void panel_metrics_on_input(void) { g_metrics.inputs++; }
void panel_metrics_on_flip(void) { g_metrics.flips_submitted++; }
void panel_metrics_on_text_raster(int drawing) {
    g_metrics.text_rasterizations++;
    if (!drawing)
        g_metrics.text_rasterizations_measure++;
}
void panel_metrics_on_art_scale(int size) {
    g_metrics.art_scalings++;
    g_metrics.art_scaled_pixels += (uint64_t)size * (uint64_t)size;
}
void panel_metrics_on_blocking_call(int kind) {
    if (!g_in_loop)
        return;
    g_metrics.blocking_loop_calls++;
    if (kind == PANEL_BLOCK_NETCTL)
        g_metrics.blocking_netctl_calls++;
    else if (kind == PANEL_BLOCK_USLEEP)
        g_metrics.blocking_usleep_calls++;
    else if (kind == PANEL_BLOCK_IME)
        g_metrics.blocking_ime_calls++;
}

int64_t panel_host_clock_ms(void) { return g_clock_ms; }
void panel_host_set_clock_ms(int64_t ms) { g_clock_ms = ms; }
void panel_host_advance_clock_ms(int64_t delta_ms) { g_clock_ms += delta_ms; }

int panel_host_usleep(unsigned usec) {
    panel_metrics_on_blocking_call(PANEL_BLOCK_USLEEP);
    int64_t ms = (int64_t)((usec + 999u) / 1000u);
    g_clock_ms += ms;
    return 0;
}

void panel_host_set_in_loop(int in_loop) { g_in_loop = in_loop; }
void panel_host_on_worker_reply(void) {}

/* ---------------- Fake FreeType ---------------- */

static FT_LibraryRec g_ft_lib;
static FT_FaceRec g_ft_faces[8];
static int g_ft_face_next;

int FT_Init_FreeType(FT_Library *alibrary) {
    g_ft_lib.inited = 1;
    g_ft_face_next = 0;
    *alibrary = &g_ft_lib;
    return 0;
}

int FT_Done_FreeType(FT_Library library) {
    if (library)
        library->inited = 0;
    return 0;
}

int FT_New_Face(FT_Library library, const char *filepathname, long face_index, FT_Face *aface) {
    (void)library;
    (void)filepathname;
    (void)face_index;
    if (g_ft_face_next >= 8)
        return -1;
    FT_Face f = &g_ft_faces[g_ft_face_next++];
    memset(f, 0, sizeof(*f));
    f->style_id = g_ft_face_next - 1;
    f->pixel_size = 24;
    f->glyph = &f->slot;
    *aface = f;
    return 0;
}

unsigned int FT_Get_Char_Index(FT_Face face, unsigned long charcode) {
    if (!face || !charcode)
        return 0;
    /* Pretend primary faces lack non-Latin scripts so fallback face path is exercised too. */
    if (face->style_id < 2 && charcode > 0x024f)
        return 0;
    if (charcode == 0xfffd)
        return 0;
    return (unsigned int)charcode;
}

int FT_Set_Pixel_Sizes(FT_Face face, unsigned int pixel_width, unsigned int pixel_height) {
    (void)pixel_width;
    if (!face || pixel_height == 0 || pixel_height > 120)
        return -1;
    face->pixel_size = (int)pixel_height;
    return 0;
}

int FT_Load_Char(FT_Face face, unsigned long char_code, int load_flags) {
    (void)load_flags;
    if (!face)
        return -1;
    int sz = face->pixel_size > 0 ? face->pixel_size : 24;
    int adv = (sz * 55) / 100;
    if (adv < 6)
        adv = 6;
    if (char_code == ' ')
        adv = sz / 3;
    unsigned int w = char_code == ' ' ? 0u : (unsigned int)((sz * 45) / 100);
    unsigned int h = char_code == ' ' ? 0u : (unsigned int)((sz * 70) / 100);
    if (w > 120)
        w = 120;
    if (h > 120)
        h = 120;
    face->slot.advance.x = (long)adv << 6;
    face->slot.advance.y = 0;
    face->slot.bitmap_left = 1;
    face->slot.bitmap_top = (int)h;
    face->slot.bitmap.width = w;
    face->slot.bitmap.rows = h;
    face->slot.bitmap.pitch = (int)w;
    face->slot.bitmap.buffer = face->glyph_buf;
    if (w && h)
        memset(face->glyph_buf, 180, (size_t)w * (size_t)h);
    return 0;
}

/* ---------------- Fake Orbis Subsystems ---------------- */

static panel_lifecycle_t g_life;
static int g_lang = 1;
static char g_ip[16] = "192.168.1.42";
static int g_flip_delay = 0;
static int g_flip_pending_ticks = 0;

static OrbisPadData g_pad_next;
static panel_tick_fn g_tick_fn;
static void *g_tick_ctx;
static int g_force_quit;
static int g_tick_count;
static panel_host_snapshot_t g_last_snap;

/* IME fake state */
static int g_ime_init_rc = 0;
static int g_ime_frames_running = 1;
static int g_ime_frames_left = 0;
static int g_ime_endstatus = ORBIS_DIALOG_OK;
static wchar_t g_ime_script_text[512];
static wchar_t *g_ime_buf = NULL;
static uint32_t g_ime_max_len = 0;
static int g_ime_open = 0;

void panel_host_reset_fakes(void) {
    memset(&g_life, 0, sizeof(g_life));
    g_lang = 1;
    snprintf(g_ip, sizeof(g_ip), "192.168.1.42");
    g_flip_delay = 0;
    g_flip_pending_ticks = 0;
    memset(&g_pad_next, 0, sizeof(g_pad_next));
    g_pad_next.connected = 1;
    g_pad_next.leftStick.x = 128;
    g_pad_next.leftStick.y = 128;
    g_pad_next.rightStick.x = 128;
    g_pad_next.rightStick.y = 128;
    g_ime_init_rc = 0;
    g_ime_frames_running = 1;
    g_ime_frames_left = 0;
    g_ime_endstatus = ORBIS_DIALOG_OK;
    g_ime_script_text[0] = 0;
    g_ime_buf = NULL;
    g_ime_max_len = 0;
    g_ime_open = 0;
    g_force_quit = 0;
    g_clock_ms = 1000;
    memset(&g_last_snap, 0, sizeof(g_last_snap));
}

void panel_host_set_language(int lang_code) { g_lang = lang_code; }
void panel_host_set_ip(const char *ip) { snprintf(g_ip, sizeof(g_ip), "%s", ip ? ip : ""); }
void panel_host_set_flip_delay(int ticks_pending) { g_flip_delay = ticks_pending; }

void panel_host_set_ime_script(int init_rc, int frames_running, int endstatus, const wchar_t *text) {
    g_ime_init_rc = init_rc;
    g_ime_frames_running = frames_running;
    g_ime_endstatus = endstatus;
    size_t i = 0;
    for (; text && text[i] && i + 1 < sizeof(g_ime_script_text) / sizeof(*g_ime_script_text); i++)
        g_ime_script_text[i] = text[i];
    g_ime_script_text[i] = 0;
}

int sceUserServiceInitialize(const OrbisUserServiceInitializeParams *params) {
    (void)params;
    return 0;
}
int sceUserServiceGetInitialUser(int32_t *userId) {
    if (userId)
        *userId = 0x10000000;
    return 0;
}
int sceSystemServiceParamGetInt(int32_t paramId, int32_t *value) {
    if (paramId == ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG && value)
        *value = g_lang;
    return 0;
}
int sceSystemServiceHideSplashScreen(void) {
    g_life.splash_hidden++;
    return 0;
}
int sceSysmoduleLoadModule(uint16_t id) {
    if (id == ORBIS_SYSMODULE_FREETYPE_OL)
        g_life.sysmod_freetype++;
    else if (id == ORBIS_SYSMODULE_IME_DIALOG)
        g_life.sysmod_ime++;
    else if (id == ORBIS_SYSMODULE_NET_CTL)
        g_life.sysmod_netctl++;
    return 0;
}

int scePadInit(void) { return 0; }
int scePadOpen(int32_t userId, int32_t type, int32_t index, const void *param) {
    (void)userId;
    (void)type;
    (void)index;
    (void)param;
    g_life.pad_opens++;
    return 1;
}
int scePadReadState(int32_t handle, OrbisPadData *data) {
    (void)handle;
    if (!data)
        return -1;
    *data = g_pad_next;
    return 0;
}
int scePadClose(int32_t handle) {
    (void)handle;
    g_life.pad_closes++;
    return 0;
}

int sceVideoOutOpen(int32_t userId, int32_t type, int32_t index, const void *param) {
    (void)userId;
    (void)type;
    (void)index;
    (void)param;
    g_life.video_opens++;
    return 1;
}
int sceVideoOutClose(int32_t handle) {
    (void)handle;
    g_life.video_closes++;
    return 0;
}
void sceVideoOutSetBufferAttribute(OrbisVideoOutBufferAttribute *attr, uint32_t pixelFormat, uint32_t tilingMode,
                                   uint32_t aspectRatio, uint32_t width, uint32_t height, uint32_t pitchInPixel) {
    if (!attr)
        return;
    attr->pixelFormat = pixelFormat;
    attr->tilingMode = tilingMode;
    attr->aspectRatio = aspectRatio;
    attr->width = width;
    attr->height = height;
    attr->pitchInPixel = pitchInPixel;
}
int sceVideoOutRegisterBuffers(int32_t handle, int32_t startIndex, void *const *addresses, int32_t bufferNum,
                               const OrbisVideoOutBufferAttribute *attr) {
    (void)handle;
    (void)startIndex;
    (void)addresses;
    (void)bufferNum;
    (void)attr;
    g_life.video_reg++;
    return 0;
}
int sceVideoOutUnregisterBuffers(int32_t handle, int32_t attributeIndex) {
    (void)handle;
    (void)attributeIndex;
    g_life.video_unreg++;
    return 0;
}
int sceVideoOutSubmitFlip(int32_t handle, int32_t bufferIndex, uint32_t flipMode, int64_t flipArg) {
    (void)handle;
    (void)bufferIndex;
    (void)flipMode;
    (void)flipArg;
    panel_metrics_on_flip();
    g_flip_pending_ticks = g_flip_delay;
    return 0;
}
int sceVideoOutGetFlipStatus(int32_t handle, OrbisVideoOutFlipStatus *status) {
    (void)handle;
    if (!status)
        return -1;
    memset(status, 0, sizeof(*status));
    if (g_flip_pending_ticks > 0) {
        status->numFlipPending = g_flip_pending_ticks;
        g_flip_pending_ticks--;
    } else {
        status->numFlipPending = 0;
    }
    return 0;
}

size_t sceKernelGetDirectMemorySize(void) { return 64 * 1024 * 1024; }
int sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t len, size_t alignment, int memoryType,
                                  off_t *physAddrOut) {
    (void)searchStart;
    (void)searchEnd;
    (void)len;
    (void)alignment;
    (void)memoryType;
    g_life.dm_allocs++;
    if (physAddrOut)
        *physAddrOut = 0x200000;
    return 0;
}
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t directMemoryStart, size_t alignment) {
    (void)prot;
    (void)flags;
    (void)directMemoryStart;
    (void)alignment;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    *addr = p;
    return 0;
}
int sceKernelReleaseDirectMemory(off_t start, size_t len) {
    (void)start;
    (void)len;
    g_life.dm_releases++;
    return 0;
}

int sceImeDialogInit(OrbisImeDialogSetting *param, void *extended) {
    (void)extended;
    panel_metrics_on_blocking_call(PANEL_BLOCK_IME);
    if (g_ime_init_rc < 0)
        return g_ime_init_rc;
    g_life.ime_inits++;
    g_ime_open = 1;
    g_ime_buf = param ? param->inputTextBuffer : NULL;
    g_ime_max_len = param ? param->maxTextLength : 0;
    g_ime_frames_left = g_ime_frames_running;
    return 0;
}
int sceImeDialogGetStatus(void) {
    if (!g_ime_open)
        return ORBIS_DIALOG_STATUS_NONE;
    if (g_ime_frames_left > 0) {
        g_ime_frames_left--;
        return ORBIS_DIALOG_STATUS_RUNNING;
    }
    return ORBIS_DIALOG_STATUS_STOPPED;
}
int sceImeDialogGetResult(OrbisDialogResult *result) {
    if (!result)
        return -1;
    result->endstatus = g_ime_endstatus;
    if (g_ime_endstatus == ORBIS_DIALOG_OK && g_ime_buf) {
        size_t i = 0;
        for (; g_ime_script_text[i] && i < g_ime_max_len; i++)
            g_ime_buf[i] = g_ime_script_text[i];
        g_ime_buf[i] = 0;
    }
    return 0;
}
int sceImeDialogTerm(void) {
    panel_metrics_on_blocking_call(PANEL_BLOCK_IME);
    if (g_ime_open) {
        g_ime_open = 0;
        g_life.ime_terms++;
    }
    return 0;
}

int sceNetCtlInit(void) {
    panel_metrics_on_blocking_call(PANEL_BLOCK_NETCTL);
    g_life.netctl_init_calls++;
    if (!g_life.sysmod_netctl)
        g_life.netctl_without_sysmodule++;
    return 0;
}
int sceNetCtlGetInfo(int code, OrbisNetCtlInfo *info) {
    panel_metrics_on_blocking_call(PANEL_BLOCK_NETCTL);
    if (code == ORBIS_NET_CTL_INFO_IP_ADDRESS && info) {
        snprintf(info->ip_address, sizeof(info->ip_address), "%s", g_ip);
        return 0;
    }
    return -1;
}
void sceNetCtlTerm(void) { panel_metrics_on_blocking_call(PANEL_BLOCK_NETCTL); }

void panel_host_on_loop_tick(const panel_host_snapshot_t *snap) {
    g_last_snap = *snap;
    if (++g_tick_count > 5000) {
        g_force_quit = 1;
        return;
    }
    if (g_tick_fn) {
        OrbisPadData next = {.connected = 1, .leftStick = {128, 128}, .rightStick = {128, 128}};
        int fq = 0;
        g_tick_fn(snap, &next, &fq, g_tick_ctx);
        g_pad_next = next;
        if (fq)
            g_force_quit = 1;
    }
}

int panel_host_should_force_quit(void) { return g_force_quit; }

int panel_host_run(panel_tick_fn fn, void *ctx, panel_host_snapshot_t *final_snap, panel_metrics_t *out_metrics,
                   panel_lifecycle_t *out_life) {
    panel_host_reset_app_globals();
    panel_metrics_reset();
    g_tick_fn = fn;
    g_tick_ctx = ctx;
    g_force_quit = 0;
    g_tick_count = 0;
    int rc = panel_app_main();
    g_tick_fn = NULL;
    g_tick_ctx = NULL;
    if (final_snap)
        *final_snap = g_last_snap;
    if (out_metrics)
        *out_metrics = panel_metrics_get();
    if (out_life)
        *out_life = g_life;
    return rc;
}

/* ---------------- Loopback Mock HTTP Server ---------------- */

static pthread_mutex_t g_srv_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_srv_arrived_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_srv_release_cv = PTHREAD_COND_INITIALIZER;
static pthread_t g_srv_thread;
static int g_srv_fd = -1;
static int g_srv_port = 0;
static int g_srv_port_override = 0;
static int g_srv_running = 0;

static char g_srv_games[65536];
static char g_srv_config[4096];
static char g_srv_links[8192];
static char g_srv_dests[4096];
static char g_srv_action_json[1024];
static int g_srv_action_status = 200;
static int g_srv_scanning = 0;
static int g_srv_net_mode = PANEL_NET_OK;
static const uint8_t *g_srv_icon_png = NULL;
static size_t g_srv_icon_len = 0;
static int g_srv_icon_status = 200;

static int g_srv_barrier_armed = 0;
static int g_srv_barrier_arrived = 0;
static int g_srv_barrier_released = 0;

enum { REQ_LOG_CAP = 256 };
static panel_req_log_t g_req_log[REQ_LOG_CAP];
static int g_req_count = 0;

int panel_host_api_port(void) {
    pthread_mutex_lock(&g_srv_mu);
    int port = g_srv_port_override ? g_srv_port_override : g_srv_port;
    if (g_srv_net_mode == PANEL_NET_REFUSED)
        port = 1; /* closed loopback port -> immediate ECONNREFUSED */
    pthread_mutex_unlock(&g_srv_mu);
    return port;
}

static void send_http(int fd, int status, const char *ctype, const void *body, size_t len, int truncate) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, ctype, len);
    if (n > 0)
        (void)send(fd, hdr, (size_t)n, 0);
    if (body && len) {
        size_t to_send = truncate && len > 2 ? len / 2 : len;
        (void)send(fd, body, to_send, 0);
    }
}

static void handle_client(int fd) {
    char buf[8192];
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
        return;
    buf[n] = 0;
    /* Read body if Content-Length wasn't in first recv. */
    char *sep = strstr(buf, "\r\n\r\n");
    if (sep) {
        long cl = 0;
        const char *p = strstr(buf, "Content-Length:");
        if (p && p < sep)
            cl = strtol(p + 15, NULL, 10);
        size_t have = (size_t)(buf + n - (sep + 4));
        while (cl > 0 && have < (size_t)cl && (size_t)n < sizeof(buf) - 1) {
            ssize_t r = recv(fd, buf + n, sizeof(buf) - 1 - (size_t)n, 0);
            if (r <= 0)
                break;
            n += r;
            buf[n] = 0;
            have = (size_t)(buf + n - (sep + 4));
        }
    }

    char method[8] = "", path[128] = "";
    (void)sscanf(buf, "%7s %127s", method, path);
    const char *body = sep ? sep + 4 : "";

    pthread_mutex_lock(&g_srv_mu);
    if (g_req_count < REQ_LOG_CAP) {
        panel_req_log_t *e = &g_req_log[g_req_count++];
        snprintf(e->method, sizeof(e->method), "%s", method);
        snprintf(e->path, sizeof(e->path), "%s", path);
        snprintf(e->body, sizeof(e->body), "%s", body);
    }

    if (!strcmp(path, "/api/games") && g_srv_barrier_armed) {
        g_srv_barrier_armed = 0;
        g_srv_barrier_arrived = 1;
        pthread_cond_broadcast(&g_srv_arrived_cv);
        while (!g_srv_barrier_released && g_srv_running)
            pthread_cond_wait(&g_srv_release_cv, &g_srv_mu);
    }

    int mode = g_srv_net_mode;
    if (mode == PANEL_NET_HTTP_500) {
        pthread_mutex_unlock(&g_srv_mu);
        send_http(fd, 500, "application/json", "{\"error\":\"fail\"}", 16, 0);
        return;
    }
    if (mode == PANEL_NET_MALFORMED_JSON) {
        pthread_mutex_unlock(&g_srv_mu);
        send_http(fd, 200, "application/json", "{not-json", 9, 0);
        return;
    }

    int trunc = (mode == PANEL_NET_TRUNCATED);
    if (!strcmp(path, "/api/games")) {
        char copy[sizeof(g_srv_games)];
        snprintf(copy, sizeof(copy), "%s", g_srv_games);
        pthread_mutex_unlock(&g_srv_mu);
        send_http(fd, 200, "application/json", copy, strlen(copy), trunc);
    } else if (!strcmp(path, "/api/status")) {
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"activity\":{\"phase\":\"%s\"}}", g_srv_scanning ? "scanning" : "");
        pthread_mutex_unlock(&g_srv_mu);
        send_http(fd, 200, "application/json", resp, strlen(resp), trunc);
    } else if (!strcmp(path, "/api/config") && !strcmp(method, "GET")) {
        char copy[sizeof(g_srv_config)];
        snprintf(copy, sizeof(copy), "%s", g_srv_config);
        pthread_mutex_unlock(&g_srv_mu);
        send_http(fd, 200, "application/json", copy, strlen(copy), trunc);
    } else if (!strcmp(path, "/api/links")) {
        char copy[sizeof(g_srv_links)];
        snprintf(copy, sizeof(copy), "%s", g_srv_links);
        pthread_mutex_unlock(&g_srv_mu);
        send_http(fd, 200, "application/json", copy, strlen(copy), trunc);
    } else if (!strcmp(path, "/api/dests")) {
        char copy[sizeof(g_srv_dests)];
        snprintf(copy, sizeof(copy), "%s", g_srv_dests);
        pthread_mutex_unlock(&g_srv_mu);
        send_http(fd, 200, "application/json", copy, strlen(copy), trunc);
    } else if (!strncmp(path, "/icon/", 6)) {
        const uint8_t *png = g_srv_icon_png;
        size_t len = g_srv_icon_len;
        int st = g_srv_icon_status;
        pthread_mutex_unlock(&g_srv_mu);
        if (st == 200 && png && len)
            send_http(fd, 200, "image/png", png, len, trunc);
        else
            send_http(fd, st == 200 ? 404 : st, "text/plain", "missing", 7, 0);
    } else {
        int st = g_srv_action_status;
        char resp[sizeof(g_srv_action_json)];
        snprintf(resp, sizeof(resp), "%s", g_srv_action_json);
        pthread_mutex_unlock(&g_srv_mu);
        send_http(fd, st, "application/json", resp, strlen(resp), trunc);
    }
}

static void *server_loop(void *unused) {
    (void)unused;
    for (;;) {
        int c = accept(g_srv_fd, NULL, NULL);
        if (c < 0) {
            pthread_mutex_lock(&g_srv_mu);
            int running = g_srv_running;
            pthread_mutex_unlock(&g_srv_mu);
            if (!running)
                break;
            if (errno == EINTR)
                continue;
            break;
        }
        handle_client(c);
        close(c);
    }
    return NULL;
}

void panel_host_server_reset(void) {
    pthread_mutex_lock(&g_srv_mu);
    snprintf(g_srv_games, sizeof(g_srv_games),
             "{\"version\":\"test\",\"lang\":\"en\",\"setup\":false,\"drives\":[{\"label\":\"USB0\",\"free\":10737418240}],\"games\":[]}");
    snprintf(g_srv_config, sizeof(g_srv_config),
             "{\"batch\":true,\"batch_size\":50,\"dlc_mode\":\"external\",\"lang\":\"auto\",\"auto_scan\":true,"
             "\"delete_action\":0,\"emu_mode\":0,\"organize\":false}");
    snprintf(g_srv_links, sizeof(g_srv_links),
             "{\"configured\":true,\"job\":{\"busy\":false,\"name\":\"\",\"phase\":\"\",\"done\":0,\"total\":0},"
             "\"jobs\":[],\"result\":\"\"}");
    snprintf(g_srv_dests, sizeof(g_srv_dests),
             "{\"dests\":[{\"id\":\"usb0\",\"label\":\"USB0\",\"free\":10737418240}]}");
    snprintf(g_srv_action_json, sizeof(g_srv_action_json), "{\"ok\":true,\"message\":\"OK\"}");
    g_srv_action_status = 200;
    g_srv_scanning = 0;
    g_srv_net_mode = PANEL_NET_OK;
    g_srv_icon_status = 200;
    g_srv_port_override = 0;
    g_srv_barrier_armed = 0;
    g_srv_barrier_arrived = 0;
    g_srv_barrier_released = 0;
    g_req_count = 0;
    pthread_mutex_unlock(&g_srv_mu);
}

int panel_host_server_start(void) {
    panel_host_server_reset();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = 0};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    socklen_t alen = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &alen);
    g_srv_fd = fd;
    g_srv_port = ntohs(a.sin_port);
    g_srv_running = 1;
    if (pthread_create(&g_srv_thread, NULL, server_loop, NULL) != 0) {
        close(fd);
        g_srv_fd = -1;
        g_srv_running = 0;
        return -1;
    }
    return g_srv_port;
}

void panel_host_server_stop(void) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_running = 0;
    g_srv_barrier_released = 1;
    pthread_cond_broadcast(&g_srv_release_cv);
    int fd = g_srv_fd;
    g_srv_fd = -1;
    pthread_mutex_unlock(&g_srv_mu);
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        pthread_join(g_srv_thread, NULL);
    }
}

void panel_host_server_set_games(const char *json) {
    pthread_mutex_lock(&g_srv_mu);
    snprintf(g_srv_games, sizeof(g_srv_games), "%s", json);
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_set_status(int scanning) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_scanning = scanning;
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_set_config(const char *json) {
    pthread_mutex_lock(&g_srv_mu);
    snprintf(g_srv_config, sizeof(g_srv_config), "%s", json);
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_set_links(const char *json) {
    pthread_mutex_lock(&g_srv_mu);
    snprintf(g_srv_links, sizeof(g_srv_links), "%s", json);
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_set_dests(const char *json) {
    pthread_mutex_lock(&g_srv_mu);
    snprintf(g_srv_dests, sizeof(g_srv_dests), "%s", json);
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_set_icon_png(const uint8_t *png, size_t len) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_icon_png = png;
    g_srv_icon_len = len;
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_set_icon_status(int status) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_icon_status = status;
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_set_net_mode(int mode) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_net_mode = mode;
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_set_action_response(int status, const char *json) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_action_status = status;
    snprintf(g_srv_action_json, sizeof(g_srv_action_json), "%s", json);
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_set_port_override(int port) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_port_override = port;
    pthread_mutex_unlock(&g_srv_mu);
}

void panel_host_server_arm_games_barrier(void) {
    pthread_mutex_lock(&g_srv_mu);
    g_srv_barrier_armed = 1;
    g_srv_barrier_arrived = 0;
    g_srv_barrier_released = 0;
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_wait_games_arrived(void) {
    pthread_mutex_lock(&g_srv_mu);
    while (!g_srv_barrier_arrived && g_srv_running)
        pthread_cond_wait(&g_srv_arrived_cv, &g_srv_mu);
    pthread_mutex_unlock(&g_srv_mu);
}
void panel_host_server_release_games_barrier(const char *next_games_json) {
    pthread_mutex_lock(&g_srv_mu);
    if (next_games_json)
        snprintf(g_srv_games, sizeof(g_srv_games), "%s", next_games_json);
    g_srv_barrier_released = 1;
    pthread_cond_broadcast(&g_srv_release_cv);
    pthread_mutex_unlock(&g_srv_mu);
}

int panel_host_server_req_count(void) {
    pthread_mutex_lock(&g_srv_mu);
    int n = g_req_count;
    pthread_mutex_unlock(&g_srv_mu);
    return n;
}
int panel_host_server_req_get(int idx, panel_req_log_t *out) {
    pthread_mutex_lock(&g_srv_mu);
    int ok = idx >= 0 && idx < g_req_count;
    if (ok && out)
        *out = g_req_log[idx];
    pthread_mutex_unlock(&g_srv_mu);
    return ok;
}
int panel_host_server_has_req(const char *method, const char *path_prefix, const char *body_substr) {
    pthread_mutex_lock(&g_srv_mu);
    int found = 0;
    size_t plen = path_prefix ? strlen(path_prefix) : 0;
    for (int i = 0; i < g_req_count; i++) {
        if (method && strcmp(g_req_log[i].method, method))
            continue;
        if (path_prefix && strncmp(g_req_log[i].path, path_prefix, plen))
            continue;
        if (body_substr && !strstr(g_req_log[i].body, body_substr))
            continue;
        found = 1;
        break;
    }
    pthread_mutex_unlock(&g_srv_mu);
    return found;
}

uint8_t *panel_host_load_png_fixture(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    if (out_len)
        *out_len = (size_t)sz;
    return buf;
}
