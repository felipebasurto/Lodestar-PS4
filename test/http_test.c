/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The web panel server: routes, methods, title id checks and malformed requests, over a
 * real socket with the app_* calls recorded (stubs.c). Run with: bash test/run.sh */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "stubs.h"
#include "web.h"

static int g_failed, g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                   \
        }                                                                                                              \
    } while (0)

static char g_resp[1 << 20];

typedef struct {
    int fd;
    const char *later;
} late_t;

static void *write_later(void *arg) {
    late_t *l = arg;
    usleep(150000);
    if (write(l->fd, l->later, strlen(l->later)) < 0)
        perror("write");
    shutdown(l->fd, SHUT_WR);
    return NULL;
}

/* Sends req (then later, after a pause, when given) and returns the full response. */
static const char *ask2(const char *req, size_t len, const char *later) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
        return "";
    /* both directions on both ends: the request is written before the server reads it,
     * and macOS's small default buffers would block that write (Linux's do not) */
    int big = 1 << 20;
    for (int i = 0; i < 2; i++) {
        setsockopt(sv[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
        setsockopt(sv[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    }
    if (len && write(sv[0], req, len) < 0)
        perror("write");
    pthread_t th;
    late_t l = {sv[0], later};
    if (later)
        pthread_create(&th, NULL, write_later, &l);
    else
        shutdown(sv[0], SHUT_WR);
    g_stub_call[0] = 0;
    g_stub_arg[0] = 0;
    web_handle(sv[1]);
    if (later)
        pthread_join(th, NULL);
    size_t n = 0;
    ssize_t r;
    while (n < sizeof(g_resp) - 1 && (r = read(sv[0], g_resp + n, sizeof(g_resp) - 1 - n)) > 0)
        n += (size_t)r;
    g_resp[n] = 0;
    close(sv[0]);
    return g_resp;
}

static const char *ask(const char *req) { return ask2(req, strlen(req), NULL); }

static int status(const char *resp, const char *code) {
    char want[32];
    snprintf(want, sizeof(want), "HTTP/1.1 %s", code);
    return !strncmp(resp, want, strlen(want));
}

static const char *post(const char *path, const char *body) {
    static char req[16384];
    snprintf(req, sizeof(req), "POST %s HTTP/1.1\r\nHost: x\r\nContent-Length: %zu\r\n\r\n%s", path, strlen(body), body);
    return ask(req);
}

/* The panel's own request: post() without the header is a client of another site. */
static const char *postw(const char *path, const char *body) {
    static char req[16384];
    snprintf(req, sizeof(req), "POST %s HTTP/1.1\r\nHost: x\r\nX-PS4Mount: 1\r\nContent-Length: %zu\r\n\r\n%s", path,
             strlen(body), body);
    return ask(req);
}

static int occurrences(const char *text, const char *needle) {
    int n = 0;
    while ((text = strstr(text, needle))) {
        n++;
        text += strlen(needle);
    }
    return n;
}

/* The application-owned fragment of /api/status, in the shape main.c installs. */
static void test_status_state(buf_t *out) {
    static const char state[] = "\"setup\":true,\"activity\":{\"busy\":true,\"phase\":\"scanning\",\"drive\":\"usb0\","
                                "\"title\":\"Test Game\",\"title_id\":\"CUSA00001\",\"done\":2,\"total\":5,\"group\":1,"
                                "\"groups\":3,\"dlc_pending\":[{\"content_id\":\"EP0001-CUSA00001_00-TESTDLC000000000\","
                                "\"name\":\"Test DLC\",\"size\":123,\"elapsed\":4}]},\"notices\":[{\"ts\":123,\"kind\":\"info\","
                                "\"text\":\"hello\"}]";
    buf_put(out, state, sizeof(state) - 1);
}

/* One real TCP client of the listening socket used by web_serve/worker-pool tests. */
static int panel_conn(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a))) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    return fd;
}

static void send_req_len_close_write(int fd, const char *req, size_t len) {
    while (len) {
        ssize_t n = write(fd, req, len);
        if (n <= 0)
            break;
        req += n;
        len -= (size_t)n;
    }
    shutdown(fd, SHUT_WR);
}

static void send_req_close_write(int fd, const char *req) { send_req_len_close_write(fd, req, strlen(req)); }

static void read_response_close(int fd, char *out, size_t cap) {
    size_t n = 0;
    ssize_t r;
    while (n + 1 < cap && (r = read(fd, out + n, cap - 1 - n)) > 0)
        n += (size_t)r;
    out[n] = 0;
    close(fd);
}

static size_t drain_response_close(int fd) {
    char tmp[8192];
    size_t n = 0;
    ssize_t r;
    while ((r = read(fd, tmp, sizeof(tmp))) > 0)
        n += (size_t)r;
    close(fd);
    return n;
}

int main(void) {
    const char *r;
    printf("pages\n");
    r = ask("GET / HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "text/html") && strstr(r, "<title>Lodestar</title>"));
    CHECK(strstr(r, "location.replace('/legacy'") != NULL);
    r = ask("GET /?app=1 HTTP/1.1\r\n\r\n"); /* the panel app */
    CHECK(status(r, "200") && strstr(r, "<title>Lodestar</title>"));
    r = ask("POST /api/app/close HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 0\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "\"ok\":true"));
    r = ask("GET /legacy HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "id=\"v-pick\"") && !strstr(r, "</dialog>"));
    r = ask("GET /nothing HTTP/1.1\r\n\r\n");
    CHECK(status(r, "404"));
    r = ask("GET /api/games HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "application/json") && strstr(r, "\"games\""));
    CHECK(strstr(r, "Cache-Control: no-store") != NULL);

    printf("agent observability\n");
    r = ask("GET /api/agent HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "application/json"));
    CHECK(strstr(r, "\"product\":\"Lodestar\"") && strstr(r, "\"version\":\"" LODESTAR_VERSION "\"") &&
          strstr(r, "\"api_version\":1"));
    CHECK(strstr(r, "\"panel\":{\"scheme\":\"http\",\"port\":8780}") != NULL);
    CHECK(strstr(r, "\"header\":\"X-Lodestar\"") && strstr(r, "\"legacy_aliases\":[\"X-PS4Mount\"]"));
    CHECK(strstr(r, "\"safety_rails\"") && strstr(r, "\"system_databases_read_only\":true") &&
          strstr(r, "\"system_delete_never_reaches_pkg\":true"));
    CHECK(!strstr(r, "/api/drive") && !strstr(r, "/api/nas")); /* removed: USB drives only */
    int endpoints = occurrences(r, "\"method\"");
    CHECK(endpoints >= 20 && endpoints == occurrences(r, "\"path\"") && endpoints == occurrences(r, "\"mutating\"") &&
          endpoints == occurrences(r, "\"summary\""));
    CHECK(strstr(r, "{\"method\":\"GET\",\"path\":\"/api/status\",\"mutating\":false") != NULL);
    CHECK(strstr(r, "\"path\":\"/api/upload\",\"mutating\":true") != NULL);
    r = ask("POST /api/agent HTTP/1.1\r\nX-Lodestar: 1\r\nContent-Length: 0\r\n\r\n");
    CHECK(status(r, "404"));

    r = ask("GET /api/status HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "\"product\":\"Lodestar\"") && strstr(r, "\"version\":\"" LODESTAR_VERSION "\""));
    CHECK(strstr(r, "\"setup\":false") && strstr(r, "\"activity\":{\"busy\":false") && strstr(r, "\"notices\":[]"));
    CHECK(strstr(r, "\"queues\":{\"web_owner_pending\":0,\"web_write_pending\":0,\"web_write_active\":0}") != NULL);
    CHECK(strstr(r, "\"endpoints\":{\"agent\":\"/api/agent\",\"status\":\"/api/status\"") != NULL);
    CHECK(!strstr(r, "\"games\":["));
    web_status_provider(test_status_state);
    r = ask("GET /api/status HTTP/1.1\r\n\r\n");
    CHECK(strstr(r, "\"setup\":true") && strstr(r, "\"phase\":\"scanning\"") && strstr(r, "\"dlc_pending\"") &&
          strstr(r, "\"text\":\"hello\""));
    CHECK(strstr(r, "\"queues\":{\"web_owner_pending\":0") != NULL);
    r = ask("POST /api/status HTTP/1.1\r\nX-Lodestar: 1\r\nContent-Length: 0\r\n\r\n");
    CHECK(status(r, "404"));
    r = ask("HEAD /api/status HTTP/1.1\r\n\r\n");
    CHECK(status(r, "404"));

    r = ask("GET /api/log HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "{\"log\":\"") && strstr(r, "\"bytes\":") && strstr(r, "\"truncated\":") &&
          strstr(r, "\"path\":\"/data/lodestar/log.txt\""));

    printf("state changes need POST\n");
    const char *changing[] = {"/api/mount?title_id=CUSA00001", "/api/unmount?title_id=CUSA00001", "/api/rescan",
                              "/api/restore", "/api/restored/forget", "/api/stop", NULL};
    for (int i = 0; changing[i]; i++) {
        char req[256];
        snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\n\r\n", changing[i]);
        int before = g_stub_calls;
        r = ask(req);
        CHECK(status(r, "404") && g_stub_calls == before);
    }

    printf("every POST needs the panel's header\n");
    /* a page of another site can POST a form but cannot send the header (CORS preflight) */
    const char *posts[] = {"/api/mount?title_id=CUSA00001", "/api/unmount?title_id=CUSA00001", "/api/rescan",
                           "/api/restore", "/api/restored/forget", "/api/config", "/api/app/close", "/api/stop", NULL};
    for (int i = 0; posts[i]; i++) {
        int before = g_stub_calls;
        r = post(posts[i], "x=1");
        CHECK(status(r, "403") && g_stub_calls == before);
    }
    /* the header line inside the body is not a header */
    int before = g_stub_calls;
    r = post("/api/config", "x=1\r\nX-PS4Mount: 1");
    CHECK(status(r, "403") && g_stub_calls == before);
    r = post("/api/config", "x=1\r\nX-Lodestar: 1");
    CHECK(status(r, "403") && g_stub_calls == before);
    r = ask("POST /api/rescan HTTP/1.1\r\nX-Lodestar: 1\r\nContent-Length: 0\r\n\r\n");
    CHECK(status(r, "200") && !strcmp(g_stub_call, "rescan-hard"));
    /* the NAS and Google Drive endpoints are gone: every one answers 404 */
    const char *gone_get[] = {"/api/nas/scan", "/api/nas/shares", "/api/nas/reload", "/api/drive", NULL};
    for (int i = 0; gone_get[i]; i++) {
        char req[128];
        snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\n\r\n", gone_get[i]);
        int before = g_stub_calls;
        r = ask(req);
        CHECK(status(r, "404") && g_stub_calls == before);
    }
    const char *gone_post[] = {"/api/nas/scan", "/api/nas/shares", "/api/nas/reload",
                               "/api/drive/connect", "/api/drive/client", NULL};
    for (int i = 0; gone_post[i]; i++) {
        char req[160];
        snprintf(req, sizeof(req), "POST %s HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 0\r\n\r\n", gone_post[i]);
        int before = g_stub_calls;
        r = ask(req);
        CHECK(status(r, "404") && g_stub_calls == before);
    }

    printf("title ids\n");
    r = postw("/api/mount?title_id=CUSA00791", "");
    CHECK(status(r, "200") && !strcmp(g_stub_call, "add") && !strcmp(g_stub_arg, "CUSA00791"));
    r = postw("/api/unmount?title_id=CUSA04263", "");
    CHECK(!strcmp(g_stub_call, "remove") && !strcmp(g_stub_arg, "CUSA04263"));
    const char *bad[] = {"/api/mount?title_id=CUSA0079", "/api/mount?title_id=../../../", "/api/mount?title_id=PPSA00001",
                         "/api/mount?title_id=cusa00791", "/api/mount?title_id=", "/api/mount", "/api/unmount?title_id=CUSA%2F%2E%2E",
                         NULL};
    for (int i = 0; bad[i]; i++) {
        int before = g_stub_calls;
        r = postw(bad[i], "");
        CHECK(status(r, "400") && g_stub_calls == before);
    }
    /* icons are read by title id: nothing else may reach the file system */
    const char *bad_icons[] = {"/icon/../../../", "/icon/..%2F..%2F", "/icon/CUSA0079", "/icon/", "/icon/CUSA00791/../x", NULL};
    for (int i = 0; bad_icons[i]; i++) {
        char req[128];
        snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\n\r\n", bad_icons[i]);
        int before = g_stub_calls;
        r = ask(req);
        CHECK(status(r, "404") && g_stub_calls == before);
    }
    r = ask("GET /icon/CUSA00791 HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "image/png") && !strcmp(g_stub_arg, "CUSA00791"));
    r = ask("GET /pic/CUSA00791 HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && !strcmp(g_stub_call, "pic") && !strcmp(g_stub_arg, "CUSA00791"));
    int calls = g_stub_calls;
    r = ask("GET /pic/../../../x HTTP/1.1\r\n\r\n");
    CHECK(status(r, "404") && g_stub_calls == calls);

    printf("moving back\n");
    postw("/api/restore", "");
    CHECK(!strcmp(g_stub_call, "restore") && !strcmp(g_stub_arg, "(null)")); /* all games */
    postw("/api/restore?title_id=CUSA00785", "");
    CHECK(!strcmp(g_stub_arg, "CUSA00785"));
    postw("/api/restore", "title_ids=CUSA00785,CUSA04263");
    CHECK(!strcmp(g_stub_arg, "CUSA00785,CUSA04263"));
    postw("/api/restore", "title_ids=CUSA00785%2CCUSA04263%2cCUSA00791");
    CHECK(!strcmp(g_stub_arg, "CUSA00785,CUSA04263,CUSA00791"));
    postw("/api/restore", "title_ids=CUSA00785&x=1");
    CHECK(!strcmp(g_stub_arg, "CUSA00785"));
    before = g_stub_calls;
    r = postw("/api/restorex", "");
    CHECK(status(r, "404") && g_stub_calls == before);
    r = postw("/api/restore?title_id=bad", "");
    CHECK(status(r, "400"));

    printf("settings\n");
    postw("/api/config", "batch=1&restore_batch=5");
    CHECK(!strcmp(g_stub_call, "config") && !strcmp(g_stub_arg, "batch=1&restore_batch=5"));
    r = ask("GET /api/config HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "\"batch\""));
    postw("/api/restored/forget", "");
    CHECK(!strcmp(g_stub_call, "forget"));

    printf("sending pkgs\n");
    /* writing files needs the panel's header (no other site can send it) */
    before = g_stub_calls;
    r = post("/api/check?size=12345", "0123456789");
    CHECK(status(r, "403") && g_stub_calls == before);
    r = post("/api/upload?dest=usb0&name=x.pkg", "0123456789");
    CHECK(status(r, "403") && g_stub_calls == before);
    r = ask("POST /api/check?size=12345 HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 10\r\n\r\n0123456789");
    CHECK(status(r, "200") && !strcmp(g_stub_call, "check") && !strcmp(g_stub_arg, "10 12345"));
    r = ask("POST /api/check HTTP/1.1\r\nx-ps4mount: 1\r\nContent-Length: 3\r\n\r\nabc");
    CHECK(status(r, "200") && !strcmp(g_stub_arg, "3 0")); /* the header name in any case */
    /* a body bigger than the first MB is refused before it is read */
    before = g_stub_calls;
    r = ask("POST /api/check HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 999999999\r\n\r\nabc");
    CHECK(status(r, "400") && g_stub_calls == before);
    /* a body that does not arrive whole */
    r = ask("POST /api/check HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 100\r\n\r\nabc");
    CHECK(status(r, "400") && g_stub_calls == before);
    r = ask("POST /api/upload?dest=usb0&name=My%20Game.pkg HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 10\r\n\r\n"
            "0123456789");
    CHECK(status(r, "400") && !strcmp(g_stub_call, "upload") && strstr(g_stub_arg, "name=My%20Game.pkg"));
    r = ask("GET /api/check HTTP/1.1\r\n\r\n");
    CHECK(status(r, "404"));
    r = ask("GET /api/uploads HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "\"state\""));

    printf("downloads from links\n");
    r = ask("GET /api/links HTTP/1.1\r\n\r\n");
    CHECK(status(r, "200") && strstr(r, "\"jobs\""));
    before = g_stub_calls;
    r = post("/api/links/add?dest=usb0", "http://example.com/a.pkg");
    CHECK(status(r, "403") && g_stub_calls == before); /* downloads write files: the panel's header */
    r = ask("POST /api/links/add?dest=usb0 HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 24\r\n\r\nhttp://example.com/a.pkg");
    CHECK(status(r, "200") && !strcmp(g_stub_call, "links_add") && !strcmp(g_stub_arg, "usb0|24|http://example.com/a.pkg|"));
    /* an archive's password, percent-encoded in its own header */
    r = ask("POST /api/links/add?dest=usb0 HTTP/1.1\r\nX-Lodestar: 1\r\nX-Lodestar-Password: a%20b%26c%C3%B1\r\nContent-Length: 24\r\n\r\n"
            "http://example.com/a.pkg");
    CHECK(status(r, "200") && !strcmp(g_stub_arg, "usb0|24|http://example.com/a.pkg|a b&c\xc3\xb1"));
    r = ask("POST /api/links/add?dest=usb0 HTTP/1.1\r\nX-PS4Mount: 1\r\nX-PS4Mount-Password: legacy\r\nContent-Length: 24\r\n\r\n"
            "http://example.com/a.pkg");
    CHECK(status(r, "200") && !strcmp(g_stub_arg, "usb0|24|http://example.com/a.pkg|legacy"));
    before = g_stub_calls;
    r = ask("POST /api/links/add?dest=../x HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 3\r\n\r\nabc");
    CHECK(status(r, "400") && g_stub_calls == before);
    r = ask("POST /api/links/add?dest=usb0 HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 99999999\r\n\r\nabc");
    CHECK(status(r, "400") && g_stub_calls == before);
    {
        /* many links: a body far past the request buffer, sent after the headers */
        static char many[60001];
        memset(many, 'x', sizeof(many) - 1);
        memcpy(many, "http://e.com/", 13);
        char h[200];
        snprintf(h, sizeof(h), "POST /api/links/add?dest=internal HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: %zu\r\n\r\n",
                 sizeof(many) - 1);
        r = ask2(h, strlen(h), many);
        CHECK(status(r, "200") && !strncmp(g_stub_arg, "internal|60000|http://e.com/xxx", 31));
    }
    r = ask("POST /api/links/cancel?id=7 HTTP/1.1\r\nX-PS4Mount: 1\r\n\r\n");
    CHECK(status(r, "200") && !strcmp(g_stub_call, "links_cancel") && !strcmp(g_stub_arg, "7"));
    r = ask("POST /api/links/retry?id=3 HTTP/1.1\r\nX-PS4Mount: 1\r\n\r\n");
    CHECK(status(r, "200") && !strcmp(g_stub_call, "links_retry") && !strcmp(g_stub_arg, "3|"));
    r = ask("POST /api/links/retry?id=3 HTTP/1.1\r\nX-PS4Mount: 1\r\nx-lodestar-password: secret\r\n\r\n");
    CHECK(status(r, "200") && !strcmp(g_stub_arg, "3|secret"));
    before = g_stub_calls;
    r = ask("POST /api/links/cancel HTTP/1.1\r\nX-PS4Mount: 1\r\n\r\n");
    CHECK(status(r, "400") && g_stub_calls == before);
    r = ask("POST /api/links/start HTTP/1.1\r\nX-PS4Mount: 1\r\n\r\n");
    CHECK(status(r, "404") && g_stub_calls == before);

    printf("a body that arrives after the headers\n");
    const char *head = "POST /api/config HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 15\r\n\r\n";
    ask2(head, strlen(head), "restore_batch=7");
    CHECK(!strcmp(g_stub_call, "config") && !strcmp(g_stub_arg, "restore_batch=7"));

    printf("malformed requests\n");
    r = ask("");
    CHECK(r[0] == 0); /* nothing sent: closed without an answer */
    r = ask("\r\n\r\n");
    CHECK(status(r, "404"));
    before = g_stub_calls;
    r = ask("POST /api/config HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: -5\r\n\r\nx=1");
    CHECK(status(r, "400") && g_stub_calls == before);
    r = ask("POST /api/config HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 999999999999\r\n\r\nx=1");
    CHECK(status(r, "400") && g_stub_calls == before);
    r = ask("POST /api/config HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 15\r\n\r\nx=1");
    CHECK(status(r, "400") && g_stub_calls == before);
    static char huge[20000];
    memset(huge, 'A', sizeof(huge) - 1);
    memcpy(huge, "GET /", 5);
    r = ask2(huge, sizeof(huge) - 1, NULL);
    CHECK(status(r, "400"));
    char nul[] = "GET /\0api/stop HTTP/1.1\r\n\r\n";
    before = g_stub_calls;
    ask2(nul, sizeof(nul) - 1, NULL);
    CHECK(g_stub_calls == before);

    printf("answers are valid JSON strings\n");
    r = postw("/api/rescan", "");
    CHECK(strstr(r, "\"message\":\"done \\\"rescan\\\"\"") != NULL || strstr(r, "Looking for games") != NULL);
    r = postw("/api/mount?title_id=CUSA00001", "");
    CHECK(strstr(r, "\"message\":\"done \\\"add\\\"\"") != NULL);

    printf("bounded workers keep owner execution ordered\n");
    int ls = web_listen();
    CHECK(ls >= 0);
    if (ls >= 0) {
        char resp[4096];
        int before = g_stub_calls;
        int c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "POST /api/mount?title_id=CUSA00001 HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 0\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 1);
        CHECK(g_stub_calls == before); /* parsed concurrently, but not executed by the worker */
        web_owner_pump();
        CHECK(g_stub_calls == before + 1 && !strcmp(g_stub_call, "add"));
        CHECK(web_owner_pending() == 0);
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200"));

        g_stub_games_pad = 768 * 1024;
        c = panel_conn();
        CHECK(c >= 0);
        if (c >= 0) {
            int small = 2048;
            setsockopt(c, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
            send_req_close_write(c, "GET /api/games HTTP/1.1\r\n\r\n");
            web_serve(ls);
            size_t bytes = drain_response_close(c); /* direct worker transmission can wait for this reader */
            web_pool_idle_wait();
            CHECK(web_owner_pending() == 0);
            CHECK(!web_write_busy()); /* no owner capture/writer handoff is needed for games */
            CHECK(bytes > (size_t)g_stub_games_pad);
        }
        g_stub_games_pad = 0;

        size_t check_len = 9000; /* past the header buffer: the worker must complete it */
        char *check_req = malloc(check_len + 256);
        CHECK(check_req != NULL);
        if (check_req) {
            int head_len = snprintf(check_req, check_len + 256,
                                    "POST /api/check?size=9999 HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: %zu\r\n\r\n",
                                    check_len);
            memset(check_req + head_len, 'b', check_len);
            check_req[head_len] = 'A';
            check_req[head_len + check_len - 1] = 'Z';
            before = g_stub_calls;
            c = panel_conn();
            CHECK(c >= 0);
            send_req_len_close_write(c, check_req, (size_t)head_len + check_len);
            web_serve(ls);
            web_pool_idle_wait();
            CHECK(web_owner_pending() == 0);
            CHECK(g_stub_calls == before + 1 && !strcmp(g_stub_call, "check")); /* complete in the worker */
            CHECK(!strcmp(g_stub_arg, "9000 9999") && g_stub_check_first == 'A' && g_stub_check_last == 'Z');
            read_response_close(c, resp, sizeof(resp));
            CHECK(status(resp, "200"));
            free(check_req);
        }

        before = g_stub_calls;
        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "POST /api/check HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 32\r\n\r\nabc");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        CHECK(g_stub_calls == before); /* short body: the historical error, without app_check or owner work */
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "400"));

        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "GET /api/links HTTP/1.1\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200") && strstr(resp, "\"jobs\""));

        before = g_stub_calls;
        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "POST /api/links/cancel?id=7 HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 0\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        CHECK(g_stub_calls == before + 1 && !strcmp(g_stub_call, "links_cancel"));
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200"));

        before = g_stub_calls;
        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "POST /api/links/retry?id=3 HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 0\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        CHECK(g_stub_calls == before + 1 && !strcmp(g_stub_call, "links_retry"));
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200"));

        before = g_stub_calls;
        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "POST /api/rescan HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 0\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        CHECK(g_stub_calls == before + 1 && !strcmp(g_stub_call, "rescan-hard"));
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200"));

        before = g_stub_calls;
        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "POST /api/stop HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 0\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        CHECK(g_stub_calls == before + 1 && !strcmp(g_stub_call, "stop"));
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200"));

        before = g_stub_calls;
        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "POST /api/links/add?dest=usb0 HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 24\r\n\r\n"
                                    "http://example.com/a.pkg");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        CHECK(g_stub_calls == before + 1 && !strcmp(g_stub_call, "links_add"));
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200"));

        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "GET /api/dests HTTP/1.1\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200") && strstr(resp, "\"dests\""));

        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "GET /api/storage HTTP/1.1\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200") && strstr(resp, "\"free\":1"));

        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "GET /api/agent HTTP/1.1\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200") && strstr(resp, "\"product\":\"Lodestar\""));

        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "GET /api/status HTTP/1.1\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200") && strstr(resp, "\"phase\":\"scanning\"") && strstr(resp, "\"web_owner_pending\":0"));

        before = g_stub_calls;
        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "GET /icon/CUSA00791 HTTP/1.1\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        CHECK(g_stub_calls == before + 1 && !strcmp(g_stub_call, "icon"));
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200"));

        before = g_stub_calls;
        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "GET /api/config HTTP/1.1\r\n\r\n");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 0);
        CHECK(g_stub_calls == before); /* the config stub does not use record(): the response is the evidence */
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200") && strstr(resp, "\"batch\":true"));

        before = g_stub_calls;
        c = panel_conn();
        CHECK(c >= 0);
        send_req_close_write(c, "POST /api/config HTTP/1.1\r\nX-PS4Mount: 1\r\nContent-Length: 15\r\n\r\nrestore_batch=7");
        web_serve(ls);
        web_pool_idle_wait();
        CHECK(web_owner_pending() == 1);
        CHECK(g_stub_calls == before); /* changing settings remains owner work */
        web_owner_pump();
        CHECK(g_stub_calls == before + 1 && !strcmp(g_stub_call, "config"));
        read_response_close(c, resp, sizeof(resp));
        CHECK(status(resp, "200"));

        close(ls);
        web_stop();
        CHECK(web_owner_pending() == 0);
    }

    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed != 0;
}
