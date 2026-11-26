/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LEFT = 96, RIGHT = 1824, GRID_X = 490, COVER = 235 };
enum { WHITE = 0xeff3ff, TEXT = 0xc4cee8, MUTED = 0xa5b3d5, BLUE = 0xa1b9ff, AMBER = 0xffca72,
       TEAL = 0x72deb8, RED = 0xffa0b3, BORDER = 0x333f63, NAVY = 0x19274b };

static const char *tr(const panel_view *v, const char *en, const char *es) { return v->spanish ? es : en; }

static const artwork *view_art(const panel_view *v, const char *id) {
    for (int i = 0; i < ART_SLOTS; i++)
        if (!strcmp(id, v->art[i].id))
            return &v->art[i];
    return NULL;
}

int available_actions(const game *g, int online, enum action out[2]) {
    int count = 0;
    if (!g || !online)
        return 0;
    if (g->state == REMOVED)
        out[count++] = ADD;
    if (g->state == READY || g->state == RESTART)
        out[count++] = REMOVE;
    if (!g->in_place && (g->state == READY || g->state == RESTART || g->state == REMOVED || g->state == PENDING))
        out[count++] = RESTORE;
    return count;
}


static const char *game_state_label(const panel_view *v, enum game_state state) {
    switch (state) {
    case READY: return tr(v, "Ready to play", "Listo para jugar");
    case RESTART: return tr(v, "Restart to play", "Reinicia para jugar");
    case RUNNING: return tr(v, "Running", "Abierto");
    case REMOVED: return tr(v, "Removed from library", "Fuera de la biblioteca");
    case OFFLINE: return tr(v, "Drive disconnected", "Unidad no conectada");
    case INTERNAL: return tr(v, "Installed on console", "Instalado en la consola");
    case PENDING: return tr(v, "Waiting to be added", "Pendiente de añadir");
    case DUPLICATE: return tr(v, "Another copy is active", "Hay otra copia activa");
    default: return tr(v, "Unknown state", "Estado desconocido");
    }
}

static uint32_t state_color(enum game_state state) {
    return state == RESTART ? AMBER : state == RUNNING ? TEAL : state == OFFLINE ? RED : MUTED;
}

static const char *action_name(const panel_view *v, enum action action) {
    switch (action) {
    case ADD: return tr(v, "Add to library", "Añadir a la biblioteca");
    case REMOVE: return tr(v, "Remove from library", "Quitar de la biblioteca");
    case RESTORE: return tr(v, "Restore original files", "Restaurar archivos originales");
    default: return "";
    }
}

static void stripe(canvas c, int x, int y, int w, int h, int focused) {
    if (!focused)
        return;
    rounded_fill(c, x, y, w, h, 7, BORDER);
    rounded_fill(c, x + 1, y + 1, w - 2, h - 2, 6, NAVY);
}

static void star(canvas c, int x, int y, int size, uint32_t color) {
    int waist = size / 6, spread = size * 9 / 10;
    for (int dy = -size; dy <= size; dy++) {
        int rise = abs(dy);
        int w = rise < waist ? spread - (spread - waist) * rise / waist :
                waist * (size - rise) / (size - waist);
        fill(c, x - w, y + dy, w * 2 + 1, 1, color);
    }
}

static void header(const panel_view *v, canvas c, const char *heading) {
    star(c, LEFT + 17, 70, 19, 0xd8deff);
    fill(c, LEFT + 16, 51, 2, 14, AMBER);
    text(c, LEFT + 48, 78, 168, 19, WHITE, "L O D E S T A R");
    fill(c, 328, 66, 7, 7, v->online ? TEAL : AMBER);
    text(c, 347, 77, 500, 18, MUTED, v->catalog ? v->catalog->version : "Lodestar");
    text(c, LEFT, 195, 1100, 64, WHITE, heading);
    fill(c, LEFT, 230, RIGHT - LEFT, 1, BORDER);
}

static void hint(canvas c, int x, int button, const char *name) {
    int y = 1024;
    if (button == 0) {
        line(c, x, y, x + 14, y + 14, BLUE);
        line(c, x, y + 14, x + 14, y, BLUE);
    } else if (button == 1) {
        fill(c, x, y, 15, 15, 0xffa9e3);
        fill(c, x + 2, y + 2, 11, 11, 0x080a15);
    } else if (button == 2) {
        line(c, x + 7, y, x, y + 14, TEAL);
        line(c, x + 7, y, x + 14, y + 14, TEAL);
        line(c, x, y + 14, x + 14, y + 14, TEAL);
    } else {
        for (int i = -7; i <= 7; i++)
            for (int j = -7; j <= 7; j++)
                if (i * i + j * j >= 31 && i * i + j * j <= 51)
                    fill(c, x + 7 + i, y + 7 + j, 1, 1, RED);
    }
    text(c, x + 26, y + 15, 208, 23, WHITE, name);
}

static void footer(const panel_view *v, canvas c) {
    fill(c, LEFT, 991, RIGHT - LEFT, 1, BORDER);
    const char *notice = v->feedback;
    if (v->waiting || v->link_writing)
        notice = tr(v, "Waiting for Lodestar…", "Esperando a Lodestar…");
    text(c, LEFT, 1039, v->page == LIBRARY ? 875 : v->page == DOWNLOADS ? 1060 : 1220, 23, WHITE, notice);
    if (v->page == LIBRARY) {
        hint(c, 1015, 0, tr(v, "Open", "Abrir"));
        hint(c, 1180, 1, tr(v, "Scan for games", "Buscar juegos"));
        hint(c, 1460, 2, tr(v, "Search", "Buscar"));
    } else if (v->page == DOWNLOADS && !v->link_writing && v->links_ready && v->links->configured) {
        hint(c, 1210, 0, *v->entered_url ? tr(v, "Queue link", "Poner en cola") : tr(v, "Enter link", "Introducir enlace"));
        if (*v->entered_url)
            hint(c, 1460, 2, tr(v, "Edit link", "Editar enlace"));
    } else if (v->confirm) {
        hint(c, 1370, 0, tr(v, "Confirm", "Confirmar"));
    } else if (v->page == SETTINGS || v->page == DETAIL) {
        hint(c, 1370, 0, v->page == SETTINGS ? tr(v, "Select", "Elegir") : tr(v, "Choose", "Elegir"));
    }
    hint(c, 1650, 3, v->page == LIBRARY ? tr(v, "Exit", "Salir") : tr(v, "Back", "Volver"));
}

static void cover(const panel_view *v, canvas c, const game *g, int x, int y, int size, int focused) {
    if (focused) {
        rounded_fill(c, x - 7, y - 7, size + 14, size + 14, 15, BLUE);
        rounded_fill(c, x - 4, y - 4, size + 8, size + 8, 12, 0x14264e);
    }
    rounded_fill(c, x, y, size, size, 9, 0x202b44);
    const artwork *a = view_art(v, g->id);
    if (a && a->pixels)
        image(c, x, y, size, 9, a->pixels);
    else {
        star(c, x + size / 2, y + size / 2 - 13, size / 5, 0x4c608c);
        text(c, x + 18, y + size - 19, size - 36, 22, TEXT, g->id);
    }
}

static const char *sidebar_name(const panel_view *v, int i) {
    switch (i) {
    case 0: return tr(v, "All games", "Todos");
    case 1: return tr(v, "Restart to play", "Reinicia para jugar");
    case 2: return tr(v, "Running", "Abiertos");
    case 3: return tr(v, "Send games", "Enviar juegos");
    case 4: return tr(v, "Downloads", "Descargas");
    default: return tr(v, "Settings", "Ajustes");
    }
}

static void show_library(const panel_view *v, canvas c) {
    char buffer[256];
    header(v, c, tr(v, "Library", "Biblioteca"));
    snprintf(buffer, sizeof(buffer), tr(v, "%d games", "%d juegos"), v->visible_count);
    text(c, v->spanish ? 418 : 338, 193, 180, 24, MUTED, buffer);
    text(c, 1440, 78, 384, 22, *v->query ? WHITE : MUTED,
         *v->query ? v->query : tr(v, "Search game or title ID", "Busca un juego o su ID"));
    fill(c, 1440, 90, 384, 1, BORDER);

    for (int i = 0; i < 3; i++) {
        int y = 259 + i * 53;
        stripe(c, LEFT, y, 328, 48, (v->in_sidebar && v->sidebar == i) || ((int)v->filter == i && !v->in_sidebar));
        text(c, LEFT + 17, y + 33, 252, 25, i == REBOOTS ? AMBER : TEXT, sidebar_name(v, i));
        snprintf(buffer, sizeof(buffer), "%d", v->counts[i]);
        text(c, LEFT + 274, y + 33, 51, 21, MUTED, buffer);
    }
    int y = 457;
    if (v->catalog && v->catalog->drive_count) {
        text(c, LEFT + 17, y, 286, 19, MUTED, tr(v, "STORAGE", "UNIDADES"));
        y += 42;
        for (int i = 0; i < v->catalog->drive_count && i < 6; i++, y += 40) {
            const drive *d = &v->catalog->drives[i];
            snprintf(buffer, sizeof(buffer), tr(v, "%.0f GB free", "%.0f GB libres"), d->free_bytes / 1073741824.0);
            text(c, LEFT + 17, y, 150, 23, TEXT, d->label);
            text(c, LEFT + 175, y, 145, 20, MUTED, buffer);
        }
    }
    y += 24;
    for (int i = 3; i < 6; i++, y += 51) {
        stripe(c, LEFT, y, 328, 48, v->in_sidebar && v->sidebar == i);
        text(c, LEFT + 17, y + 32, 294, 25, TEXT, sidebar_name(v, i));
    }
    if (!v->online) {
        text(c, GRID_X, 328, 1350, 35, WHITE, v->catalog ? tr(v, "Connection lost", "Conexión perdida") :
             v->loading_library ? tr(v, "Connecting to Lodestar…", "Conectando con Lodestar…") :
             tr(v, "Lodestar is not running", "Lodestar no está en marcha"));
        text(c, GRID_X, 390, 1350, 26, MUTED,
             tr(v, "Load the Lodestar payload and press Square to reconnect.",
                "Carga el payload de Lodestar y pulsa cuadrado para volver a conectar."));
        return;
    }
    if (v->catalog->setup) {
        text(c, GRID_X, 954, 1310, 22, AMBER,
             tr(v, "First start: review Settings and save to start scanning.",
                "Primer inicio: revisa Ajustes y guarda para iniciar la búsqueda."));
    } else if (v->catalog->overflow) {
        text(c, GRID_X, 954, 1310, 22, AMBER, tr(v, "Library limit reached", "Se alcanzó el límite de la biblioteca"));
    }
    if (!v->visible_count) {
        text(c, GRID_X, 328, 1360, 36, WHITE, v->counts[ALL] || *v->query ? tr(v, "No matching games", "No hay coincidencias") :
             tr(v, "No games found", "No se encontraron juegos"));
        text(c, GRID_X, 377, 1290, 25, MUTED, v->counts[ALL] || *v->query ?
             tr(v, "Choose another filter or search again.", "Cambia el filtro o vuelve a buscar.") :
             tr(v, "Connect a USB drive with your packages, then scan for games.",
                "Conecta un USB con tus paquetes y pulsa Buscar juegos."));
        return;
    }
    int first = v->selected / PAGE * PAGE;
    for (int i = first; i < first + PAGE && i < v->visible_count; i++) {
        const game *g = &v->catalog->games[v->indices[i]];
        int x = GRID_X + (i - first) % COLS * 274, cy = 260 + (i - first) / COLS * 336;
        cover(v, c, g, x, cy, COVER, !v->in_sidebar && v->selected == i);
        text(c, x, cy + COVER + 33, COVER, 23, WHITE, *g->name ? g->name : g->id);
        if (g->state == READY && *g->storage) {
            snprintf(buffer, sizeof(buffer), "%s · %.1f GB", g->storage, g->bytes / 1073741824.0);
            text(c, x, cy + COVER + 59, COVER, 19, MUTED, buffer);
        } else {
            text(c, x, cy + COVER + 59, COVER, 19, state_color(g->state), game_state_label(v, g->state));
        }
    }
    if (v->visible_count > PAGE) {
        snprintf(buffer, sizeof(buffer), "%d / %d", first / PAGE + 1, (v->visible_count + PAGE - 1) / PAGE);
        text(c, 1700, 954, 124, 21, MUTED, buffer);
    }
}

static void info_row(canvas c, int y, const char *label, const char *value) {
    text(c, 590, y, 230, 24, MUTED, label);
    text(c, 837, y, RIGHT - 837, 24, TEXT, *value ? value : "—");
}

static void show_detail(const panel_view *v, canvas c) {
    const game *g = v->focused;
    if (!g)
        return;
    char buffer[512];
    cover(v, c, g, LEFT, 247, 420, 0);
    text(c, 590, 306, RIGHT - 590, 55, WHITE, *g->name ? g->name : g->id);
    snprintf(buffer, sizeof(buffer), "%s · %s", g->id, game_state_label(v, g->state));
    text(c, 590, 352, RIGHT - 590, 26, state_color(g->state), buffer);
    info_row(c, 414, tr(v, "Version", "Versión"), g->version);
    snprintf(buffer, sizeof(buffer), "%lld", g->dlc);
    info_row(c, 455, "DLC", buffer);
    info_row(c, 496, "Firmware", g->firmware);
    snprintf(buffer, sizeof(buffer), "%.1f GB", g->bytes / 1073741824.0);
    info_row(c, 537, tr(v, "Size", "Tamaño"), g->bytes ? buffer : "");
    snprintf(buffer, sizeof(buffer), "%s%s%s", g->storage, *g->storage && *g->source ? " · " : "", g->source);
    info_row(c, 578, tr(v, "Storage", "Ubicación"), buffer);
    if (*g->issue)
        text(c, 590, 622, RIGHT - 590, 23, AMBER, g->issue);
    enum action actions[2];
    for (int i = 0, n = available_actions(g, v->online, actions); i < n; i++) {
        int y = 654 + i * 61;
        stripe(c, 590, y, 570, 55, i == v->action_row);
        text(c, 608, y + 36, 534, 26, TEXT, action_name(v, actions[i]));
    }
    if (v->confirm) {
        fill(c, 590, 795, 1130, 145, 0x101b33);
        text(c, 609, 844, 1088, 29, WHITE, action_name(v, v->confirm));
        text(c, 609, 892, 1088, 23, TEXT,
             v->confirm == RESTORE ? tr(v, "Moves the packages back to their original location.",
                                     "Mueve los paquetes de vuelta a su ubicación original.") :
                                  tr(v, "Keeps your package files on the drive.", "Conserva tus paquetes en la unidad."));
    }
}

#define FIELD(f) #f
#define SET(f, en, es, help_en, help_es, min, max) { FIELD(f), en, es, help_en, help_es, offsetof(settings, f), min, max }
const setting fields[SETTINGS_COUNT] = {
    SET(organize, "Organize packages", "Organizar los paquetes",
        "Moves packages into ps4/<title ID> on their drive.", "Mueve los paquetes a ps4/<ID del juego> en su unidad.", 0, 1),
    SET(internal, "Use internal storage", "Usar almacenamiento interno",
        "Scan the console’s /data/PS4Mount folder.", "Busca paquetes en /data/PS4Mount en la consola.", 0, 1),
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

static void show_settings(const panel_view *v, canvas c) {
    header(v, c, tr(v, "Settings", "Ajustes"));
    if (!v->config_ready) {
        text(c, 490, 332, 1050, 32, WHITE, v->loading_config ?
             tr(v, "Loading settings…", "Cargando ajustes…") :
             tr(v, "Settings are unavailable", "Ajustes no disponibles"));
        return;
    }
    const char *sections[] = {tr(v, "Library", "Biblioteca"), "DLC", tr(v, "Console", "Consola")};
    for (int i = 0; i < 3; i++) {
        int y = 260 + i * 54;
        stripe(c, LEFT, y, 285, 50, i == v->category && v->in_sidebar);
        text(c, LEFT + 16, y + 34, 246, 25, i == v->category ? WHITE : MUTED, sections[i]);
    }
    fill(c, 394, 254, 1, 680, BORDER);
    int count = setting_start[v->category + 1] - setting_start[v->category];
    for (int i = 0; i < count; i++) {
        const setting *f = &fields[setting_start[v->category] + i];
        int y = 260 + 58 * i;
        char temp[24];
        stripe(c, 440, y, 780, 50, !v->in_sidebar && v->setting_row == i);
        text(c, 456, y + 35, 521, 25, WHITE, v->spanish ? f->es : f->en);
        text(c, 1001, y + 35, 208, 22, TEXT, setting_text(v, f, *(const int *)((const char *)v->config + f->offset), temp, sizeof(temp)));
    }
    int y = 281 + 58 * count;
    stripe(c, 440, y, 780, 50, !v->in_sidebar && v->setting_row == count);
    text(c, 456, y + 35, 720, 26, BLUE, tr(v, "Save settings", "Guardar ajustes"));
    if (!v->in_sidebar && v->setting_row < count) {
        const setting *f = &fields[setting_start[v->category] + v->setting_row];
        fill(c, 1256, 260, 568, 178, 0x111b34);
        text(c, 1284, 312, 515, 32, WHITE, v->spanish ? f->es : f->en);
        const char *help = v->spanish ? f->help_es : f->help_en;
        char first[64];
        size_t split = strlen(help);
        if (split > 46) {
            split = 46;
            while (split && help[split] != ' ')
                split--;
        }
        snprintf(first, sizeof(first), "%.*s", (int)split, help);
        text(c, 1284, 360, 515, 22, TEXT, first);
        if (help[split])
            text(c, 1284, 393, 515, 22, TEXT, help + split + 1);
    }
}

static void show_phone(const panel_view *v, canvas c) {
    header(v, c, tr(v, "Send games", "Enviar juegos"));
    text(c, LEFT, 322, 1400, 34, WHITE,
         tr(v, "Open Lodestar on your phone or computer", "Abre Lodestar en tu móvil u ordenador"));
    text(c, LEFT, 397, 1500, 45, BLUE, *v->local_url ? v->local_url :
         tr(v, "http://<PS4 IP>:8770", "http://<IP de la PS4>:8770"));
    if (!*v->local_url)
        text(c, LEFT, 464, 1500, 28, MUTED,
             tr(v, "Use the address in the Lodestar startup notification.",
                "Usa la dirección del aviso de inicio de Lodestar."));
    text(c, LEFT, 512, 1500, 28, MUTED,
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
    header(v, c, tr(v, "Downloads", "Descargas"));
    if (!v->links_ready || !v->links->configured) {
        text(c, LEFT, 322, 1450, 34, WHITE, v->loading_links ?
             tr(v, "Reading the download queue…", "Leyendo la cola de descargas…") :
             tr(v, "Downloads are unavailable", "Descargas no disponibles"));
        return;
    }
    if (*v->entered_url) {
        text(c, LEFT, 304, 1728, 27, WHITE, v->entered_url);
        text(c, LEFT, 355, 1300, 25, TEXT,
             v->dests->count ? v->dests->items[v->dest].label : !v->dest_ready && (v->loading_destinations) ?
             tr(v, "Reading destinations…", "Leyendo destinos…") : tr(v, "No destination selected", "Sin destino seleccionado"));
        if (v->dests->count > 1)
            text(c, LEFT, 398, 1200, 22, MUTED, tr(v, "Left / Right to change drive", "Izquierda / derecha para cambiar de unidad"));
    } else {
        text(c, LEFT, 316, 1350, 28, TEXT,
             tr(v, "Add a direct link to a package or archive", "Añade un enlace directo a un paquete o archivo"));
    }
    if (v->links->active) {
        char progress[200];
        text(c, LEFT, 475, 1350, 30, WHITE, v->links->name);
        const char *phase = !strcmp(v->links->phase, "extracting") ? tr(v, "Extracting", "Extrayendo") :
                            !strcmp(v->links->phase, "probing") ? tr(v, "Checking link", "Comprobando enlace") :
                            !strcmp(v->links->phase, "retrying") ? tr(v, "Retrying", "Reintentando") :
                            tr(v, "Downloading", "Descargando");
        if (v->links->total > 0) {
            double ratio = (double)v->links->done / (double)v->links->total;
            snprintf(progress, sizeof(progress), "%s · %.0f %%", phase, ratio * 100);
            fill(c, LEFT, 540, RIGHT - LEFT, 4, BORDER);
            fill(c, LEFT, 540, (int)((ratio < 0 ? 0 : ratio > 1 ? 1 : ratio) * (RIGHT - LEFT)), 4, TEAL);
        } else {
            snprintf(progress, sizeof(progress), "%s · %.0f MB", phase, v->links->done / 1048576.0);
        }
        text(c, LEFT, 517, 1400, 23, TEAL, progress);
    } else if (*v->links->message) {
        text(c, LEFT, 490, 1650, 27, TEXT, v->links->message);
    } else if (!v->links->count) {
        text(c, LEFT, 490, 1650, 27, MUTED, tr(v, "The download queue is empty", "La cola de descargas está vacía"));
    }
    int shown = v->links->count < 6 ? v->links->count : 6;
    for (int i = 0; i < shown; i++) {
        int y = 590 + i * 51;
        const download *job = &v->links->jobs[i];
        fill(c, LEFT, y - 29, RIGHT - LEFT, 1, BORDER);
        text(c, LEFT, y + 7, 930, 24, WHITE, job->name);
        text(c, 1100, y + 7, 132, 22, *job->error ? RED : TEXT, download_state(v, job->state));
        text(c, 1250, y + 7, 574, 22, MUTED, job->error);
    }
    if (v->links->count > shown)
        text(c, LEFT, 921, 1600, 23, MUTED, tr(v, "Open the web panel to see all downloads.", "Abre el panel web para ver todas las descargas."));
}

void panel_draw(const panel_view *v, canvas c) {
    uint32_t color = 0x18234d;
    if (v->page == DETAIL) {
        const game *g = v->focused;
        const artwork *a = g ? view_art(v, g->id) : NULL;
        if (a && a->pixels)
            color = a->accent | 0x101010;
    }
    background(c, color);
    if (v->page == DETAIL)
        show_detail(v, c);
    else {
        star(c, 1770, 942, 252, 0x171b2d);
        fill(c, 1284, 148, 2, 2, 0x52699d);
        fill(c, 236, 765, 2, 2, 0x52699d);
        if (v->page == LIBRARY)
            show_library(v, c);
        else if (v->page == SETTINGS)
            show_settings(v, c);
        else if (v->page == PHONE)
            show_phone(v, c);
        else
            show_downloads(v, c);
    }
    footer(v, c);
}
