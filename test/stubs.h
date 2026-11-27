/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Last app_* call made by the web panel in the host tests. */
#pragma once
#include <stddef.h>
#include <stdint.h>

extern char g_stub_call[64], g_stub_arg[4096];
extern int g_stub_calls;
extern int g_stub_games_pad;
extern unsigned char g_stub_check_first, g_stub_check_last;
extern const char *g_stub_games_json;
extern const char *g_stub_config_json;
extern const char *g_stub_dests_json;
extern const char *g_stub_links_json;
extern const uint8_t *g_stub_icon_bytes;
extern size_t g_stub_icon_len;
