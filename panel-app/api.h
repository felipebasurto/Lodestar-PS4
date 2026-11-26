/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum { GAME_LIMIT = 4096, DRIVE_LIMIT = 16 };
enum game_state { UNKNOWN, READY, RESTART, RUNNING, REMOVED, OFFLINE, INTERNAL, PENDING, DUPLICATE };

typedef struct {
    char id[10], name[192], source[384], storage[48], version[24], firmware[24], issue[160];
    enum game_state state;
    long long bytes, dlc;
    int in_place;
} game;

typedef struct {
    char label[48];
    long long free_bytes;
} drive;

typedef struct {
    int count, drive_count, spanish, setup, overflow;
    char version[48];
    drive drives[DRIVE_LIMIT];
    game games[GAME_LIMIT];
} library;

typedef struct {
    int organize, internal, dlc_mode, dlc_big, language, autorun, batch, batch_size, klog;
} settings;

typedef struct {
    int status;
    size_t size;
    char *body;
} response;

enum { API_PORT = 8780 };
/* fd is an already connected stream. It stays owned by the caller. */
response api_exchange(int fd, const char *path, const char *form, size_t limit, int timeout_ms);
response api_request(const char *path, const char *form, size_t limit, int timeout_ms);
int api_library(const response *res, library *out);
int api_settings(const response *res, settings *out);
int api_result(const response *res, char *message, size_t size);
int api_scan_active(const response *res);

enum { DOWNLOAD_LIMIT = 32 };

typedef struct {
    char name[128], state[16], error[160];
} download;

typedef struct {
    int configured, count, active;
    char name[128], phase[24], message[160];
    long long done, total;
    download jobs[DOWNLOAD_LIMIT];
} downloads;

typedef struct {
    int count;
    struct { char id[16], label[48]; } items[DRIVE_LIMIT];
} destinations;

int api_downloads(const response *res, downloads *out);
int api_destinations(const response *res, destinations *out);
