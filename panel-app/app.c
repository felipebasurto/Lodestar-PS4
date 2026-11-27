/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef PANEL_HOST_TEST
#include "panel_host_hooks.h"
#endif

static const char *tr(const panel_app *app, const char *en, const char *es) {
    return app->spanish ? es : en;
}

const char *panel_task_name(enum panel_task kind) {
    switch (kind) {
    case TASK_IDLE: return "idle";
    case TASK_LOAD: return "load";
    case TASK_STATUS: return "status";
    case TASK_SCAN: return "scan";
    case TASK_CONFIG: return "config";
    case TASK_SAVE: return "save";
    case TASK_CHANGE: return "change";
    case TASK_ART: return "art";
    case TASK_LINKS: return "links";
    case TASK_DESTS: return "dests";
    case TASK_ENQUEUE: return "enqueue";
    case TASK_ADDR: return "addr";
    default: return "unknown";
    }
}

void panel_result_free(panel_result *r) {
    if (!r)
        return;
    free(r->catalog);
    free(r->cover.pixels);
    free(r->cover.thumb64);
    *r = (panel_result){0};
}

static int task_is_write(enum panel_task kind) {
    return kind == TASK_CHANGE || kind == TASK_SAVE || kind == TASK_ENQUEUE || kind == TASK_SCAN;
}

static int writing(const panel_app *app) {
    return app->active == TASK_CHANGE || app->active == TASK_SAVE ||
           app->pending_write.kind == TASK_CHANGE || app->pending_write.kind == TASK_SAVE ||
           app->link_writing;
}

static void queue_write(panel_app *app, panel_request req) {
    if (app->quit)
        return;
    app->pending_write = req;
    app->dirty = 1;
}

static void queue_read(panel_app *app, panel_request req) {
    if (app->quit)
        return;
    app->pending_read = req;
    app->dirty = 1;
}

static void say(panel_app *app, const char *message, int64_t now) {
    snprintf(app->feedback, sizeof(app->feedback), "%s", message ? message : "");
    app->feedback_until = now + 6500;
    app->dirty = 1;
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

const game *panel_app_focused(const panel_app *app) {
    return app->catalog && app->selected >= 0 && app->selected < app->visible_count
               ? &app->catalog->games[app->indices[app->selected]]
               : NULL;
}

/* qsort comparator needs context; sort using a tiny in-place merge/shell sort or thread-local pointer.
 * Shell sort with Ciura gaps is ~15 lines, O(n log n) on <=4096 games, zero global state, 100% thread-safe! */
static int game_less(const library *cat, int ia, int ib) {
    const game *ga = &cat->games[ia], *gb = &cat->games[ib];
    int n = strcasecmp(ga->name, gb->name);
    return n ? n < 0 : strcmp(ga->id, gb->id) < 0;
}

static void sort_indices(const library *cat, int *indices, int count) {
    static const int gaps[] = {1750, 701, 301, 132, 57, 23, 10, 4, 1};
    for (size_t g = 0; g < sizeof(gaps) / sizeof(*gaps); g++) {
        int gap = gaps[g];
        for (int i = gap; i < count; i++) {
            int tmp = indices[i];
            int j = i;
            while (j >= gap && game_less(cat, tmp, indices[j - gap])) {
                indices[j] = indices[j - gap];
                j -= gap;
            }
            indices[j] = tmp;
        }
    }
}

static void rebuild(panel_app *app, const char *keep) {
    int previous = app->selected;
    app->visible_count = 0;
    memset(app->counts, 0, sizeof(app->counts));
    for (int i = 0; app->catalog && i < app->catalog->count; i++) {
        const game *g = &app->catalog->games[i];
        if (!contains(g->name, app->query) && !contains(g->id, app->query))
            continue;
        for (int s = 0; s < SECTIONS; s++)
            app->counts[s] += lane_in_section(g, (enum section)s);
        if (lane_in_section(g, app->section))
            app->indices[app->visible_count++] = i;
    }
    if (app->visible_count > 1)
        sort_indices(app->catalog, app->indices, app->visible_count);
    app->selected = 0;
    for (int i = 0; keep && i < app->visible_count; i++) {
        if (!strcmp(keep, app->catalog->games[app->indices[i]].id)) {
            app->selected = i;
            return;
        }
    }
    /* The game left this section: stay where the controller was. */
    if (keep && *keep && app->visible_count)
        app->selected = previous < app->visible_count ? previous : app->visible_count - 1;
    if (app->page == DETAIL)
        app->page = LIBRARY;
}

static artwork *find_art(panel_app *app, const char *id) {
    for (size_t i = 0; i < ART_SLOTS; i++)
        if (!strcmp(id, app->artwork_cache[i].id))
            return &app->artwork_cache[i];
    return NULL;
}

static void clear_art(panel_app *app) {
    for (size_t i = 0; i < ART_SLOTS; i++) {
        free(app->artwork_cache[i].pixels);
        free(app->artwork_cache[i].thumb64);
        app->artwork_cache[i] = (artwork){0};
    }
}

static void *worker_main(void *arg) {
    panel_app *app = arg;
    pthread_mutex_lock(&app->mutex);
    for (;;) {
        while (!app->mailbox.kind && !app->quit_worker)
            pthread_cond_wait(&app->wake, &app->mutex);
        if (app->quit_worker)
            break;
        panel_request req = app->mailbox;
        app->mailbox = (panel_request){0};
        pthread_mutex_unlock(&app->mutex);

        panel_result r = {0};
        if (app->plat.perform && !__atomic_load_n(&app->stop_flag, __ATOMIC_ACQUIRE))
            r = app->plat.perform(app->plat.ctx, &req, &app->stop_flag);
        r.kind = req.kind;

        pthread_mutex_lock(&app->mutex);
        if (app->quit_worker) {
            panel_result_free(&r);
            break;
        }
        panel_result_free(&app->reply);
        app->reply = r;
#ifdef PANEL_HOST_TEST
        pthread_cond_broadcast(&panel_host_reply_cond);
#endif
    }
    pthread_mutex_unlock(&app->mutex);
    return NULL;
}

static void submit(panel_app *app, panel_request req) {
    if (app->quit || !app->worker_running)
        return;
    if (app->log && req.kind != TASK_ART) {
        char safe[128];
        applog_safe_path(req.path, safe, sizeof(safe));
        applog_write(app->log, "api", "submit kind=%s path=%s form_len=%zu",
                     panel_task_name(req.kind), safe, strlen(req.form));
    }
    pthread_mutex_lock(&app->mutex);
    app->mailbox = req;
    pthread_cond_signal(&app->wake);
    pthread_mutex_unlock(&app->mutex);
    app->active = req.kind;
}

void panel_app_request_quit(panel_app *app, const char *reason) {
    if (!app || app->quit)
        return;
    app->quit = 1;
    app->quit_reason = reason ? reason : "unknown";
    __atomic_store_n(&app->stop_flag, 1, __ATOMIC_RELEASE);
    if (app->log)
        applog_write(app->log, "exit", "quit requested reason=%s page=%d wheel=%d ime=%d",
                     app->quit_reason, (int)app->page, app->wheel, app->ime);
}

int panel_app_init(panel_app *app, panel_platform plat, applog *log, int spanish) {
    if (!app)
        return -1;
    memset(app, 0, sizeof(*app));
    app->plat = plat;
    app->log = log;
    app->spanish = spanish ? 1 : 0;
    app->section = SECTION_ALL;
    app->wheel_at = -1;
    app->dirty = 1;
    if (pthread_mutex_init(&app->mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&app->wake, NULL) != 0) {
        pthread_mutex_destroy(&app->mutex);
        return -1;
    }
    app->sync_ready = 1;
    return 0;
}

int panel_app_start_worker(panel_app *app) {
    if (!app || !app->sync_ready || app->worker_running)
        return -1;
    app->quit_worker = 0;
    __atomic_store_n(&app->stop_flag, 0, __ATOMIC_RELEASE);
    if (pthread_create(&app->worker, NULL, worker_main, app) != 0)
        return -1;
    app->worker_running = 1;
    return 0;
}

void panel_app_stop_worker(panel_app *app) {
    if (!app || !app->sync_ready)
        return;
    __atomic_store_n(&app->stop_flag, 1, __ATOMIC_RELEASE);
    if (app->worker_running) {
        pthread_mutex_lock(&app->mutex);
        app->quit_worker = 1;
        pthread_cond_signal(&app->wake);
        pthread_mutex_unlock(&app->mutex);
        pthread_join(app->worker, NULL);
        app->worker_running = 0;
    }
    pthread_mutex_lock(&app->mutex);
    panel_result_free(&app->reply);
    app->mailbox = (panel_request){0};
    pthread_mutex_unlock(&app->mutex);
    app->active = TASK_IDLE;
}

void panel_app_destroy(panel_app *app) {
    if (!app)
        return;
    panel_app_stop_worker(app);
    if (app->ime) {
        if (app->plat.ime_close)
            app->plat.ime_close(app->plat.ctx);
        app->ime = 0;
        app->ime_target = IME_NONE;
    }
    clear_art(app);
    free(app->catalog);
    app->catalog = NULL;
    if (app->sync_ready) {
        pthread_cond_destroy(&app->wake);
        pthread_mutex_destroy(&app->mutex);
        app->sync_ready = 0;
    }
}

static void accept_reply(panel_app *app, int64_t now) {
    if (!app->sync_ready)
        return;
    pthread_mutex_lock(&app->mutex);
    if (!app->reply.kind) {
        pthread_mutex_unlock(&app->mutex);
        return;
    }
    panel_result r = app->reply;
    app->reply = (panel_result){0};
    pthread_mutex_unlock(&app->mutex);
    app->active = TASK_IDLE;

    if (app->quit) {
        panel_result_free(&r);
        return;
    }

    if (app->log && (r.kind != TASK_ART || !r.cover.pixels)) {
        applog_write(app->log, "api", "done kind=%s ok=%d status=%d ms=%lld",
                     panel_task_name(r.kind), r.ok, r.http_status, (long long)r.elapsed_ms);
    }

    app->dirty = 1;
    switch (r.kind) {
    case TASK_LOAD:
        app->online = r.ok;
        if (r.ok && r.catalog) {
            const game *g = panel_app_focused(app);
            char keep[10] = "";
            if (g)
                memcpy(keep, g->id, sizeof(keep));
            free(app->catalog);
            app->catalog = r.catalog;
            r.catalog = NULL;
            app->spanish = app->catalog->spanish;
            app->catalog_gen++;
            rebuild(app, keep);
            app->library_at = now + (app->catalog->setup ? 30000 : 45000);
            /* Retry previously unavailable icons after the daemon's next library read. */
            for (size_t i = 0; i < ART_SLOTS; i++)
                if (!app->artwork_cache[i].pixels)
                    app->artwork_cache[i].id[0] = 0;
            if (app->log)
                applog_write(app->log, "api", "catalog count=%d visible=%d setup=%d",
                             app->catalog->count, app->visible_count, app->catalog->setup);
        }
        app->refresh_at = now + (!app->online ? 3000 : app->scan_until ? 1000 : 15000);
        break;
    case TASK_STATUS:
        if (app->scan_until && (now >= app->scan_until || (!r.ok && app->scan_seen))) {
            app->scan_until = 0;
            if (!app->pending_read.kind)
                queue_read(app, (panel_request){.kind = TASK_LOAD});
        }
        if (r.ok >= 0)
            app->scan_seen = r.ok;
        if (r.ok < 0)
            app->online = 0;
        app->refresh_at = now + (app->scan_until ? 1000 : 15000);
        break;
    case TASK_LINKS:
        app->links_ready = r.ok;
        if (r.ok) {
            app->links = r.links;
            app->links_gen++;
        }
        app->links_at = now + 2500;
        break;
    case TASK_DESTS:
        app->dest = 0;
        app->dest_ready = r.ok;
        app->dests = r.dests;
        app->dests_gen++;
        if (!r.ok)
            say(app, tr(app, "Could not read destinations.", "No se pudieron leer los destinos."), now);
        break;
    case TASK_ENQUEUE:
        app->link_writing = 0;
        if (r.ok) {
            *app->entered_url = 0;
            app->links_at = 0;
        }
        break;
    case TASK_CONFIG:
        app->config_ready = r.ok;
        app->config = app->original_config = r.config;
        app->config_gen++;
        if (!r.ok)
            say(app, tr(app, "Could not read settings. Press Cross to retry.",
                        "No se pudieron leer los ajustes. Pulsa X para reintentar."), now);
        break;
    case TASK_ART: {
        artwork *slot = find_art(app, r.cover.id);
        if (!slot)
            slot = &app->artwork_cache[app->next_art++ % ART_SLOTS];
        free(slot->pixels);
        free(slot->thumb64);
        *slot = r.cover;
        r.cover.pixels = NULL;
        r.cover.thumb64 = NULL;
        slot->gen = ++app->art_slot_gen;
        app->art_gen++;
        memcpy(app->art_last_id, slot->id, sizeof(app->art_last_id));
        break;
    }
    case TASK_ADDR:
        if (r.ok)
            snprintf(app->local_url, sizeof(app->local_url), "%s", r.local_url);
        else
            app->local_url[0] = 0;
        break;
    case TASK_SCAN:
        if (r.ok) {
            app->scan_seen = 0;
            app->scan_until = now + 20000;
            app->refresh_at = now + 1000;
            /* A short scan can begin and end between status reads.
             * Don't clobber an explicit page read already waiting in pending_read. */
            if (!app->pending_read.kind)
                queue_read(app, (panel_request){.kind = TASK_LOAD, .path = "/api/games"});
            else
                app->library_at = now;
        }
        break;
    case TASK_CHANGE:
        if (r.ok) {
            app->page = LIBRARY;
            queue_read(app, (panel_request){.kind = TASK_LOAD});
        }
        break;
    case TASK_SAVE:
        if (r.ok) {
            app->online = 1;
            app->config_ready = 0;
            app->page = LIBRARY;
            queue_read(app, (panel_request){.kind = TASK_LOAD});
        }
        break;
    default:
        break;
    }
    if (task_is_write(r.kind))
        say(app, *r.message ? r.message : tr(app, "No response from Lodestar. Check the library before retrying.",
                                             "Lodestar no respondió. Comprueba la biblioteca antes de reintentar."),
            now);
    panel_result_free(&r);
}

static void schedule(panel_app *app, int64_t now) {
    if (app->quit || app->active || app->ime || !app->worker_running)
        return;
    if (app->pending_write.kind) {
        panel_request req = app->pending_write;
        app->pending_write = (panel_request){0};
        submit(app, req);
        return;
    }
    if (app->pending_read.kind) {
        panel_request req = app->pending_read;
        app->pending_read = (panel_request){0};
        submit(app, req);
        return;
    }
    if (app->online && (app->page == LIBRARY || (app->catalog && app->catalog->setup)) &&
        !app->scan_until && now >= app->library_at) {
        submit(app, (panel_request){.kind = TASK_LOAD, .path = "/api/games"});
        return;
    }
    if (now >= app->refresh_at) {
        submit(app, app->online ? (panel_request){.kind = TASK_STATUS, .path = "/api/status"}
                                : (panel_request){.kind = TASK_LOAD, .path = "/api/games"});
        return;
    }
    if (app->page == DOWNLOADS && now >= app->links_at) {
        submit(app, (panel_request){.kind = TASK_LINKS, .path = "/api/links"});
        return;
    }
    /* Pause background cover fetches while L2 wheel or a button hold is active. */
    if (!app->online || app->wheel || app->held.action ||
        (app->page != LIBRARY && app->page != DETAIL) || !app->visible_count)
        return;
    /* The focused game first, then the rows around it. */
    int first = app->selected > 2 ? app->selected - 2 : 0;
    for (int i = -1; i < PAGE; i++) {
        int at = i < 0 ? app->selected : first + i;
        if (at >= app->visible_count)
            break;
        const game *g = &app->catalog->games[app->indices[at]];
        if (find_art(app, g->id))
            continue;
        panel_request req = {.kind = TASK_ART};
        memcpy(req.id, g->id, sizeof(req.id));
        snprintf(req.path, sizeof(req.path), "/icon/%s", g->id);
        submit(app, req);
        break;
    }
}

static const char *const modes[] = {"external", "internal", "off"};
static const char *const languages[] = {"auto", "en", "es"};

static int *setting_value(settings *s, const setting *f) { return (int *)((char *)s + f->offset); }
static int setting_count(const panel_app *app) { return setting_start[app->category + 1] - setting_start[app->category]; }

static void start_keyboard(panel_app *app, int64_t now) {
    enum ime_target target = app->page == DOWNLOADS ? IME_URL : IME_SEARCH;
    const char *initial = target == IME_URL ? app->entered_url : app->query;
    size_t max_chars = target == IME_URL ? 511 : 63;
    if (app->plat.ime_open && app->plat.ime_open(app->plat.ctx, target, initial, max_chars) == 0) {
        app->ime = 1;
        app->ime_target = target;
        app->held.action = NONE;
        if (app->log)
            applog_write(app->log, "input", "ime open target=%s", target == IME_URL ? "url" : "search");
    } else {
        if (app->log)
            applog_write(app->log, "error", "ime open failed target=%s", target == IME_URL ? "url" : "search");
        say(app, tr(app, "Could not open the keyboard.", "No se pudo abrir el teclado."), now);
    }
}

static void poll_keyboard(panel_app *app, int64_t now) {
    if (!app->ime || !app->plat.ime_poll)
        return;
    char buf[sizeof(app->entered_url)];
    buf[0] = 0;
    enum ime_status st = app->plat.ime_poll(app->plat.ctx, buf, sizeof(buf));
    if (st == IME_RUNNING)
        return;
    enum ime_target target = app->ime_target;
    if (app->plat.ime_close)
        app->plat.ime_close(app->plat.ctx);
    app->ime = 0;
    app->ime_target = IME_NONE;
    app->ime_cooldown_until = now + 220;
    app->dirty = 1;

    if (app->log)
        applog_write(app->log, "input", "ime close target=%s status=%s len=%zu",
                     target == IME_URL ? "url" : "search",
                     st == IME_OK ? "ok" : "canceled", strlen(buf));

    if (st == IME_OK) {
        if (target == IME_URL) {
            snprintf(app->entered_url, sizeof(app->entered_url), "%s", buf);
            if (*app->entered_url) {
                app->dest_ready = 0;
                app->dests.count = 0;
                app->dests_gen++;
                queue_read(app, (panel_request){.kind = TASK_DESTS, .path = "/api/dests"});
            }
        } else if (target == IME_SEARCH) {
            snprintf(app->query, sizeof(app->query), "%s", buf);
            app->selected = 0;
            rebuild(app, NULL);
        }
    }
}

static void save_config(panel_app *app) {
    panel_request req = {.kind = TASK_SAVE, .path = "/api/config"};
    size_t used = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); i++) {
        const setting *f = &fields[i];
        int value = *setting_value(&app->config, f);
        if (value == *setting_value(&app->original_config, f))
            continue;
        char number[16];
        snprintf(number, sizeof(number), "%d", value);
        const char *key = f->key, *str = number;
        if (!strcmp(key, "language")) {
            key = "lang";
            str = languages[value >= 0 && value <= 2 ? value : 0];
        } else if (!strcmp(key, "dlc_mode")) {
            str = modes[value >= 0 && value <= 2 ? value : 0];
        }
        int n = snprintf(req.form + used, sizeof(req.form) - used, "%s%s=%s", used ? "&" : "", key, str);
        if (n < 0 || (size_t)n >= sizeof(req.form) - used)
            return;
        used += (size_t)n;
    }
    if (used || (app->catalog && app->catalog->setup)) {
        if (app->log)
            applog_write(app->log, "input", "save_config bytes=%zu", used);
        queue_write(app, req);
    } else {
        app->page = LIBRARY;
    }
}

static void change_game(panel_app *app, enum action action, const game *g) {
    if (!g)
        return;
    const char *verb = action == ADD ? "mount" : action == REMOVE ? "unmount" : "restore";
    panel_request req = {.kind = TASK_CHANGE};
    snprintf(req.path, sizeof(req.path), "/api/%s?title_id=%s", verb, g->id);
    if (app->log)
        applog_write(app->log, "hold", "commit action=%s title_id=%s", verb, g->id);
    queue_write(app, req);
}

/* Cross adds or removes, Square restores; both only after being held (PANEL-8). */
static void press_action(panel_app *app, uint32_t button, int64_t now) {
    const game *g = panel_app_focused(app);
    if (writing(app) || app->pending_write.kind)
        return;
    enum action actions[2];
    for (int i = 0, n = available_actions(g, app->online, actions); i < n; i++) {
        if ((button == PANEL_BTN_SQUARE) == (actions[i] == RESTORE)) {
            hold_press(&app->held, actions[i], g, now);
            app->held_button = button;
            if (app->log)
                applog_write(app->log, "hold", "start action=%d title_id=%s", (int)actions[i], g->id);
        }
    }
}

static void detail_input(panel_app *app, uint32_t buttons, int64_t now) {
    if (buttons & (PANEL_BTN_CIRCLE | PANEL_BTN_TRIANGLE)) {
        app->page = LIBRARY;
        app->held.action = NONE;
        if (app->log)
            applog_write(app->log, "input", "page=library (from detail)");
    } else if (buttons & PANEL_BTN_CROSS) {
        press_action(app, PANEL_BTN_CROSS, now);
    } else if (buttons & PANEL_BTN_SQUARE) {
        press_action(app, PANEL_BTN_SQUARE, now);
    }
}

static void settings_input(panel_app *app, uint32_t buttons) {
    if (writing(app))
        return;
    if (buttons & PANEL_BTN_CIRCLE) {
        app->page = LIBRARY;
        if (app->log)
            applog_write(app->log, "input", "page=library (from settings)");
        return;
    }
    if (!app->config_ready) {
        if ((buttons & PANEL_BTN_CROSS) && app->active != TASK_CONFIG)
            queue_read(app, (panel_request){.kind = TASK_CONFIG, .path = "/api/config"});
        return;
    }
    int count = setting_count(app);
    if (app->in_sidebar) {
        if ((buttons & PANEL_BTN_UP) && app->category > 0)
            app->category--;
        if ((buttons & PANEL_BTN_DOWN) && app->category < 2)
            app->category++;
        if (buttons & (PANEL_BTN_RIGHT | PANEL_BTN_CROSS)) {
            app->in_sidebar = 0;
            app->setting_row = 0;
        }
        return;
    }
    if ((buttons & PANEL_BTN_UP) && app->setting_row > 0)
        app->setting_row--;
    if ((buttons & PANEL_BTN_DOWN) && app->setting_row < count)
        app->setting_row++;
    if (buttons & PANEL_BTN_LEFT) {
        app->in_sidebar = 1;
        return;
    }
    if (buttons & (PANEL_BTN_CROSS | PANEL_BTN_RIGHT)) {
        if (app->setting_row == count) {
            if (buttons & PANEL_BTN_CROSS)
                save_config(app);
        } else {
            const setting *f = &fields[setting_start[app->category] + app->setting_row];
            int *v = setting_value(&app->config, f);
            *v = *v < f->max ? *v + 1 : f->min;
        }
    }
}

static void library_input(panel_app *app, uint32_t buttons, int64_t now) {
    if (buttons & PANEL_BTN_CIRCLE) {
        if (*app->query) {
            *app->query = 0;
            app->selected = 0;
            rebuild(app, NULL);
            if (app->log)
                applog_write(app->log, "input", "search cleared");
        } else {
            panel_app_request_quit(app, "circle");
        }
        return;
    }
    if (!app->online) {
        if ((buttons & PANEL_BTN_CROSS) && !writing(app))
            queue_read(app, (panel_request){.kind = TASK_LOAD, .path = "/api/games"});
        return;
    }
    if (buttons & (PANEL_BTN_L1 | PANEL_BTN_R1)) {
        app->section = (enum section)((app->section + ((buttons & PANEL_BTN_R1) ? 1 : SECTIONS - 1)) % SECTIONS);
        app->selected = 0;
        app->held.action = NONE;
        rebuild(app, NULL);
        if (app->log)
            applog_write(app->log, "input", "section=%d visible=%d", (int)app->section, app->visible_count);
        return;
    }
    if (!app->visible_count)
        return;
    if ((buttons & PANEL_BTN_UP) && app->selected > 0)
        app->selected--;
    if ((buttons & PANEL_BTN_DOWN) && app->selected + 1 < app->visible_count)
        app->selected++;
    if (buttons & PANEL_BTN_LEFT)
        app->selected = lane_jump(app->catalog, app->indices, app->visible_count, app->selected, -1);
    if (buttons & PANEL_BTN_RIGHT)
        app->selected = lane_jump(app->catalog, app->indices, app->visible_count, app->selected, 1);
    if (buttons & PANEL_BTN_TRIANGLE) {
        app->page = DETAIL;
        app->held.action = NONE;
        if (app->log && panel_app_focused(app))
            applog_write(app->log, "input", "page=detail title_id=%s", panel_app_focused(app)->id);
    } else if (buttons & PANEL_BTN_CROSS) {
        press_action(app, PANEL_BTN_CROSS, now);
    } else if (buttons & PANEL_BTN_SQUARE) {
        press_action(app, PANEL_BTN_SQUARE, now);
    }
}

/* The wheel, clockwise from the top: scan, send, transfers, settings, search, exit. */
static void wheel_run(panel_app *app, int64_t now) {
    int chosen = app->wheel_at;
    app->wheel = 0;
    app->wheel_at = -1;
    app->dirty = 1;
    if (app->log)
        applog_write(app->log, "wheel", "release chosen=%d", chosen);
    if (writing(app) && (chosen == 0 || chosen == 2 || chosen == 3))
        return;
    if (chosen == 0) {
        if (app->online)
            queue_write(app, (panel_request){.kind = TASK_SCAN, .path = "/api/rescan"});
        else
            queue_read(app, (panel_request){.kind = TASK_LOAD, .path = "/api/games"});
        app->page = LIBRARY;
    } else if (chosen == 1) {
        app->page = PHONE;
        queue_read(app, (panel_request){.kind = TASK_ADDR});
    } else if (chosen == 2) {
        app->page = DOWNLOADS;
        app->links_at = 0;
    } else if (chosen == 3) {
        app->page = SETTINGS;
        app->config_ready = 0;
        app->category = 0;
        app->in_sidebar = 1;
        app->setting_row = 0;
        queue_read(app, (panel_request){.kind = TASK_CONFIG, .path = "/api/config"});
    } else if (chosen == 4) {
        app->page = LIBRARY;
        start_keyboard(app, now);
    } else if (chosen == 5) {
        panel_app_request_quit(app, "wheel");
    }
}

static void wheel_input(panel_app *app, uint32_t buttons, int64_t now) {
    if (buttons & PANEL_BTN_CIRCLE) {
        app->wheel = 0;
        app->wheel_at = -1;
        if (app->log)
            applog_write(app->log, "wheel", "cancel");
    } else if (buttons & (PANEL_BTN_RIGHT | PANEL_BTN_DOWN)) {
        app->wheel_at = app->wheel_at < 0 ? 0 : (app->wheel_at + 1) % WHEEL_ITEMS;
    } else if (buttons & (PANEL_BTN_LEFT | PANEL_BTN_UP)) {
        app->wheel_at = app->wheel_at < 0 ? 0 : (app->wheel_at + WHEEL_ITEMS - 1) % WHEEL_ITEMS;
    } else if ((buttons & PANEL_BTN_CROSS) && app->wheel_at >= 0) {
        wheel_run(app, now);
    }
}

static void downloads_input(panel_app *app, uint32_t buttons, int64_t now) {
    if (buttons & PANEL_BTN_CIRCLE) {
        app->page = LIBRARY;
        if (app->log)
            applog_write(app->log, "input", "page=library (from downloads)");
        return;
    }
    if (app->link_writing || !app->links_ready || !app->links.configured)
        return;
    if ((buttons & PANEL_BTN_TRIANGLE) || ((buttons & PANEL_BTN_CROSS) && !*app->entered_url)) {
        start_keyboard(app, now);
        return;
    }
    if ((buttons & PANEL_BTN_LEFT) && app->dest > 0)
        app->dest--;
    if ((buttons & PANEL_BTN_RIGHT) && app->dest + 1 < app->dests.count)
        app->dest++;
    if (!(buttons & PANEL_BTN_CROSS))
        return;
    if (!app->dests.count) {
        queue_read(app, (panel_request){.kind = TASK_DESTS, .path = "/api/dests"});
        return;
    }
    if ((strncmp(app->entered_url, "https://", 8) && strncmp(app->entered_url, "http://", 7)) ||
        strpbrk(app->entered_url, "\r\n")) {
        say(app, tr(app, "Enter one HTTP or HTTPS URL.", "Introduce una URL HTTP o HTTPS."), now);
        return;
    }
    int d = app->dest >= 0 && app->dest < app->dests.count ? app->dest : 0;
    panel_request req = {.kind = TASK_ENQUEUE};
    snprintf(req.path, sizeof(req.path), "/api/links/add?dest=%s", app->dests.items[d].id);
    snprintf(req.form, sizeof(req.form), "%s", app->entered_url);
    app->link_writing = 1;
    if (app->log)
        applog_write(app->log, "input", "enqueue dest=%s url_len=%zu",
                     app->dests.items[d].id, strlen(app->entered_url));
    queue_write(app, req);
}

static void handle_buttons(panel_app *app, uint32_t buttons, int64_t now) {
    app->dirty = 1;
    if (app->wheel) {
        wheel_input(app, buttons, now);
        return;
    }
    if (buttons & PANEL_BTN_L2) {
        app->wheel = 1;
        app->wheel_at = -1;
        if (app->held.action && app->log)
            applog_write(app->log, "hold", "cancel reason=wheel");
        app->held.action = NONE;
        if (app->log)
            applog_write(app->log, "wheel", "open page=%d", (int)app->page);
        return;
    }
    if (app->page == LIBRARY)
        library_input(app, buttons, now);
    else if (app->page == DETAIL)
        detail_input(app, buttons, now);
    else if (app->page == SETTINGS)
        settings_input(app, buttons);
    else if (app->page == DOWNLOADS)
        downloads_input(app, buttons, now);
    else if (buttons & PANEL_BTN_CIRCLE) {
        app->page = LIBRARY;
        if (app->log)
            applog_write(app->log, "input", "page=library (from phone)");
    }
}

void panel_app_tick(panel_app *app, const panel_pad *pad, int64_t now) {
    if (!app || app->quit)
        return;

    accept_reply(app, now);
    if (app->quit)
        return;
    schedule(app, now);

    if (*app->feedback && now >= app->feedback_until) {
        *app->feedback = 0;
        app->dirty = 1;
    }

    int connected = pad && pad->connected;
    uint32_t buttons = connected ? pad->buttons : app->last_buttons;
    int stick_x = connected ? pad->lx : 0;
    int stick_y = connected ? pad->ly : 0;

    if (app->wheel) {
        int at = wheel_point(stick_x, stick_y);
        if (at < 0 && connected)
            at = wheel_point(pad->rx, pad->ry);
        if (at >= 0 && at != app->wheel_at) {
            app->wheel_at = at;
            app->dirty = 1;
        }
    } else if (connected) {
        /* The left stick walks the menus like the arrows. */
        buttons |= (stick_y < -72 ? PANEL_BTN_UP : 0) | (stick_y > 72 ? PANEL_BTN_DOWN : 0) |
                   (stick_x < -96 ? PANEL_BTN_LEFT : 0) | (stick_x > 96 ? PANEL_BTN_RIGHT : 0);
    }

    const uint32_t arrows = PANEL_BTN_UP | PANEL_BTN_DOWN | PANEL_BTN_LEFT | PANEL_BTN_RIGHT;
    if (app->ime) {
        poll_keyboard(app, now);
    } else if (connected && now >= app->ime_cooldown_until) {
        uint32_t pressed = buttons & ~app->last_buttons;
        if ((buttons & arrows) != (app->last_buttons & arrows))
            app->repeat_at = now + 320;
        else if ((buttons & arrows) && now >= app->repeat_at) {
            pressed |= buttons & arrows;
            app->repeat_at = now + 95;
        }
        if (pressed)
            handle_buttons(app, pressed, now);
        if (!app->quit && app->wheel && !(buttons & PANEL_BTN_L2))
            wheel_run(app, now);
    }
    app->last_buttons = buttons;

    if (!app->quit && app->held.action) {
        /* After accept_reply: a library read just now may have moved another game under the button. */
        const game *g = panel_app_focused(app);
        enum action was = app->held.action;
        int down = connected && !app->ime && !app->wheel &&
                   (app->page == LIBRARY || app->page == DETAIL) &&
                   (buttons & app->held_button);
        enum action due = hold_tick(&app->held, g, app->online && !writing(app), down, now);
        if (due) {
            change_game(app, due, g);
        } else if (was && !app->held.action && app->log) {
            applog_write(app->log, "hold", "cancel reason=%s",
                         !down ? "released" : !g ? "empty" : "changed");
        }
        app->dirty = 1;
    }
}

void panel_app_build_view(const panel_app *app, int64_t now, panel_view *out) {
    memset(out, 0, sizeof(*out));
    out->page = app->page;
    out->section = app->section;
    out->spanish = app->spanish;
    out->online = app->online;
    out->selected = app->selected;
    out->in_sidebar = app->in_sidebar;
    out->visible_count = app->visible_count;
    out->indices = app->indices;
    out->counts = app->counts;
    out->catalog = app->catalog;
    out->focused = panel_app_focused(app);
    out->art = app->artwork_cache;
    out->config = &app->config;
    out->config_ready = app->config_ready;
    out->category = app->category;
    out->setting_row = app->setting_row;
    out->holding = app->held.action;
    out->held = hold_progress(&app->held, now);
    out->wheel = app->wheel;
    out->wheel_at = app->wheel_at;
    out->links = &app->links;
    out->dests = &app->dests;
    out->links_ready = app->links_ready;
    out->dest_ready = app->dest_ready;
    out->dest = app->dest;
    out->link_writing = app->link_writing;
    out->waiting = writing(app);
    out->loading_library = app->active == TASK_LOAD;
    out->loading_links = app->active == TASK_LINKS;
    out->loading_config = app->active == TASK_CONFIG || app->pending_read.kind == TASK_CONFIG;
    out->loading_destinations = app->active == TASK_DESTS || app->pending_read.kind == TASK_DESTS;
    out->query = app->query;
    out->feedback = app->feedback;
    out->entered_url = app->entered_url;
    out->local_url = app->local_url;
    out->catalog_gen = app->catalog_gen;
    out->art_gen = app->art_gen;
    out->links_gen = app->links_gen;
    out->config_gen = app->config_gen;
    out->dests_gen = app->dests_gen;
    memcpy(out->art_last_id, app->art_last_id, sizeof(out->art_last_id));
}
