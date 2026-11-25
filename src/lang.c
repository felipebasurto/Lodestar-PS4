/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Languages: see lang.h. */
#include "lang.h"

#include <string.h>

int g_lang = LANG_EN;

void lang_set(int lang) { __atomic_store_n(&g_lang, lang, __ATOMIC_RELEASE); }

int lang_current(void) { return __atomic_load_n(&g_lang, __ATOMIC_ACQUIRE); }

static const char *const k_tags[LANG_COUNT] = {"en", "es"};

const char *lang_pick(const char *const *texts, size_t n) {
    int lang = lang_current();
    return lang >= 0 && (size_t)lang < n && texts[lang] ? texts[lang] : texts[0];
}

const char *lang_tag(int lang) { return lang >= 0 && lang < LANG_COUNT ? k_tags[lang] : k_tags[LANG_EN]; }

int lang_from_tag(const char *tag) {
    for (int i = 0; i < LANG_COUNT; i++)
        if (!strcmp(tag, k_tags[i]))
            return i;
    return -1;
}

int lang_from_console(int code) { return code == 3 || code == 20 ? LANG_ES : LANG_EN; }
