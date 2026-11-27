/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"
#include "applog.h"
#include "render.h"

#include <orbis/ImeDialog.h>
#include <orbis/NetCtl.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define PANEL_APP_LOG_PATH "/data/lodestar/panel-app.log"

enum {
    ALIGN = 0x200000,
    FRAME_BYTES = WIDTH * HEIGHT * 4,
    STRIDE = (FRAME_BYTES + ALIGN - 1) & ~(ALIGN - 1),
};

typedef struct {
    applog log;
    panel_app app;
    panel_renderer renderer;
    int app_ready;
    int renderer_ready;
    int font_ready;
    int netctl_owned;
    int32_t user;
    int pad;
    int video;
    int buffers_registered;
    int direct_allocated;
    off_t direct_offset;
    void *direct_memory;
    uint32_t *buffers[2];
    int front;
    int submitted;
    wchar_t keyboard[512];
} ps4_sys;

static int64_t clock_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static panel_result ps4_perform(void *ctx, const panel_request *req, const int *stop) {
    ps4_sys *sys = ctx;
    panel_result r = {.kind = req->kind};
    int64_t t0 = clock_ms();
    response res = {0};

    switch (req->kind) {
    case TASK_LOAD:
        res = api_request_ex("/api/games", NULL, 24 * 1024 * 1024, 12000, stop);
        r.http_status = res.status;
        r.catalog = calloc(1, sizeof(*r.catalog));
        r.ok = r.catalog && !api_library(&res, r.catalog);
        if (!r.ok) {
            free(r.catalog);
            r.catalog = NULL;
        }
        break;
    case TASK_STATUS:
        res = api_request_ex("/api/status", NULL, 65536, 2000, stop);
        r.http_status = res.status;
        r.ok = api_scan_active(&res);
        break;
    case TASK_CONFIG:
        res = api_request_ex("/api/config", NULL, 65536, 5000, stop);
        r.http_status = res.status;
        r.ok = !api_settings(&res, &r.config);
        break;
    case TASK_LINKS:
        res = api_request_ex("/api/links", NULL, 65536, 3000, stop);
        r.http_status = res.status;
        r.ok = !api_downloads(&res, &r.links);
        break;
    case TASK_DESTS:
        res = api_request_ex("/api/dests", NULL, 65536, 3000, stop);
        r.http_status = res.status;
        r.ok = !api_destinations(&res, &r.dests);
        break;
    case TASK_ART:
        memcpy(r.cover.id, req->id, sizeof(req->id));
        res = api_request_ex(req->path, NULL, 8 * 1024 * 1024, 2200, stop);
        r.http_status = res.status;
        if (res.status == 200) {
            r.cover.pixels = decode_art_ex(res.body, res.size, &r.cover.accent,
                                           &r.cover.thumb64, &r.cover.opaque);
            r.ok = r.cover.pixels != NULL;
        }
        break;
    case TASK_ENQUEUE:
    case TASK_SAVE:
    case TASK_SCAN:
    case TASK_CHANGE:
        res = api_request_ex(req->path, req->form, 65536, 12000, stop);
        r.http_status = res.status;
        r.ok = !api_result(&res, r.message, sizeof(r.message));
        break;
    case TASK_ADDR: {
        OrbisNetCtlInfo info;
        memset(&info, 0, sizeof(info));
        int32_t rc = sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info);
        if (rc >= 0 && *info.ip_address) {
            snprintf(r.local_url, sizeof(r.local_url), "http://%.15s:%d", info.ip_address, API_PORT);
            r.ok = 1;
        } else {
            applog_write(&sys->log, "error", "sceNetCtlGetInfo rc=0x%x", (unsigned)rc);
        }
        break;
    }
    default:
        break;
    }

    free(res.body);
    r.elapsed_ms = clock_ms() - t0;
    return r;
}

static int ps4_ime_open(void *ctx, enum ime_target target, const char *initial_utf8, size_t max_chars) {
    ps4_sys *sys = ctx;
    const char *p = initial_utf8 ? initial_utf8 : "";
    size_t len = 0;
    size_t cap = sizeof(sys->keyboard) / sizeof(*sys->keyboard);
    while (*p && len + 2 < cap) {
        unsigned cp = utf8_read(&p);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            sys->keyboard[len++] = (wchar_t)(0xd800 | (cp >> 10));
            cp = 0xdc00 | (cp & 0x3ff);
        }
        sys->keyboard[len++] = (wchar_t)cp;
    }
    sys->keyboard[len] = 0;

    static OrbisImeDialogSetting params;
    memset(&params, 0, sizeof(params));
    params.userId = (uint32_t)sys->user;
    params.enterLabel = target == IME_URL ? ORBIS_BUTTON_LABEL_DEFAULT : ORBIS_BUTTON_LABEL_SEARCH;
    params.type = target == IME_URL ? ORBIS_TYPE_TYPE_URL : ORBIS_TYPE_DEFAULT;
    params.maxTextLength = (uint32_t)(max_chars < cap - 1 ? max_chars : cap - 1);
    params.inputTextBuffer = sys->keyboard;
    params.posx = WIDTH / 2;
    params.posy = HEIGHT / 2;
    params.horizontalAlignment = ORBIS_H_CENTER;
    params.verticalAlignment = ORBIS_V_CENTER;

    int32_t rc = sceImeDialogInit(&params, NULL);
    if (rc < 0) {
        applog_write(&sys->log, "error", "sceImeDialogInit rc=0x%x", (unsigned)rc);
        return -1;
    }
    return 0;
}

static enum ime_status ps4_ime_poll(void *ctx, char *out_utf8, size_t out_cap) {
    ps4_sys *sys = ctx;
    OrbisDialogStatus status = sceImeDialogGetStatus();
    if (status == ORBIS_DIALOG_STATUS_RUNNING)
        return IME_RUNNING;
    if (status == ORBIS_DIALOG_STATUS_NONE)
        return IME_CANCELED;

    OrbisDialogResult res;
    memset(&res, 0, sizeof(res));
    if (sceImeDialogGetResult(&res) == 0 && res.endstatus == ORBIS_DIALOG_OK) {
        size_t cap = sizeof(sys->keyboard) / sizeof(*sys->keyboard);
        sys->keyboard[cap - 1] = 0;
        size_t n = 0;
        for (size_t i = 0; i < cap - 1 && sys->keyboard[i]; i++) {
            unsigned cp = (uint16_t)sys->keyboard[i];
            if (cp >= 0xd800 && cp <= 0xdbff && (uint16_t)sys->keyboard[i + 1] >= 0xdc00 &&
                (uint16_t)sys->keyboard[i + 1] <= 0xdfff)
                cp = 0x10000 + ((cp - 0xd800) << 10) + ((uint16_t)sys->keyboard[++i] - 0xdc00);
            else if (cp >= 0xd800 && cp <= 0xdfff)
                cp = 0xfffd;
            size_t bytes = utf8_write(cp, out_utf8 + n, out_cap - n - 1);
            if (!bytes)
                break;
            n += bytes;
        }
        out_utf8[n] = 0;
        return IME_OK;
    }
    return IME_CANCELED;
}

static void ps4_ime_close(void *ctx) {
    (void)ctx;
    OrbisDialogStatus status = sceImeDialogGetStatus();
    if (status == ORBIS_DIALOG_STATUS_RUNNING)
        sceImeDialogAbort();
    if (status != ORBIS_DIALOG_STATUS_NONE)
        sceImeDialogTerm();
}

static uint32_t map_orbis_buttons(uint32_t b) {
    uint32_t out = 0;
    if (b & ORBIS_PAD_BUTTON_UP)       out |= PANEL_BTN_UP;
    if (b & ORBIS_PAD_BUTTON_DOWN)     out |= PANEL_BTN_DOWN;
    if (b & ORBIS_PAD_BUTTON_LEFT)     out |= PANEL_BTN_LEFT;
    if (b & ORBIS_PAD_BUTTON_RIGHT)    out |= PANEL_BTN_RIGHT;
    if (b & ORBIS_PAD_BUTTON_CROSS)    out |= PANEL_BTN_CROSS;
    if (b & ORBIS_PAD_BUTTON_CIRCLE)   out |= PANEL_BTN_CIRCLE;
    if (b & ORBIS_PAD_BUTTON_SQUARE)   out |= PANEL_BTN_SQUARE;
    if (b & ORBIS_PAD_BUTTON_TRIANGLE) out |= PANEL_BTN_TRIANGLE;
    if (b & ORBIS_PAD_BUTTON_L1)       out |= PANEL_BTN_L1;
    if (b & ORBIS_PAD_BUTTON_R1)       out |= PANEL_BTN_R1;
    if (b & ORBIS_PAD_BUTTON_L2)       out |= PANEL_BTN_L2;
    return out;
}

/* PANEL-10: Ordered teardown and SystemService exit. Never returns to crt1/libc exit() (SIGSYS). */
static _Noreturn void app_shutdown_and_exit(ps4_sys *sys, const char *reason, int32_t code) {
    applog_write(&sys->log, "exit", "shutdown start reason=%s code=0x%x", reason ? reason : "normal", (unsigned)code);

    /* 1. Drain any pending VideoOut flip before unregistering buffers. */
    if (sys->video >= 0 && sys->submitted >= 0) {
        for (int i = 0; i < 200; i++) {
            OrbisVideoOutFlipStatus status = {0};
            if (sceVideoOutGetFlipStatus(sys->video, &status) >= 0 && !status.numFlipPending)
                break;
            sceKernelUsleep(1000);
        }
        sys->submitted = -1;
    }

    /* 2. Stop worker (cooperatively aborts HTTP and joins) and close IME/app allocations. */
    if (sys->app_ready) {
        panel_app_destroy(&sys->app);
        sys->app_ready = 0;
    }

    /* 3. Release VideoOut and direct memory in reverse acquisition order. */
    if (sys->video >= 0) {
        if (sys->buffers_registered) {
            sceVideoOutUnregisterBuffers(sys->video, 0);
            sys->buffers_registered = 0;
        }
        sceVideoOutClose(sys->video);
        sys->video = -1;
    }
    if (sys->direct_memory) {
        sceKernelMunmap(sys->direct_memory, STRIDE * 2);
        sys->direct_memory = NULL;
    }
    if (sys->direct_allocated) {
        sceKernelReleaseDirectMemory(sys->direct_offset, STRIDE * 2);
        sys->direct_allocated = 0;
    }

    /* 4. Close pad, NetCtl, renderer shadow/caches and FreeType. */
    if (sys->pad >= 0) {
        scePadClose(sys->pad);
        sys->pad = -1;
    }
    if (sys->netctl_owned) {
        sceNetCtlTerm();
        sys->netctl_owned = 0;
    }
    if (sys->renderer_ready) {
        render_stats rs = panel_renderer_stats(&sys->renderer);
        draw_stats ds = draw_stats_get();
        applog_write(&sys->log, "exit",
                     "stats blits=%llu full=%llu bytes=%llu glyphs=%llu/%llu art=%llu/%llu",
                     (unsigned long long)rs.fb_blits, (unsigned long long)rs.fb_full_copies,
                     (unsigned long long)rs.fb_bytes_written,
                     (unsigned long long)ds.glyph_rasterizations, (unsigned long long)ds.glyph_cache_hits,
                     (unsigned long long)ds.art_scales, (unsigned long long)ds.art_cache_hits);
        panel_renderer_close(&sys->renderer);
        sys->renderer_ready = 0;
    }
    if (sys->font_ready) {
        font_close();
        sys->font_ready = 0;
    }

    /* 5. Request clean title termination from SystemService. */
    applog_write(&sys->log, "exit", "calling sceSystemServiceLoadExec(exit)");
    for (int attempt = 0; attempt < 3; attempt++) {
        int32_t rc = sceSystemServiceLoadExec("exit", NULL);
        applog_write(&sys->log, "error", "sceSystemServiceLoadExec attempt=%d returned 0x%x",
                     attempt + 1, (unsigned)rc);
        sceKernelUsleep(200000);
    }

    /* Safe fallback: never fall back to libc return/exit/_exit (SIGSYS inside libkernel). */
    applog_write(&sys->log, "exit", "fallback: navigating home and parking safely");
    applog_close(&sys->log);
    sceSystemServiceNavigateToGoHome();
    for (;;)
        sceKernelUsleep(200000);
}

static int video_init(ps4_sys *sys) {
    sys->video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
    if (sys->video < 0) {
        applog_write(&sys->log, "error", "sceVideoOutOpen rc=0x%x", (unsigned)sys->video);
        return sys->video;
    }
    int32_t rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), STRIDE * 2, ALIGN,
                                               ORBIS_KERNEL_WC_GARLIC, &sys->direct_offset);
    if (rc < 0) {
        applog_write(&sys->log, "error", "sceKernelAllocateDirectMemory rc=0x%x", (unsigned)rc);
        return rc;
    }
    sys->direct_allocated = 1;

    rc = sceKernelMapDirectMemory(&sys->direct_memory, STRIDE * 2, 0x33, 0, sys->direct_offset, ALIGN);
    if (rc < 0 || !sys->direct_memory) {
        applog_write(&sys->log, "error", "sceKernelMapDirectMemory rc=0x%x", (unsigned)rc);
        sys->direct_memory = NULL;
        return rc < 0 ? rc : -1;
    }
    sys->buffers[0] = (uint32_t *)sys->direct_memory;
    sys->buffers[1] = (uint32_t *)((char *)sys->direct_memory + STRIDE);
    void *addresses[] = {sys->buffers[0], sys->buffers[1]};
    OrbisVideoOutBufferAttribute attributes;
    memset(&attributes, 0, sizeof(attributes));
    sceVideoOutSetBufferAttribute(&attributes, 0x80000000, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
                                  ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, WIDTH, HEIGHT, WIDTH);
    rc = sceVideoOutRegisterBuffers(sys->video, 0, addresses, 2, &attributes);
    if (rc < 0) {
        applog_write(&sys->log, "error", "sceVideoOutRegisterBuffers rc=0x%x", (unsigned)rc);
        return rc;
    }
    sys->buffers_registered = 1;
    sceSystemServiceHideSplashScreen();
    applog_write(&sys->log, "init", "video ready handle=%d stride=%d", sys->video, STRIDE);
    return 0;
}

static void run_loop(ps4_sys *sys) {
    while (!sys->app.quit) {
        int64_t now = clock_ms();
        OrbisPadData state;
        memset(&state, 0, sizeof(state));
        int connected = scePadReadState(sys->pad, &state) >= 0 && state.connected;
        panel_pad pad = {
            .connected = connected,
            .buttons = connected ? map_orbis_buttons(state.buttons) : 0,
            .lx = connected ? (int)state.leftStick.x - 128 : 0,
            .ly = connected ? (int)state.leftStick.y - 128 : 0,
            .rx = connected ? (int)state.rightStick.x - 128 : 0,
            .ry = connected ? (int)state.rightStick.y - 128 : 0,
        };

        panel_app_tick(&sys->app, &pad, now);

        if (sys->submitted >= 0) {
            OrbisVideoOutFlipStatus status = {0};
            if (sceVideoOutGetFlipStatus(sys->video, &status) >= 0 && !status.numFlipPending) {
                sys->front = sys->submitted;
                sys->submitted = -1;
            }
        }

        if (sys->app.dirty && sys->submitted < 0 && !sys->app.quit) {
            int back = sys->front == 0 ? 1 : 0;
            panel_view view;
            panel_app_build_view(&sys->app, clock_ms(), &view);
            if (panel_renderer_present(&sys->renderer, &view, back, sys->buffers[back])) {
                int32_t rc = sceVideoOutSubmitFlip(sys->video, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0);
                if (rc >= 0) {
                    sys->submitted = back;
                    sys->app.dirty = 0;
                } else {
                    applog_write(&sys->log, "error", "sceVideoOutSubmitFlip rc=0x%x", (unsigned)rc);
                }
            } else {
                sys->app.dirty = 0;
            }
        }

        sceKernelUsleep((sys->app.wheel || sys->app.held.action) ? 8000 : 16000);
    }
}

int main(void) {
    static ps4_sys sys;
    memset(&sys, 0, sizeof(sys));
    sys.user = -1;
    sys.pad = -1;
    sys.video = -1;
    sys.front = -1;
    sys.submitted = -1;

    applog_open(&sys.log, PANEL_APP_LOG_PATH, APPLOG_DEFAULT_MAX);
    applog_write(&sys.log, "init", "start LODS00001");

    int32_t language = 1;
    OrbisUserServiceInitializeParams user_params = {.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST};
    int32_t rc = sceUserServiceInitialize(&user_params);
    applog_write(&sys.log, "init", "UserServiceInitialize rc=0x%x", (unsigned)rc);

    sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG, &language);
    int spanish = language == 3 || language == 20;
    applog_write(&sys.log, "init", "lang=%d spanish=%d", language, spanish);

    int32_t ft_rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_FREETYPE_OL);
    int32_t ime_rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
    int32_t net_rc = sceNetCtlInit();
    sys.netctl_owned = (net_rc == 0);
    applog_write(&sys.log, "init", "modules ft=0x%x ime=0x%x netctl=0x%x",
                 (unsigned)ft_rc, (unsigned)ime_rc, (unsigned)net_rc);

    rc = sceUserServiceGetInitialUser(&sys.user);
    if (rc < 0)
        app_shutdown_and_exit(&sys, "init_user", rc);

    rc = scePadInit();
    if (rc < 0)
        app_shutdown_and_exit(&sys, "init_pad", rc);

    if (font_open("/app0") < 0)
        app_shutdown_and_exit(&sys, "init_font", -1);
    sys.font_ready = 1;

    if (panel_renderer_init(&sys.renderer) < 0)
        app_shutdown_and_exit(&sys, "init_renderer", -1);
    sys.renderer_ready = 1;

    sys.pad = scePadOpen(sys.user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    if (sys.pad < 0)
        app_shutdown_and_exit(&sys, "init_pad_open", sys.pad);

    panel_platform plat = {
        .ctx = &sys,
        .perform = ps4_perform,
        .ime_open = ps4_ime_open,
        .ime_poll = ps4_ime_poll,
        .ime_close = ps4_ime_close,
    };
    if (panel_app_init(&sys.app, plat, &sys.log, spanish) < 0)
        app_shutdown_and_exit(&sys, "init_app", -1);
    sys.app_ready = 1;

    if (panel_app_start_worker(&sys.app) < 0)
        app_shutdown_and_exit(&sys, "init_worker", -1);

    rc = video_init(&sys);
    if (rc < 0)
        app_shutdown_and_exit(&sys, "init_video", rc);

    run_loop(&sys);
    app_shutdown_and_exit(&sys, sys.app.quit_reason ? sys.app.quit_reason : "normal", 0);
}
