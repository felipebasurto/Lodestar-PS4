/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PANEL-5: the controller client speaks the same HTTP/JSON contract as the browser. */
#include "../panel-app/api.h"
#include "web.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

_Static_assert((int)API_PORT == WEB_PORT, "the native app uses the daemon's port");

typedef struct {
    int fd;
    const char *reply;
} server;

static void *serve(void *arg) {
    server *s = arg;
    char req[512] = {0};
    size_t used = 0;
    while (!strstr(req, "\r\n\r\n") || !strstr(req, "dlc_mode=external")) {
        assert(used + 1 < sizeof(req));
        ssize_t n = read(s->fd, req + used, sizeof(req) - used - 1);
        assert(n > 0);
        used += (size_t)n;
    }
    assert(!strncmp(req, "POST /api/config HTTP/1.1\r\n", 27));
    assert(strstr(req, "\r\nX-Lodestar: 1\r\n"));
    assert(strstr(req, "\r\nContent-Length: 17\r\n\r\ndlc_mode=external"));
    size_t n = strlen(s->reply);
    for (size_t i = 0; i < n; ++i)
        assert(write(s->fd, s->reply + i, 1) == 1);
    close(s->fd);
    return NULL;
}

static response exchange(const char *wire, size_t limit) {
    int fd[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fd));
    server s = {fd[1], wire};
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, serve, &s));
    response r = api_exchange(fd[0], "/api/config", "dlc_mode=external", limit, 2000);
    assert(!pthread_join(thread, NULL));
    close(fd[0]);
    return r;
}

static response json(const char *text) {
    return (response){.status = 200, .size = strlen(text), .body = (char *)text};
}

int main(void) {
    response r = exchange("HTTP/1.1 200 OK\r\nContent-Length: 33\r\nConnection: close\r\n\r\n"
                          "{\"ok\":true,\"message\":\"All saved\"}", 512);
    char message[32];
    assert(r.status == 200 && !api_result(&r, message, sizeof(message)));
    assert(!strcmp(message, "All saved"));
    free(r.body);
    r = exchange("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort", 512);
    assert(!r.body && !r.status);
    r = exchange("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n", 32);
    assert(!r.body && !r.status);
    r = exchange("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n", 32);
    assert(!r.body && !r.status);
    r = json("{\"ok\":false,\"error\":\"Drive unavailable\"}");
    r.status = 400;
    assert(api_result(&r, message, sizeof(message)) && !strcmp(message, "Drive unavailable"));

    library *games = calloc(1, sizeof(*games));
    assert(games);
    r = json("{\"lang\":\"es\",\"setup\":true,\"version\":\"Lodestar 3.0.0\",\"drives\":["
             "{\"label\":\"usb0\",\"free\":100000000000}],\"games\":["
             "{\"title_id\":\"CUSA00023\",\"name\":\"Título \\\"A\\\"\",\"state\":\"restart\",\"in_place\":true},"
             "{\"title_id\":\"a/invalid\",\"state\":\"ready\"},"
             "{\"title_id\":\"CUSA12345\",\"state\":\"offline\",\"source\":\"/mnt/usb1/ps4\"}]}");
    assert(!api_library(&r, games));
    assert(games->spanish && games->setup && games->count == 2);
    assert(games->drive_count == 1 && games->drives[0].free_bytes == 100000000000LL);
    assert(!strcmp(games->games[0].name, "Título \"A\"") && games->games[0].state == RESTART);
    assert(games->games[0].in_place && games->games[1].state == OFFLINE);
    assert(!strcmp(games->games[1].id, "CUSA12345"));
    memset(games, 0, sizeof(*games));
    r = json("{\"games\":[]}");
    assert(!api_library(&r, games) && !games->count);
    r = json("{\"games\":{}}");
    assert(api_library(&r, games));
    free(games);

    settings c;
    r = json("{\"dlc_mode\":\"external\",\"lang\":\"es\",\"batch_size\":12,\"internal\":true}");
    assert(!api_settings(&r, &c) && c.internal && !c.dlc_mode && c.language == 2 && c.batch_size == 12);
    r = json("{\"activity\":{\"phase\":\"scanning\"}}");
    assert(api_scan_active(&r) == 1);

    downloads list = {0};
    r = json("{\"configured\":true,\"jobs\":[{\"name\":\"Game.zip\",\"state\":\"running\"}],"
             "\"job\":{\"busy\":true,\"name\":\"Game.zip\",\"done\":2000,\"total\":3000}}");
    assert(!api_downloads(&r, &list) && list.configured && list.count == 1 && list.active);
    assert(list.done == 2000 && list.total == 3000 && !strcmp(list.jobs[0].name, "Game.zip"));
    destinations targets;
    r = json("{\"dests\":[{\"id\":\"usb0&x=1\",\"label\":\"Bad\"},{\"id\":\"usb0\",\"label\":\"USB 0\"},"
             "{\"id\":\"internal\",\"label\":\"Console\"}]}");
    assert(!api_destinations(&r, &targets) && targets.count == 2);
    assert(!strcmp(targets.items[0].id, "usb0") && !strcmp(targets.items[1].label, "Console"));
    puts("PANEL-5 native HTTP client and API responses hold");
    return 0;
}
