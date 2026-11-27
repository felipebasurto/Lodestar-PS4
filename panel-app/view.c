/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* One column of games (PANEL-1); dark ground, orange for what is chosen (PANEL-7). */
enum {
    LEFT = PANEL_LEFT, RIGHT = PANEL_RIGHT, LIST_TOP = PANEL_LIST_TOP, LIST_BOTTOM = PANEL_LIST_BOTTOM,
    ROW = PANEL_ROW, OPEN_ROW = PANEL_OPEN_ROW, EDGE = PANEL_EDGE,
};
enum { WHITE = 0xf3f4f5, TEXT = 0xc9cdd3, MUTED = 0x9aa0a8, ORANGE = 0xff6a13, GOLD = 0xffcb47, GREEN = 0x9bd65a,
       RED = 0xff6b5c, INK = 0x0a0b0d, RULE = 0x343941 };
enum glyph { PLAIN, CROSS, SQUARE, TRIANGLE, CIRCLE, KEY };

static const int wheel_pos[WHEEL_ITEMS][2] = {
    {960, 220}, {1410, 380}, {1410, 700}, {960, 860}, {510, 700}, {510, 380},
};

static const char *tr(const panel_view *v, const char *en, const char *es) { return v->spanish ? es : en; }

static const artwork *view_art(const panel_view *v, const char *id) {
    if (!v->art || !id || !*id)
        return NULL;
    for (int i = 0; i < ART_SLOTS; i++)
        if (!strcmp(id, v->art[i].id))
            return &v->art[i];
    return NULL;
}

static const char *game_state_label(const panel_view *v, enum game_state state) {
    switch (state) {
    case READY: return tr(v, "Ready to play", "Listo para jugar");
    case RESTART: return tr(v, "Restart to play", "Reinicia para jugar");
    case RUNNING: return tr(v, "Running", "Abierto");
    case REMOVED: return tr(v, "Not in library", "Fuera de la biblioteca");
    case OFFLINE: return tr(v, "Drive not connected", "Unidad no conectada");
    case INTERNAL: return tr(v, "Installed on console", "Instalado en la consola");
    case PENDING: return tr(v, "Waiting to be added", "Pendiente de añadir");
    case DUPLICATE: return tr(v, "Another copy is active", "Hay otra copia activa");
    default: return tr(v, "Unknown state", "Estado desconocido");
    }
}

/* What a state that needs explaining means and what to do about it (PANEL-9). */
static const char *state_help(const panel_view *v, enum game_state state) {
    switch (state) {
    case RESTART: return tr(v, "Restart the console once (not rest mode), then make sure Lodestar is loaded.",
                            "Reinicia la consola una vez (no vale el reposo) y comprueba que Lodestar está cargado.");
    case OFFLINE: return tr(v, "Connect its drive to play it.", "Conecta su unidad para jugarlo.");
    case DUPLICATE: return tr(v, "Another copy of this game is in use. Keep only one.",
                              "Se está usando otra copia de este juego. Deja solo una.");
    case PENDING: return tr(v, "Lodestar still has to add it. If it stays like this, check the log.",
                            "Lodestar todavía tiene que añadirlo. Si sigue así, mira el log.");
    case INTERNAL: return tr(v, "Installed the normal way. Lodestar leaves it alone.",
                             "Instalado de la forma normal. Lodestar no lo toca.");
    default: return "";
    }
}

static uint32_t state_color(enum game_state state) {
    return state == READY ? WHITE : state == RESTART || state == DUPLICATE ? GOLD : state == RUNNING ? GREEN :
           state == OFFLINE ? RED : MUTED;
}

static const char *action_name(const panel_view *v, enum action action) {
    switch (action) {
    case ADD: return tr(v, "Add to library", "Añadir a la biblioteca");
    case REMOVE: return tr(v, "Remove from library", "Quitar de la biblioteca");
    case RESTORE: return tr(v, "Restore original files", "Restaurar archivos originales");
    default: return "";
    }
}

static void centred(canvas c, int cx, int y, int size, uint32_t color, const char *label, int heavy) {
    int base_w = text_width(size, label);
    int width = base_w + (heavy ? size : 0);
    (heavy ? bold : text)(c, cx - base_w / 2, y, width + size, size, color, label);
}

static void right_aligned(canvas c, int right, int y, int size, uint32_t color, const char *label) {
    int width = text_width(size, label);
    text(c, right - width, y, width + size, size, color, label);
}

/* A face button in its ring, centred on (cx, cy). */
static void face(canvas c, int cx, int cy, enum glyph button) {
    uint32_t colour = button == CROSS ? 0x7ea7ff : button == SQUARE ? 0xee8fd0 : button == TRIANGLE ? 0x5fd6a8 : RED;
    ring(c, cx, cy, 17, 1, colour);
    if (button == CROSS) {
        for (int o = 0; o < 2; o++) {
            line(c, cx - 6 + o, cy - 6, cx + 6 + o, cy + 6, colour);
            line(c, cx - 6 + o, cy + 6, cx + 6 + o, cy - 6, colour);
        }
    } else if (button == SQUARE) {
        frame(c, cx - 6, cy - 6, 13, 13, 2, colour);
    } else if (button == TRIANGLE) {
        line(c, cx, cy - 8, cx - 7, cy + 5, colour);
        line(c, cx, cy - 8, cx + 7, cy + 5, colour);
        line(c, cx - 7, cy + 5, cx + 7, cy + 5, colour);
    } else {
        ring(c, cx, cy, 8, 2, colour);
    }
}

typedef struct {
    enum glyph glyph;
    const char *key, *label;
} hint;

static int hint_width(const hint *h) {
    int lead = h->glyph == PLAIN ? 0 : h->glyph == KEY ? text_width(24, h->key) + 38 : 48;
    return lead + text_width(28, h->label);
}

/* The controls, right aligned in the bottom strip; returns where they start. */
static int legend(canvas c, const hint *hints, int count) {
    int width = (count - 1) * 40;
    for (int i = 0; i < count; i++)
        width += hint_width(&hints[i]);
    int x = RIGHT - width, start = x;
    for (int i = 0; i < count; i++) {
        const hint *h = &hints[i];
        int lead = 0;
        if (h->glyph == KEY) {
            int key = text_width(24, h->key);
            frame(c, x, 1023, key + 28, 34, 1, MUTED);
            text(c, x + 14, 1048, key + 24, 24, TEXT, h->key);
            lead = key + 38;
        } else if (h->glyph != PLAIN) {
            face(c, x + 18, 1040, h->glyph);
            lead = 48;
        }
        text(c, x + lead, 1050, text_width(28, h->label) + 28, 28, MUTED, h->label);
        x += hint_width(h) + 40;
    }
    return start;
}

static void header(const panel_view *v, canvas c) {
    brand(c, LEFT, 36);
    const char *version = v->catalog ? v->catalog->version : "Lodestar";
    int vw = text_width(26, version);
    text(c, RIGHT - vw, 78, vw + 26, 26, MUTED, version);
    fill(c, RIGHT - vw - 22, 65, 10, 10, v->online ? GREEN : GOLD);
}

static void heading(canvas c, const char *label) { bold(c, LEFT, 164, 1500, 56, WHITE, label); }

static void cover(const panel_view *v, canvas c, const game *g, int x, int y, int size) {
    fill(c, x, y, size, size, 0x16191e);
    const artwork *a = view_art(v, g->id);
    if (a && a->pixels) {
        image_art(c, x, y, size, a->pixels, a->thumb64, a->opaque, a->gen);
    } else {
        ring(c, x + size / 2, y + size / 2, size / 5, 2, RULE);
    }
}

/* What the buttons do to this game; the one being held fills and says what it means. */
static void verbs(const panel_view *v, canvas c, const game *g, int x, int y, int width, int details) {
    enum action actions[2];
    int n = available_actions(g, v->online, actions);
    for (int i = 0; i < n; i++, y += 52) {
        int chosen = v->holding == actions[i];
        face(c, x + 18, y - 10, actions[i] == RESTORE ? SQUARE : CROSS);
        (chosen ? bold : text)(c, x + 48, y, width - 48, 32, chosen ? ORANGE : WHITE, action_name(v, actions[i]));
    }
    if (!n && g->state == RUNNING) {
        text(c, x, y, width, 28, MUTED, tr(v, "Close the game to remove it", "Cierra el juego para quitarlo"));
        y += 52;
    }
    if (details) {
        face(c, x + 18, y - 10, TRIANGLE);
        text(c, x + 48, y, width - 48, 32, TEXT, tr(v, "Details", "Ficha"));
        y += 52;
    }
    if (v->holding == REMOVE)
        text(c, x, y, width, 26, MUTED, tr(v, "Keeps your package files.", "Conserva tus paquetes."));
    else if (v->holding == RESTORE)
        text(c, x, y, width, 26, MUTED, tr(v, "Moves the packages back.", "Devuelve los paquetes a su sitio."));
}

static const char *section_name(const panel_view *v, int i) {
    return i == SECTION_NEEDS ? tr(v, "Needs attention", "Pendientes") :
           i == SECTION_ALL ? tr(v, "All", "Todos") : tr(v, "Running", "Abiertos");
}

static void message(canvas c, const char *what, const char *next) {
    bold(c, LEFT, 360, 1700, 52, WHITE, what);
    text(c, LEFT, 414, 1700, 32, TEXT, next);
}

static void open_row(const panel_view *v, canvas c, const game *g, int y) {
    char buffer[256];
    shade(c, LEFT - EDGE, y, RIGHT - LEFT + 2 * EDGE, OPEN_ROW, ORANGE, 22);
    frame(c, LEFT - EDGE, y, RIGHT - LEFT + 2 * EDGE, OPEN_ROW, 2, ORANGE);
    cover(v, c, g, LEFT, y + 28, 184);
    bold(c, 296, y + 84, 990, 64, WHITE, *g->name ? g->name : g->id);
    text(c, 296, y + 130, 990, 32, state_color(g->state), game_state_label(v, g->state));
    if (g->bytes)
        snprintf(buffer, sizeof(buffer), "%s%s%.1f GB", g->storage, *g->storage ? " · " : "", g->bytes / 1073741824.0);
    else
        snprintf(buffer, sizeof(buffer), "%s", g->storage);
    text(c, 296, y + 170, 990, 28, MUTED, buffer);
    const char *note = *g->issue ? g->issue : state_help(v, g->state);
    if (*note)
        text(c, 296, y + 208, 990, 28, *g->issue || g->state != INTERNAL ? GOLD : MUTED, note);
    verbs(v, c, g, 1320, y + 68, 520, 1);
    if (v->holding)
        fill(c, LEFT - EDGE, y + OPEN_ROW - 8, (RIGHT - LEFT + 2 * EDGE) * v->held / 1000, 8, ORANGE);
}

static void library_list(const panel_view *v, canvas c) {
    if (!v->online || !v->catalog || !v->visible_count)
        return;
    /* The focus rests on the third line and the list moves under it. */
    int first = v->selected > 2 ? v->selected - 2 : 0;
    for (int i = first, y = LIST_TOP; i < v->visible_count; i++) {
        const game *g = &v->catalog->games[v->indices[i]];
        int height = i == v->selected ? OPEN_ROW : ROW;
        if (y + height > LIST_BOTTOM)
            break;
        if (y + height > c.clip_y0 && y < c.clip_y1) {
            if (i == v->selected) {
                open_row(v, c, g, y);
            } else {
                cover(v, c, g, LEFT, y + 14, 64);
                text(c, 176, y + 60, 1200, 40, TEXT, *g->name ? g->name : g->id);
                if (g->state != READY)
                    right_aligned(c, RIGHT, y + 58, 28, state_color(g->state), game_state_label(v, g->state));
            }
        }
        y += height;
    }
}

static void show_library(const panel_view *v, canvas c) {
    char buffer[256];
    header(v, c);
    if (!v->online || !v->catalog) {
        message(c, v->catalog ? tr(v, "Connection lost", "Conexión perdida") :
                v->loading_library ? tr(v, "Connecting to Lodestar…", "Conectando con Lodestar…") :
                tr(v, "Lodestar is not running", "Lodestar no está en marcha"),
                tr(v, "Load the Lodestar payload and press Cross to reconnect.",
                   "Carga el payload de Lodestar y pulsa X para volver a conectar."));
        return;
    }

    for (int i = 0, x = LEFT; i < SECTIONS; i++) {
        int chosen = (int)v->section == i;
        snprintf(buffer, sizeof(buffer), "%s %d", section_name(v, i), v->counts[i]);
        int width = text_width(36, buffer) + (chosen ? 8 : 0);
        (chosen ? bold : text)(c, x, 156, width + 36, 36, chosen ? WHITE : MUTED, buffer);
        if (chosen)
            fill(c, x, 170, width, 3, ORANGE);
        x += width + 48;
    }
    int restarts = 0;
    for (int i = 0; i < v->catalog->count; i++)
        restarts += v->catalog->games[i].state == RESTART;
    if (*v->query) {
        snprintf(buffer, sizeof(buffer), tr(v, "Search: %s", "Búsqueda: %s"), v->query);
        int width = text_width(32, buffer);
        text(c, width > 800 ? RIGHT - 800 : RIGHT - width, 156, 832, 32, ORANGE, buffer);
    } else if (v->catalog->setup) {
        right_aligned(c, RIGHT, 156, 28, GOLD,
                      tr(v, "First start: hold L2, open Settings and save.", "Primer inicio: mantén L2, abre Ajustes y guarda."));
    } else if (restarts) {
        if (restarts == 1)
            snprintf(buffer, sizeof(buffer), "%s", tr(v, "1 game waits: restart the console (not rest mode)",
                                                      "1 juego espera: reinicia la consola (no vale el reposo)"));
        else
            snprintf(buffer, sizeof(buffer), tr(v, "%d games wait: restart the console (not rest mode)",
                                                "%d juegos esperan: reinicia la consola (no vale el reposo)"), restarts);
        right_aligned(c, RIGHT, 156, 28, GOLD, buffer);
    } else if (v->catalog->overflow) {
        right_aligned(c, RIGHT, 156, 28, GOLD, tr(v, "Library limit reached", "Se alcanzó el límite de la biblioteca"));
    }

    if (!v->visible_count) {
        if (*v->query)
            message(c, tr(v, "No matching games", "No hay coincidencias"),
                    tr(v, "Press Circle to clear the search.", "Pulsa círculo para borrar la búsqueda."));
        else if (v->counts[SECTION_ALL])
            message(c, tr(v, "Nothing in this section", "Nada en esta sección"),
                    tr(v, "L1 and R1 change section.", "L1 y R1 cambian de sección."));
        else
            message(c, tr(v, "No games found", "No se encontraron juegos"),
                    tr(v, "Put your packages in the Lodestar folder of a USB drive, then hold L2 and scan.",
                       "Pon tus paquetes en la carpeta Lodestar de un USB, mantén L2 y busca juegos."));
        return;
    }
    library_list(v, c);
}

static void info_row(canvas c, int y, const char *label, const char *value) {
    text(c, 548, y, 240, 30, MUTED, label);
    text(c, 800, y, RIGHT - 800, 32, WHITE, *value ? value : "—");
}

static void show_detail(const panel_view *v, canvas c) {
    const game *g = v->focused;
    if (!g)
        return;
    char buffer[512];
    header(v, c);
    cover(v, c, g, LEFT, 156, 420);
    bold(c, 548, 212, RIGHT - 548, 64, WHITE, *g->name ? g->name : g->id);
    text(c, 548, 260, RIGHT - 548, 32, state_color(g->state), game_state_label(v, g->state));
    info_row(c, 330, "ID", g->id);
    info_row(c, 380, tr(v, "Version", "Versión"), g->version);
    snprintf(buffer, sizeof(buffer), "%lld", g->dlc);
    info_row(c, 430, "DLC", buffer);
    info_row(c, 480, "Firmware", g->firmware);
    snprintf(buffer, sizeof(buffer), "%.1f GB", g->bytes / 1073741824.0);
    info_row(c, 530, tr(v, "Size", "Tamaño"), g->bytes ? buffer : "");
    snprintf(buffer, sizeof(buffer), "%s%s%s", g->storage, *g->storage && *g->source ? " · " : "", g->source);
    info_row(c, 580, tr(v, "Storage", "Ubicación"), buffer);
    const char *help = state_help(v, g->state);
    if (*help)
        text(c, 548, 628, RIGHT - 548, 28, g->state == INTERNAL ? MUTED : GOLD, help);
    if (*g->issue)
        text(c, 548, 664, RIGHT - 548, 28, GOLD, g->issue);
    verbs(v, c, g, 548, 710, 700, 0);
    if (v->holding)
        fill(c, 548, 860, 700 * v->held / 1000, 8, ORANGE);
}

#define FIELD(f) #f
#define SET(f, en, es, help_en, help_es, min, max) { FIELD(f), en, es, help_en, help_es, offsetof(settings, f), min, max }
const setting fields[SETTINGS_COUNT] = {
    SET(organize, "Organize packages", "Organizar los paquetes",
        "Moves packages into ps4/<title ID> on their drive.", "Mueve los paquetes a ps4/<ID del juego> en su unidad.", 0, 1),
    SET(internal, "Use internal storage", "Usar almacenamiento interno",
        "Scan the console’s /data/Lodestar folder.", "Busca paquetes en /data/Lodestar en la consola.", 0, 1),
    SET(batch, "Add games in groups", "Añadir juegos en grupos",
        "Register smaller batches when many games are found.", "Registra en grupos pequeños cuando encuentra muchos juegos.", 0, 1),
    SET(batch_size, "Games per group", "Juegos por grupo",
        "Sets the batch size when grouping is enabled.", "Define el tamaño del grupo si está activado.", 1, 50),
    SET(dlc_mode, "Add-on mode", "Modo de DLC",
        "Choose the source drive, a console install, or turn add-ons off.",
        "Elige la unidad de origen, la instalación en consola o desactivar los DLC.", 0, 2),
    SET(dlc_big, "Install large add-ons", "Instalar DLC grandes",
        "Applies to internal installs of add-ons larger than 256 MB.",
        "Se aplica si instalas en la consola DLC de más de 256 MB.", 0, 1),
    SET(language, "Language", "Idioma",
        "Use the console language, English, or Spanish.", "Elige el idioma de la consola, inglés o español.", 0, 2),
    SET(autorun, "Start with GoldHEN", "Arrancar con GoldHEN",
        "Start Lodestar when GoldHEN loads on this console.", "Inicia Lodestar cuando esta consola carga GoldHEN.", 0, 1),
    SET(klog, "Read console events", "Leer eventos de consola",
        "Tracks when games start and stop.", "Detecta cuándo se abren y cierran los juegos.", 0, 1),
};
#undef SET
#undef FIELD

const int setting_start[4] = {0, 4, 6, 9};
static const char *setting_text(const panel_view *v, const setting *f, int value, char *temp, size_t size) {
    if (!strcmp(f->key, "language")) {
        const char *names[] = {tr(v, "Console", "Consola"), "English", "Español"};
        return names[value];
    }
    if (!strcmp(f->key, "dlc_mode")) {
        const char *names[] = {tr(v, "Source drive", "Unidad de origen"), tr(v, "Install internally", "Instalar en consola"),
                               tr(v, "Off", "Desactivado")};
        return names[value];
    }
    if (f->max == 1)
        return value ? tr(v, "Yes", "Sí") : "No";
    snprintf(temp, size, "%d", value);
    return temp;
}

static void draw_setting_row(const panel_view *v, canvas c, int i, int count) {
    if (i == count) {
        int y = 256 + 60 * count;
        int save = !v->in_sidebar && v->setting_row == count;
        if (save)
            fill(c, 424, y, 800, 54, ORANGE);
        (save ? bold : text)(c, 440, y + 39, 760, 36, save ? INK : ORANGE, tr(v, "Save settings", "Guardar ajustes"));
        return;
    }
    const setting *f = &fields[setting_start[v->category] + i];
    int y = 232 + 60 * i;
    int focused = !v->in_sidebar && v->setting_row == i;
    char temp[24];
    if (focused)
        fill(c, 424, y, 800, 54, ORANGE);
    (focused ? bold : text)(c, 440, y + 39, 540, 34, focused ? INK : WHITE, v->spanish ? f->es : f->en);
    const char *value = setting_text(v, f, *(const int *)((const char *)v->config + f->offset), temp, sizeof(temp));
    int width = text_width(30, value);
    width = width > 220 ? 220 : width;
    text(c, 1208 - width, y + 38, width + 12, 30, focused ? INK : ORANGE, value);
}

static void draw_setting_help(const panel_view *v, canvas c, int count) {
    if (!v->in_sidebar && v->setting_row < count) {
        const setting *f = &fields[setting_start[v->category] + v->setting_row];
        const char *help = v->spanish ? f->help_es : f->help_en;
        char first[64];
        size_t split = strlen(help);
        if (split > 46) {
            split = 46;
            while (split && help[split] != ' ')
                split--;
        }
        snprintf(first, sizeof(first), "%.*s", (int)split, help);
        text(c, 1280, 271, 560, 28, MUTED, first);
        if (help[split])
            text(c, 1280, 309, 560, 28, MUTED, help + split + 1);
    }
}

static void show_settings(const panel_view *v, canvas c) {
    header(v, c);
    heading(c, tr(v, "Settings", "Ajustes"));
    if (!v->config_ready) {
        text(c, LEFT, 300, 1600, 36, WHITE, v->loading_config ?
             tr(v, "Loading settings…", "Cargando ajustes…") :
             tr(v, "Settings are unavailable", "Ajustes no disponibles"));
        return;
    }
    const char *sections[] = {tr(v, "Library", "Biblioteca"), "DLC", tr(v, "Console", "Consola")};
    for (int i = 0; i < 3; i++) {
        int y = 232 + i * 60;
        int focused = i == v->category && v->in_sidebar;
        if (focused)
            fill(c, LEFT - 16, y, 300, 54, ORANGE);
        (i == v->category ? bold : text)(c, LEFT, y + 40, 270, 38,
                                         focused ? INK : i == v->category ? WHITE : MUTED, sections[i]);
    }
    int count = setting_start[v->category + 1] - setting_start[v->category];
    for (int i = 0; i <= count; i++)
        draw_setting_row(v, c, i, count);
    draw_setting_help(v, c, count);
}

static void show_phone(const panel_view *v, canvas c) {
    header(v, c);
    heading(c, tr(v, "Send packages", "Enviar paquetes"));
    text(c, LEFT, 300, 1500, 40, WHITE,
         tr(v, "Open Lodestar on your phone or computer", "Abre Lodestar en tu móvil u ordenador"));
    bold(c, LEFT, 412, 1700, 76, ORANGE, *v->local_url ? v->local_url :
         tr(v, "http://<PS4 IP>:8780", "http://<IP de la PS4>:8780"));
    if (!*v->local_url)
        text(c, LEFT, 464, 1500, 32, MUTED,
             tr(v, "Use the address in the Lodestar startup notification.",
                "Usa la dirección del aviso de inicio de Lodestar."));
    text(c, LEFT, 524, 1500, 32, TEXT,
         tr(v, "Send packages, manage downloads and open advanced settings.",
            "Envía paquetes, gestiona descargas y abre los ajustes avanzados."));
}

static const char *download_state(const panel_view *v, const char *state) {
    if (!strcmp(state, "running"))
        return tr(v, "Downloading", "En curso");
    if (!strcmp(state, "failed"))
        return tr(v, "Failed", "Falló");
    return tr(v, "Queued", "En cola");
}

static void show_downloads(const panel_view *v, canvas c) {
    header(v, c);
    heading(c, tr(v, "Transfers", "Transferencias"));
    if (!v->links_ready || !v->links->configured) {
        text(c, LEFT, 300, 1450, 36, WHITE, v->loading_links ?
             tr(v, "Reading the download queue…", "Leyendo la cola de descargas…") :
             tr(v, "Transfers are unavailable", "Transferencias no disponibles"));
        return;
    }
    if (*v->entered_url) {
        int d = v->dests && v->dests->count ? (v->dest >= 0 && v->dest < v->dests->count ? v->dest : 0) : 0;
        text(c, LEFT, 260, RIGHT - LEFT, 34, WHITE, v->entered_url);
        text(c, LEFT, 310, 1300, 32, ORANGE,
             v->dests && v->dests->count ? v->dests->items[d].label : !v->dest_ready && (v->loading_destinations) ?
             tr(v, "Reading destinations…", "Leyendo destinos…") : tr(v, "No destination selected", "Sin destino seleccionado"));
        if (v->dests && v->dests->count > 1)
            text(c, LEFT, 352, 1200, 26, MUTED, tr(v, "Left / Right to change drive", "Izquierda / derecha para cambiar de unidad"));
    } else {
        text(c, LEFT, 270, 1350, 34, TEXT,
             tr(v, "Enter a direct URL to your package or archive", "Introduce la URL directa de tu paquete o archivo"));
    }
    if (v->links->active) {
        char progress[200];
        bold(c, LEFT, 440, 1350, 40, WHITE, v->links->name);
        const char *phase = !strcmp(v->links->phase, "extracting") ? tr(v, "Extracting", "Extrayendo") :
                            !strcmp(v->links->phase, "probing") ? tr(v, "Checking link", "Comprobando enlace") :
                            !strcmp(v->links->phase, "retrying") ? tr(v, "Retrying", "Reintentando") :
                            tr(v, "Downloading", "Descargando");
        if (v->links->total > 0) {
            double ratio = (double)v->links->done / (double)v->links->total;
            snprintf(progress, sizeof(progress), "%s · %.0f %%", phase, ratio * 100);
            fill(c, LEFT, 506, RIGHT - LEFT, 8, RULE);
            fill(c, LEFT, 506, (int)((ratio < 0 ? 0 : ratio > 1 ? 1 : ratio) * (RIGHT - LEFT)), 8, ORANGE);
        } else {
            snprintf(progress, sizeof(progress), "%s · %.0f MB", phase, v->links->done / 1048576.0);
        }
        text(c, LEFT, 486, 1400, 28, ORANGE, progress);
    } else if (*v->links->message) {
        text(c, LEFT, 460, 1650, 32, TEXT, v->links->message);
    } else if (!v->links->count) {
        text(c, LEFT, 460, 1650, 32, MUTED, tr(v, "The download queue is empty", "La cola de descargas está vacía"));
    }
    int shown = v->links->count < 6 ? v->links->count : 6;
    for (int i = 0; i < shown; i++) {
        int y = 590 + i * 52;
        const download *job = &v->links->jobs[i];
        text(c, LEFT, y, 930, 30, WHITE, job->name);
        text(c, 1100, y, 180, 28, *job->error ? RED : ORANGE, download_state(v, job->state));
        text(c, 1300, y, RIGHT - 1300, 28, MUTED, job->error);
    }
    if (v->links->count > shown)
        text(c, LEFT, 925, 1600, 26, MUTED, tr(v, "Open the web panel to see all downloads.", "Abre el panel web para ver todas las descargas."));
}

static const char *wheel_name(const panel_view *v, int i) {
    switch (i) {
    case 0: return tr(v, "Scan for games", "Buscar juegos");
    case 1: return tr(v, "Send packages", "Enviar paquetes");
    case 2: return tr(v, "Transfers", "Transferencias");
    case 3: return tr(v, "Settings", "Ajustes");
    case 4: return tr(v, "Search by name", "Buscar por nombre");
    default: return tr(v, "Exit", "Salir");
    }
}

static void draw_wheel_pill_content(const panel_view *v, canvas c, int i) {
    int chosen = v->wheel_at == i;
    if (chosen)
        fill(c, wheel_pos[i][0] - PANEL_WHEEL_W / 2, wheel_pos[i][1] - PANEL_WHEEL_H / 2,
             PANEL_WHEEL_W, PANEL_WHEEL_H, ORANGE);
    centred(c, wheel_pos[i][0], wheel_pos[i][1] + 14, 40, chosen ? INK : WHITE, wheel_name(v, i), chosen);
}

static void draw_wheel_drives(const panel_view *v, canvas c) {
    /* The drives sit in the hub: free space is a glance away without a place on every screen. */
    int n = v->catalog ? v->catalog->drive_count : 0;
    n = n > 6 ? 6 : n;
    for (int i = 0; i < n; i++) {
        char buffer[128];
        const drive *d = &v->catalog->drives[i];
        snprintf(buffer, sizeof(buffer), tr(v, "%s · %.0f GB free", "%s · %.0f GB libres"), d->label,
                 d->free_bytes / 1073741824.0);
        centred(c, 960, 550 - n * 22 + i * 44, 30, MUTED, buffer, 0);
    }
}

/* The app-wide actions around a circle, over whatever was on screen (PANEL-2). */
static void show_wheel(const panel_view *v, canvas c) {
    shade(c, 0, 0, WIDTH, HEIGHT, INK, 240);
    for (int i = 0; i < WHEEL_ITEMS; i++)
        draw_wheel_pill_content(v, c, i);
    draw_wheel_drives(v, c);
}

static void footer(const panel_view *v, canvas c) {
    hint hints[6];
    int n = 0;
    int games = v->page == LIBRARY && v->online && v->visible_count;
    if (v->wheel) {
        hints[n++] = (hint){PLAIN, "", tr(v, "Release L2 to choose", "Suelta L2 para elegir")};
        hints[n++] = (hint){CIRCLE, "", tr(v, "Cancel", "Cancelar")};
    } else {
        if (v->page == LIBRARY && !v->online)
            hints[n++] = (hint){CROSS, "", tr(v, "Reconnect", "Reconectar")};
        if (games) {
            hints[n++] = (hint){KEY, "‹ ›", tr(v, "Jump by letter", "Saltar de letra")};
        }
        if (v->page == LIBRARY && v->online)
            hints[n++] = (hint){KEY, "L1 R1", tr(v, "Section", "Sección")};
        if (games || v->page == DETAIL)
            hints[n++] = (hint){PLAIN, "", tr(v, "Hold to confirm", "Mantén para confirmar")};
        if (v->page == SETTINGS)
            hints[n++] = (hint){CROSS, "", tr(v, "Select", "Elegir")};
        if (v->page == DOWNLOADS && !v->link_writing && v->links_ready && v->links->configured) {
            hints[n++] = (hint){CROSS, "", *v->entered_url ? tr(v, "Queue link", "Poner en cola") :
                                           tr(v, "Enter link", "Introducir enlace")};
            if (*v->entered_url)
                hints[n++] = (hint){TRIANGLE, "", tr(v, "Edit link", "Editar enlace")};
        }
        hints[n++] = (hint){KEY, "L2", tr(v, "Wheel", "Rueda")};
        hints[n++] = (hint){CIRCLE, "", v->page != LIBRARY ? tr(v, "Back", "Volver") :
                                         *v->query ? tr(v, "Clear search", "Borrar búsqueda") : tr(v, "Exit", "Salir")};
    }
    shade(c, 0, LIST_BOTTOM, WIDTH, HEIGHT - LIST_BOTTOM, INK, 210);
    int start = legend(c, hints, n);
    const char *notice = v->waiting || v->link_writing ? tr(v, "Waiting for Lodestar…", "Esperando a Lodestar…") :
                                                         v->feedback;
    if (start - LEFT > 80)
        text(c, LEFT, 1050, start - LEFT - 40, 28, WHITE, notice);
}

void panel_draw(const panel_view *v, canvas c) {
    background(c);
    if (v->page == DETAIL)
        show_detail(v, c);
    else if (v->page == LIBRARY)
        show_library(v, c);
    else if (v->page == SETTINGS)
        show_settings(v, c);
    else if (v->page == PHONE)
        show_phone(v, c);
    else
        show_downloads(v, c);
    if (v->wheel)
        show_wheel(v, c);
    footer(v, c);
}

rect panel_header_rect(void) { return (rect){0, 0, WIDTH, PANEL_HEADER_BOTTOM}; }
rect panel_footer_rect(void) { return (rect){0, LIST_BOTTOM, WIDTH, HEIGHT - LIST_BOTTOM}; }
rect panel_list_rect(void) { return (rect){LEFT - EDGE, LIST_TOP, RIGHT - LEFT + 2 * EDGE, LIST_BOTTOM - LIST_TOP}; }

rect panel_wheel_pill_rect(int idx) {
    if (idx < 0 || idx >= WHEEL_ITEMS)
        return (rect){0, 0, 0, 0};
    return (rect){wheel_pos[idx][0] - PANEL_WHEEL_W / 2, wheel_pos[idx][1] - PANEL_WHEEL_H / 2,
                  PANEL_WHEEL_W, PANEL_WHEEL_H};
}

int panel_open_row_y(const panel_view *v) {
    if (!v || v->page != LIBRARY || !v->online || !v->catalog || !v->visible_count ||
        v->selected < 0 || v->selected >= v->visible_count)
        return -1;
    int first = v->selected > 2 ? v->selected - 2 : 0;
    int y = LIST_TOP + (v->selected - first) * ROW;
    return y + OPEN_ROW <= LIST_BOTTOM ? y : -1;
}

rect panel_open_row_rect(const panel_view *v) {
    int y = panel_open_row_y(v);
    if (y < 0)
        return (rect){0, 0, 0, 0};
    return (rect){LEFT - EDGE, y, RIGHT - LEFT + 2 * EDGE, OPEN_ROW};
}

rect panel_hold_bar_rect(const panel_view *v) {
    if (!v)
        return (rect){0, 0, 0, 0};
    if (v->page == LIBRARY) {
        int y = panel_open_row_y(v);
        if (y < 0)
            return (rect){0, 0, 0, 0};
        return (rect){LEFT - EDGE, y + OPEN_ROW - 8, RIGHT - LEFT + 2 * EDGE, 8};
    }
    if (v->page == DETAIL && v->focused)
        return (rect){548, 860, 700, 8};
    return (rect){0, 0, 0, 0};
}

rect panel_detail_verbs_rect(void) { return (rect){540, 680, 720, 200}; }

rect panel_settings_row_rect(int row, int count) {
    if (row == count)
        return (rect){424, 256 + 60 * count, 800, 54};
    return (rect){424, 232 + 60 * row, 800, 54};
}

rect panel_settings_help_rect(void) { return (rect){1264, 232, 600, 120}; }

int panel_find_cover_rect(const panel_view *v, const char *id, rect *out) {
    if (!v || !id || !*id || !out || v->wheel)
        return 0;
    if (v->page == DETAIL) {
        if (v->focused && !strcmp(v->focused->id, id)) {
            *out = (rect){LEFT, 156, 420, 420};
            return 1;
        }
        return 0;
    }
    if (v->page != LIBRARY || !v->online || !v->catalog || !v->visible_count)
        return 0;
    int first = v->selected > 2 ? v->selected - 2 : 0;
    for (int i = first, y = LIST_TOP; i < v->visible_count; i++) {
        const game *g = &v->catalog->games[v->indices[i]];
        int height = i == v->selected ? OPEN_ROW : ROW;
        if (y + height > LIST_BOTTOM)
            break;
        if (!strcmp(g->id, id)) {
            if (i == v->selected)
                *out = (rect){LEFT, y + 28, 184, 184};
            else
                *out = (rect){LEFT, y + 14, 64, 64};
            return 1;
        }
        y += height;
    }
    return 0;
}

void panel_draw_header_region(const panel_view *v, canvas c) {
    rect r = panel_header_rect();
    canvas sub = canvas_clip(c.pixels, r);
    background(sub);
    header(v, sub);
}

void panel_draw_footer_region(const panel_view *v, canvas c) {
    rect r = panel_footer_rect();
    canvas sub = canvas_clip(c.pixels, r);
    background(sub);
    if (v->wheel)
        shade(sub, r.x, r.y, r.w, r.h, INK, 240);
    footer(v, sub);
}

void panel_draw_library_list_region(const panel_view *v, canvas c, rect clip) {
    canvas sub = canvas_clip(c.pixels, clip);
    background(sub);
    library_list(v, sub);
}

void panel_draw_open_row_region(const panel_view *v, canvas c) {
    rect r = panel_open_row_rect(v);
    if (r.w <= 0 || !v->focused)
        return;
    canvas sub = canvas_clip(c.pixels, r);
    background(sub);
    open_row(v, sub, v->focused, r.y);
}

void panel_draw_hold_bar_delta(const panel_view *v, canvas c, int prev_held, rect *out_damage) {
    rect bar = panel_hold_bar_rect(v);
    if (bar.w <= 0 || !v->holding) {
        if (out_damage)
            *out_damage = (rect){0, 0, 0, 0};
        return;
    }
    int prev_w = prev_held > 0 ? bar.w * prev_held / 1000 : 0;
    int cur_w = v->held > 0 ? bar.w * v->held / 1000 : 0;
    if (cur_w <= prev_w) {
        if (out_damage)
            *out_damage = (rect){0, 0, 0, 0};
        return;
    }
    rect delta = {bar.x + prev_w, bar.y, cur_w - prev_w, bar.h};
    fill(canvas_clip(c.pixels, delta), delta.x, delta.y, delta.w, delta.h, ORANGE);
    if (out_damage)
        *out_damage = delta;
}

void panel_draw_cover_region(const panel_view *v, canvas c, const char *id, rect r) {
    if (!v || !v->catalog || !id || r.w <= 0)
        return;
    const game *found = NULL;
    if (v->focused && !strcmp(v->focused->id, id)) {
        found = v->focused;
    } else {
        for (int i = 0; i < v->catalog->count; i++) {
            if (!strcmp(v->catalog->games[i].id, id)) {
                found = &v->catalog->games[i];
                break;
            }
        }
    }
    if (!found)
        return;
    canvas sub = canvas_clip(c.pixels, r);
    cover(v, sub, found, r.x, r.y, r.w);
}

void panel_draw_wheel_open(const panel_view *v, canvas c, uint32_t *pill_bg[WHEEL_ITEMS]) {
    /* Clear the old footer strip to ground before dimming so old footer text does not bleed through. */
    rect f = panel_footer_rect();
    background_rect(c, f.x, f.y, f.w, f.h);
    shade(c, 0, 0, WIDTH, HEIGHT, INK, 240);
    for (int i = 0; i < WHEEL_ITEMS; i++) {
        if (!pill_bg[i])
            continue;
        rect p = panel_wheel_pill_rect(i);
        size_t row_bytes = (size_t)p.w * sizeof(*c.pixels);
        for (int row = 0; row < p.h; row++)
            memcpy(pill_bg[i] + (size_t)row * (size_t)p.w,
                   c.pixels + (size_t)(p.y + row) * WIDTH + p.x, row_bytes);
    }
    for (int i = 0; i < WHEEL_ITEMS; i++)
        draw_wheel_pill_content(v, c, i);
    draw_wheel_drives(v, c);
    footer(v, c);
}

void panel_draw_wheel_pill(const panel_view *v, canvas c, int idx, const uint32_t *pill_bg) {
    if (idx < 0 || idx >= WHEEL_ITEMS)
        return;
    rect p = panel_wheel_pill_rect(idx);
    canvas sub = canvas_clip(c.pixels, p);
    if (v->wheel_at != idx && pill_bg) {
        size_t row_bytes = (size_t)p.w * sizeof(*c.pixels);
        for (int row = 0; row < p.h; row++)
            memcpy(c.pixels + (size_t)(p.y + row) * WIDTH + p.x,
                   pill_bg + (size_t)row * (size_t)p.w, row_bytes);
    }
    draw_wheel_pill_content(v, sub, idx);
}

void panel_draw_detail_verbs_region(const panel_view *v, canvas c) {
    if (!v->focused)
        return;
    rect r = panel_detail_verbs_rect();
    canvas sub = canvas_clip(c.pixels, r);
    background(sub);
    verbs(v, sub, v->focused, 548, 710, 700, 0);
    if (v->holding)
        fill(sub, 548, 860, 700 * v->held / 1000, 8, ORANGE);
}

void panel_draw_settings_row_region(const panel_view *v, canvas c, int row) {
    int count = setting_start[v->category + 1] - setting_start[v->category];
    if (row < 0 || row > count)
        return;
    rect r = panel_settings_row_rect(row, count);
    canvas sub = canvas_clip(c.pixels, r);
    background(sub);
    draw_setting_row(v, sub, row, count);
}

void panel_draw_settings_help_region(const panel_view *v, canvas c) {
    int count = setting_start[v->category + 1] - setting_start[v->category];
    rect r = panel_settings_help_rect();
    canvas sub = canvas_clip(c.pixels, r);
    background(sub);
    draw_setting_help(v, sub, count);
}
