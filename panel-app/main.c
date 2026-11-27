/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "view.h"

#include <orbis/ImeDialog.h>
#include <orbis/NetCtl.h>
#include <orbis/Pad.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum task { IDLE, LOAD, STATUS, SCAN, CONFIG, SAVE, CHANGE, ART, LINKS, DESTS, ENQUEUE };

typedef struct {
    enum task kind;
    char id[10], path[96], form[2048];
} request;

typedef struct {
    enum task kind;
    int ok;
    char message[384];
    library *catalog;
    settings config;
    artwork cover;
    downloads links;
    destinations dests;
} result;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static request mailbox;
static result reply;
static int quit_worker;

static library *catalog;
static settings config, original_config;
static int spanish, online, config_ready, sidebar, selected, in_sidebar, visible_count;
static int indices[GAME_LIMIT], counts[3], category, setting_row, action_row;
static enum filter filter;
static enum page page;
static enum action confirm;
static enum task active;
static request pending;
static artwork artwork_cache[ART_SLOTS];
static unsigned next_art;
static char query[256], feedback[384];
static wchar_t keyboard[512];
static char entered_url[sizeof(((request *)0)->form)], local_url[48];
static downloads links;
static destinations dests;
static int links_ready, dest, dest_ready, link_writing;
static int64_t links_at;
static int ime, dirty = 1, quit;
static int scan_seen;
static int64_t refresh_at, library_at, scan_until, feedback_until;

static const char *tr(const char *en, const char *es) { return spanish ? es : en; }

static int64_t clock_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static result perform(request req) {
    result r = {.kind = req.kind};
    response res = {0};
    switch (req.kind) {
    case LOAD:
        res = api_request("/api/games", NULL, 24 * 1024 * 1024, 12000);
        r.catalog = calloc(1, sizeof(*r.catalog));
        r.ok = r.catalog && !api_library(&res, r.catalog);
        if (!r.ok) {
            free(r.catalog);
            r.catalog = NULL;
        }
        break;
    case STATUS:
        res = api_request("/api/status", NULL, 65536, 2000);
        r.ok = api_scan_active(&res);
        break;
    case CONFIG:
        res = api_request("/api/config", NULL, 65536, 5000);
        r.ok = !api_settings(&res, &r.config);
        break;
    case LINKS:
        res = api_request("/api/links", NULL, 65536, 3000);
        r.ok = !api_downloads(&res, &r.links);
        break;
    case DESTS:
        res = api_request("/api/dests", NULL, 65536, 3000);
        r.ok = !api_destinations(&res, &r.dests);
        break;
    case ART:
        memcpy(r.cover.id, req.id, sizeof(req.id));
        res = api_request(req.path, NULL, 8 * 1024 * 1024, 2200);
        if (res.status == 200)
            r.cover.pixels = decode_art(res.body, res.size, &r.cover.accent);
        break;
    case ENQUEUE:
    case SAVE:
    case SCAN:
    case CHANGE:
        res = api_request(req.path, req.form, 65536, 12000);
        r.ok = !api_result(&res, r.message, sizeof(r.message));
        break;
    default:
        break;
    }
    free(res.body);
    return r;
}

static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&mutex);
    for (;;) {
        while (!mailbox.kind && !quit_worker)
            pthread_cond_wait(&wake, &mutex);
        if (quit_worker)
            break;
        request req = mailbox;
        mailbox.kind = IDLE;
        pthread_mutex_unlock(&mutex);
        result r = perform(req);
        pthread_mutex_lock(&mutex);
        reply = r;
    }
    pthread_mutex_unlock(&mutex);
    return NULL;
}

static void submit(request req) {
    pthread_mutex_lock(&mutex);
    mailbox = req;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&mutex);
    active = req.kind;
}

static int writing(void) { return active == CHANGE || active == SAVE || pending.kind == CHANGE || pending.kind == SAVE || link_writing; }

static void say(const char *message) {
    snprintf(feedback, sizeof(feedback), "%s", message);
    feedback_until = clock_ms() + 6500;
    dirty = 1;
}

static int contains(const char *label, const char *part) {
    size_t n = strlen(part);
    if (!n)
        return 1;
    for (; *label; label++)
        if (!strncasecmp(label, part, n))
            return 1;
    return 0;
}

static const game *focused_game(void) {
    return catalog && selected < visible_count ? &catalog->games[indices[selected]] : NULL;
}

static int title_order(const void *a, const void *b) {
    const game *ga = &catalog->games[*(const int *)a], *gb = &catalog->games[*(const int *)b];
    int n = strcasecmp(ga->name, gb->name);
    return n ? n : strcmp(ga->id, gb->id);
}

static void rebuild(const char *keep) {
    visible_count = 0;
    memset(counts, 0, sizeof(counts));
    for (int i = 0; catalog && i < catalog->count; i++) {
        const game *g = &catalog->games[i];
        if (!contains(g->name, query) && !contains(g->id, query))
            continue;
        counts[ALL]++;
        counts[REBOOTS] += g->state == RESTART;
        counts[OPEN] += g->state == RUNNING;
        if (filter == ALL || (filter == REBOOTS && g->state == RESTART) || (filter == OPEN && g->state == RUNNING))
            indices[visible_count++] = i;
    }
    qsort(indices, (size_t)visible_count, sizeof(indices[0]), title_order);
    selected = 0;
    for (int i = 0; keep && i < visible_count; i++) {
        if (!strcmp(keep, catalog->games[indices[i]].id)) {
            selected = i;
            return;
        }
    }
    if (page == DETAIL)
        page = LIBRARY;
}

static artwork *find_art(const char *id) {
    for (size_t i = 0; i < sizeof(artwork_cache) / sizeof(*artwork_cache); i++)
        if (!strcmp(id, artwork_cache[i].id))
            return &artwork_cache[i];
    return NULL;
}

static void clear_art(void) {
    for (size_t i = 0; i < sizeof(artwork_cache) / sizeof(*artwork_cache); i++) {
        free(artwork_cache[i].pixels);
        artwork_cache[i] = (artwork){0};
    }
}

static void accept_reply(void) {
    pthread_mutex_lock(&mutex);
    if (!reply.kind) {
        pthread_mutex_unlock(&mutex);
        return;
    }
    result r = reply;
    reply = (result){0};
    pthread_mutex_unlock(&mutex);
    active = IDLE;
    dirty = 1;
    switch (r.kind) {
    case LOAD:
        online = r.ok;
        if (r.ok) {
            const game *g = focused_game();
            char keep[10] = "";
            if (g)
                memcpy(keep, g->id, sizeof(keep));
            free(catalog);
            catalog = r.catalog;
            spanish = catalog->spanish;
            rebuild(keep);
            library_at = clock_ms() + (catalog->setup ? 30000 : 45000);
            /* Retry previously unavailable icons after the daemon's next library read. */
            for (size_t i = 0; i < sizeof(artwork_cache) / sizeof(*artwork_cache); i++)
                if (!artwork_cache[i].pixels)
                    artwork_cache[i].id[0] = 0;
        }
        refresh_at = clock_ms() + (!online ? 3000 : scan_until ? 1000 : 15000);
        break;
    case STATUS:
        if (scan_until && (clock_ms() >= scan_until || (!r.ok && scan_seen))) {
            scan_until = 0;
            if (!pending.kind)
                pending.kind = LOAD;
        }
        if (r.ok >= 0)
            scan_seen = r.ok;
        if (r.ok < 0)
            online = 0;
        refresh_at = clock_ms() + (scan_until ? 1000 : 15000);
        break;
    case LINKS:
        links_ready = r.ok;
        if (r.ok)
            links = r.links;
        links_at = clock_ms() + 2500;
        break;
    case DESTS:
        dest = 0;
        dest_ready = r.ok;
        dests = r.dests;
        if (!r.ok)
            say(tr("Could not read destinations.", "No se pudieron leer los destinos."));
        break;
    case ENQUEUE:
        link_writing = 0;
        if (r.ok) {
            *entered_url = 0;
            links_at = 0;
        }
        break;
    case CONFIG:
        config_ready = r.ok;
        config = original_config = r.config;
        if (!r.ok)
            say(tr("Could not read settings. Press Cross to retry.", "No se pudieron leer los ajustes. Pulsa X para reintentar."));
        break;
    case ART: {
        artwork *slot = &artwork_cache[next_art++ % (sizeof(artwork_cache) / sizeof(*artwork_cache))];
        free(slot->pixels);
        *slot = r.cover;
        break;
    }
    case SCAN:
        if (r.ok) {
            scan_seen = 0;
            scan_until = clock_ms() + 20000;
            refresh_at = clock_ms() + 1000;
            /* A short scan can begin and end between status reads. */
            pending.kind = LOAD;
        }
        break;
    case CHANGE:
        if (r.ok) {
            page = LIBRARY;
            pending.kind = LOAD;
        }
        break;
    case SAVE:
        if (r.ok) {
            online = 1;
            config_ready = 0;
            page = LIBRARY;
            pending.kind = LOAD;
        }
        break;
    default:
        break;
    }
    if (r.kind == SCAN || r.kind == CHANGE || r.kind == SAVE || r.kind == ENQUEUE)
        say(*r.message ? r.message : tr("No response from Lodestar. Check the library before retrying.",
                                      "Lodestar no respondió. Comprueba la biblioteca antes de reintentar."));
}

static void schedule(void) {
    if (active || ime)
        return;
    if (pending.kind) {
        submit(pending);
        pending = (request){0};
        return;
    }
    int64_t now = clock_ms();
    if (online && (page == LIBRARY || (catalog && catalog->setup)) && !scan_until && now >= library_at) {
        submit((request){.kind = LOAD});
        return;
    }
    if (now >= refresh_at) {
        submit((request){.kind = online ? STATUS : LOAD});
        return;
    }
    if (page == DOWNLOADS && now >= links_at) {
        submit((request){.kind = LINKS});
        return;
    }
    if (!online || (page != LIBRARY && page != DETAIL))
        return;
    int first = selected / PAGE * PAGE;
    for (int i = first; i < first + PAGE && i < visible_count; i++) {
        const game *g = &catalog->games[indices[i]];
        if (find_art(g->id))
            continue;
        request req = {.kind = ART};
        memcpy(req.id, g->id, sizeof(req.id));
        snprintf(req.path, sizeof(req.path), "/icon/%s", g->id);
        submit(req);
        break;
    }
}

static const char *const modes[] = {"external", "internal", "off"};
static const char *const languages[] = {"auto", "en", "es"};

static int *setting_value(settings *s, const setting *f) { return (int *)((char *)s + f->offset); }
static int setting_count(void) { return setting_start[category + 1] - setting_start[category]; }

static void start_keyboard(int user_id) {
    const char *p = page == DOWNLOADS ? entered_url : query;
    size_t len = 0;
    while (*p && len + 2 < sizeof(keyboard) / sizeof(*keyboard)) {
        unsigned cp = utf8_read(&p);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            keyboard[len++] = (wchar_t)(0xd800 | (cp >> 10));
            cp = 0xdc00 | (cp & 0x3ff);
        }
        keyboard[len++] = (wchar_t)cp;
    }
    keyboard[len] = 0;
    static OrbisImeDialogSetting params;
    params = (OrbisImeDialogSetting){
        .userId = user_id,
        .enterLabel = page == DOWNLOADS ? ORBIS_BUTTON_LABEL_DEFAULT : ORBIS_BUTTON_LABEL_SEARCH,
        .type = page == DOWNLOADS ? ORBIS_TYPE_TYPE_URL : ORBIS_TYPE_DEFAULT,
        .maxTextLength = page == DOWNLOADS ? sizeof(keyboard) / sizeof(*keyboard) - 1 : 63,
        .inputTextBuffer = keyboard,
        .posx = WIDTH / 2, .posy = HEIGHT / 2,
        .horizontalAlignment = ORBIS_H_CENTER, .verticalAlignment = ORBIS_V_CENTER
    };
    ime = sceImeDialogInit(&params, NULL) == 0;
    if (!ime)
        say(tr("Could not open the keyboard.", "No se pudo abrir el teclado."));
}

static void close_keyboard(void) {
    OrbisDialogResult result;
    if (sceImeDialogGetResult(&result) == 0 && result.endstatus == ORBIS_DIALOG_OK) {
        char *target = page == DOWNLOADS ? entered_url : query;
        size_t cap = page == DOWNLOADS ? sizeof(entered_url) : sizeof(query), n = 0;
        for (size_t i = 0; i < sizeof(keyboard) / sizeof(*keyboard) - 1 && keyboard[i]; i++) {
            unsigned cp = (uint16_t)keyboard[i];
            if (cp >= 0xd800 && cp <= 0xdbff && (uint16_t)keyboard[i + 1] >= 0xdc00 &&
                (uint16_t)keyboard[i + 1] <= 0xdfff)
                cp = 0x10000 + ((cp - 0xd800) << 10) + ((uint16_t)keyboard[++i] - 0xdc00);
            else if (cp >= 0xd800 && cp <= 0xdfff)
                cp = 0xfffd;
            size_t bytes = utf8_write(cp, target + n, cap - n - 1);
            if (!bytes)
                break;
            n += bytes;
        }
        target[n] = 0;
        if (page == DOWNLOADS) {
            if (*entered_url) {
                dest_ready = 0;
                dests.count = 0;
                pending.kind = DESTS;
            }
        } else {
            rebuild(NULL);
            in_sidebar = !visible_count;
        }
    }
    sceImeDialogTerm();
    ime = 0;
    dirty = 1;
}

static void save_config(void) {
    request req = {.kind = SAVE, .path = "/api/config"};
    size_t used = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        const setting *f = &fields[i];
        int value = *setting_value(&config, f);
        if (value == *setting_value(&original_config, f))
            continue;
        char number[16];
        snprintf(number, sizeof(number), "%d", value);
        const char *key = f->key, *str = number;
        if (!strcmp(key, "language")) {
            key = "lang";
            str = languages[value];
        } else if (!strcmp(key, "dlc_mode")) {
            str = modes[value];
        }
        int n = snprintf(req.form + used, sizeof(req.form) - used, "%s%s=%s", used ? "&" : "", key, str);
        if (n < 0 || (size_t)n >= sizeof(req.form) - used)
            return;
        used += (size_t)n;
    }
    if (used || (catalog && catalog->setup))
        pending = req;
    else
        page = LIBRARY;
}

static void change_game(enum action action, const game *g) {
    const char *verb = action == ADD ? "mount" : action == REMOVE ? "unmount" : "restore";
    request req = {.kind = CHANGE};
    snprintf(req.path, sizeof(req.path), "/api/%s?title_id=%s", verb, g->id);
    pending = req;
    confirm = NONE;
}

static void detail_input(uint32_t buttons) {
    const game *g = focused_game();
    enum action actions[2];
    int n = available_actions(g, online, actions);
    if (buttons & ORBIS_PAD_BUTTON_CIRCLE) {
        if (confirm)
            confirm = NONE;
        else
            page = LIBRARY;
        return;
    }
    if (!n || writing())
        return;
    if (action_row >= n)
        action_row = n - 1;
    if (confirm) {
        if (buttons & ORBIS_PAD_BUTTON_CROSS) {
            for (int i = 0; i < n; i++)
                if (actions[i] == confirm) {
                    change_game(confirm, g);
                    return;
                }
            confirm = NONE;
        }
    } else if ((buttons & ORBIS_PAD_BUTTON_UP) && action_row > 0) {
        action_row--;
    } else if ((buttons & ORBIS_PAD_BUTTON_DOWN) && action_row + 1 < n) {
        action_row++;
    } else if (buttons & ORBIS_PAD_BUTTON_CROSS) {
        if (actions[action_row] == ADD)
            change_game(ADD, g);
        else
            confirm = actions[action_row];
    }
}

static void settings_input(uint32_t buttons) {
    if (writing())
        return;
    if (buttons & ORBIS_PAD_BUTTON_CIRCLE) {
        page = LIBRARY;
        in_sidebar = 1;
        return;
    }
    if (!config_ready) {
        if ((buttons & ORBIS_PAD_BUTTON_CROSS) && active != CONFIG)
            pending.kind = CONFIG;
        return;
    }
    int count = setting_count();
    if (in_sidebar) {
        if ((buttons & ORBIS_PAD_BUTTON_UP) && category > 0)
            category--;
        if ((buttons & ORBIS_PAD_BUTTON_DOWN) && category < 2)
            category++;
        if (buttons & (ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)) {
            in_sidebar = 0;
            setting_row = 0;
        }
        return;
    }
    if ((buttons & ORBIS_PAD_BUTTON_UP) && setting_row > 0)
        setting_row--;
    if ((buttons & ORBIS_PAD_BUTTON_DOWN) && setting_row < count)
        setting_row++;
    if (buttons & ORBIS_PAD_BUTTON_LEFT) {
        in_sidebar = 1;
        return;
    }
    if (buttons & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_RIGHT)) {
        if (setting_row == count) {
            if (buttons & ORBIS_PAD_BUTTON_CROSS)
                save_config();
        } else {
            const setting *f = &fields[setting_start[category] + setting_row];
            int *v = setting_value(&config, f);
            *v = *v < f->max ? *v + 1 : f->min;
        }
    }
}

static void library_input(uint32_t buttons, int user_id) {
    if (buttons & ORBIS_PAD_BUTTON_CIRCLE) {
        quit = 1;
        return;
    }
    if (buttons & ORBIS_PAD_BUTTON_SQUARE) {
        if (!writing())
            pending = online ? (request){.kind = SCAN, .path = "/api/rescan"} : (request){.kind = LOAD};
        return;
    }
    if (buttons & ORBIS_PAD_BUTTON_TRIANGLE) {
        start_keyboard(user_id);
        return;
    }
    if (in_sidebar || !visible_count || !online) {
        in_sidebar = 1;
        if ((buttons & ORBIS_PAD_BUTTON_UP) && sidebar > 0)
            sidebar--;
        if ((buttons & ORBIS_PAD_BUTTON_DOWN) && sidebar < 5)
            sidebar++;
        if (buttons & ORBIS_PAD_BUTTON_CROSS) {
            if (link_writing && (sidebar == 4 || sidebar == 5))
                return;
            if (sidebar == 5) {
                page = SETTINGS;
                config_ready = 0;
                category = 0;
                pending.kind = CONFIG;
                return;
            }
            if (sidebar == 3) {
                page = PHONE;
                OrbisNetCtlInfo info;
                local_url[0] = 0;
                if (sceNetCtlInit() >= 0) {
                    if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) >= 0 && *info.ip_address)
                        snprintf(local_url, sizeof(local_url), "http://%.15s:%d", info.ip_address, API_PORT);
                    sceNetCtlTerm();
                }
                return;
            }
            if (sidebar == 4) {
                page = DOWNLOADS;
                links_at = 0;
                return;
            }
            filter = (enum filter)sidebar;
            rebuild(NULL);
        }
        if (visible_count && online && (buttons & (ORBIS_PAD_BUTTON_RIGHT | ORBIS_PAD_BUTTON_CROSS)))
            in_sidebar = 0;
        return;
    }
    if (buttons & ORBIS_PAD_BUTTON_CROSS) {
        page = DETAIL;
        confirm = NONE;
        action_row = 0;
        return;
    }
    if (buttons & ORBIS_PAD_BUTTON_LEFT) {
        if (selected % COLS == 0) {
            in_sidebar = 1;
            sidebar = filter;
        } else {
            selected--;
        }
    }
    if ((buttons & ORBIS_PAD_BUTTON_RIGHT) && selected + 1 < visible_count)
        selected++;
    if ((buttons & ORBIS_PAD_BUTTON_UP) && selected >= COLS)
        selected -= COLS;
    if ((buttons & ORBIS_PAD_BUTTON_DOWN) && (selected / COLS + 1) * COLS < visible_count) {
        selected += COLS;
        if (selected >= visible_count)
            selected = visible_count - 1;
    }
}

static void downloads_input(uint32_t buttons, int user) {
    if (buttons & ORBIS_PAD_BUTTON_CIRCLE) {
        page = LIBRARY;
        return;
    }
    if (link_writing || !links_ready || !links.configured)
        return;
    if ((buttons & ORBIS_PAD_BUTTON_TRIANGLE) || ((buttons & ORBIS_PAD_BUTTON_CROSS) && !*entered_url)) {
        start_keyboard(user);
        return;
    }
    if ((buttons & ORBIS_PAD_BUTTON_LEFT) && dest > 0)
        dest--;
    if ((buttons & ORBIS_PAD_BUTTON_RIGHT) && dest + 1 < dests.count)
        dest++;
    if (!(buttons & ORBIS_PAD_BUTTON_CROSS))
        return;
    if (!dests.count) {
        pending.kind = DESTS;
        return;
    }
    if ((strncmp(entered_url, "https://", 8) && strncmp(entered_url, "http://", 7)) || strpbrk(entered_url, "\r\n")) {
        say(tr("Enter one HTTP or HTTPS URL.", "Introduce una URL HTTP o HTTPS."));
        return;
    }
    request req = {.kind = ENQUEUE};
    snprintf(req.path, sizeof(req.path), "/api/links/add?dest=%s", dests.items[dest].id);
    snprintf(req.form, sizeof(req.form), "%s", entered_url);
    pending = req;
    link_writing = 1;
}

static void input(uint32_t buttons, int user) {
    dirty = 1;
    if (page == LIBRARY)
        library_input(buttons, user);
    else if (page == DETAIL)
        detail_input(buttons);
    else if (page == SETTINGS)
        settings_input(buttons);
    else if (page == DOWNLOADS)
        downloads_input(buttons, user);
    else if (buttons & ORBIS_PAD_BUTTON_CIRCLE)
        page = LIBRARY;
}

static void draw(canvas c) {
    panel_view view = {
        .page = page, .confirm = confirm, .filter = filter, .spanish = spanish, .online = online,
        .sidebar = sidebar, .selected = selected, .in_sidebar = in_sidebar, .visible_count = visible_count,
        .indices = indices, .counts = counts, .catalog = catalog, .focused = focused_game(), .art = artwork_cache,
        .config = &config, .config_ready = config_ready, .category = category,
        .setting_row = setting_row, .action_row = action_row,
        .links = &links, .dests = &dests, .links_ready = links_ready, .dest_ready = dest_ready,
        .dest = dest, .link_writing = link_writing, .waiting = writing(),
        .loading_library = active == LOAD, .loading_links = active == LINKS,
        .loading_config = active == CONFIG || pending.kind == CONFIG,
        .loading_destinations = active == DESTS || pending.kind == DESTS,
        .query = query, .feedback = feedback, .entered_url = entered_url, .local_url = local_url,
    };
    panel_draw(&view, c);
}

enum {
    ALIGN = 0x200000,
    FRAME_BYTES = WIDTH * HEIGHT * 4,
    STRIDE = (FRAME_BYTES + ALIGN - 1) & ~(ALIGN - 1)
};

static void loop(int video, uint32_t *buffers[2], int pad, int user) {
    int front = -1, submitted = -1;
    uint32_t last = 0;
    int64_t repeat_at = 0;
    const uint32_t arrows = ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN |
                            ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT;
    while (!quit) {
        accept_reply();
        schedule();
        int64_t now = clock_ms();
        if (*feedback && now >= feedback_until) {
            *feedback = 0;
            dirty = 1;
        }
        OrbisPadData state = {0};
        int connected = scePadReadState(pad, &state) >= 0 && state.connected;
        uint32_t buttons = connected ? state.buttons : last;
        if (ime) {
            if (sceImeDialogGetStatus() == ORBIS_DIALOG_STATUS_STOPPED)
                close_keyboard();
        } else if (connected) {
            uint32_t pressed = buttons & ~last;
            if ((buttons & arrows) != (last & arrows))
                repeat_at = now + 320;
            else if ((buttons & arrows) && now >= repeat_at) {
                pressed |= buttons & arrows;
                repeat_at = now + 95;
            }
            if (pressed)
                input(pressed, user);
        }
        last = buttons;
        if (submitted >= 0) {
            OrbisVideoOutFlipStatus status = {0};
            if (sceVideoOutGetFlipStatus(video, &status) >= 0 && !status.numFlipPending) {
                front = submitted;
                submitted = -1;
            }
        }
        if (dirty && submitted < 0 && !quit) {
            int back = front == 0 ? 1 : 0;
            draw((canvas){buffers[back]});
            if (sceVideoOutSubmitFlip(video, back, ORBIS_VIDEO_OUT_FLIP_VSYNC, 0) >= 0) {
                submitted = back;
                dirty = 0;
            }
        }
        usleep(16000);
    }
    for (int i = 0; submitted >= 0 && i < 200; i++) {
        OrbisVideoOutFlipStatus status = {0};
        if (sceVideoOutGetFlipStatus(video, &status) >= 0 && !status.numFlipPending)
            break;
        usleep(1000);
    }
    if (ime)
        sceImeDialogTerm();
}

static int video_loop(int pad, int user) {
    int video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
    if (video < 0)
        return -1;
    off_t offset;
    void *memory = NULL;
    int rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), STRIDE * 2, ALIGN, 3, &offset);
    if (rc >= 0) {
        rc = sceKernelMapDirectMemory(&memory, STRIDE * 2, 0x33, 0, offset, ALIGN);
        if (rc >= 0) {
            uint32_t *buffers[] = {memory, (uint32_t *)((char *)memory + STRIDE)};
            void *addresses[] = {buffers[0], buffers[1]};
            OrbisVideoOutBufferAttribute attributes;
            sceVideoOutSetBufferAttribute(&attributes, 0x80000000, ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
                                          ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9, WIDTH, HEIGHT, WIDTH);
            rc = sceVideoOutRegisterBuffers(video, 0, addresses, 2, &attributes);
            if (rc >= 0) {
                sceSystemServiceHideSplashScreen();
                loop(video, buffers, pad, user);
                sceVideoOutUnregisterBuffers(video, 0);
            }
            sceVideoOutClose(video);
            video = -1;
            munmap(memory, STRIDE * 2);
        }
        sceKernelReleaseDirectMemory(offset, STRIDE * 2);
    }
    if (video >= 0)
        sceVideoOutClose(video);
    return rc;
}

int main(void) {
    int32_t user = -1;
    int language = 1;
    OrbisUserServiceInitializeParams user_params = {.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST};
    sceUserServiceInitialize(&user_params);
    sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_LANG, &language);
    spanish = language == 3 || language == 20;
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_FREETYPE_OL);
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_IME_DIALOG);
    if (sceUserServiceGetInitialUser(&user) < 0 ||
        scePadInit() < 0 || font_open("/app0/Roboto-Regular.ttf"))
        return 1;
    int pad = scePadOpen(user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
    pthread_t thread;
    if (pad < 0 || pthread_create(&thread, NULL, worker, NULL)) {
        if (pad >= 0)
            scePadClose(pad);
        font_close();
        return 1;
    }
    int rc = video_loop(pad, user);
    pthread_mutex_lock(&mutex);
    quit_worker = 1;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&mutex);
    pthread_join(thread, NULL);
    scePadClose(pad);
    clear_art();
    free(catalog);
    free(reply.catalog);
    free(reply.cover.pixels);
    font_close();
    return rc < 0;
}
