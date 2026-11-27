/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    ORBIS_DIALOG_STATUS_NONE = 0,
    ORBIS_DIALOG_STATUS_RUNNING = 1,
    ORBIS_DIALOG_STATUS_STOPPED = 2,
};

enum {
    ORBIS_DIALOG_OK = 0,
    ORBIS_DIALOG_CANCELED = 1,
};

enum {
    ORBIS_BUTTON_LABEL_DEFAULT = 0,
    ORBIS_BUTTON_LABEL_SEARCH = 1,
};

enum {
    ORBIS_TYPE_DEFAULT = 0,
    ORBIS_TYPE_TYPE_URL = 1,
};

enum {
    ORBIS_H_CENTER = 1,
    ORBIS_V_CENTER = 1,
};

typedef struct {
    int32_t userId;
    uint32_t type;
    uint64_t supportedLanguages;
    uint32_t enterLabel;
    uint32_t inputMethod;
    void *filter;
    uint32_t option;
    uint32_t maxTextLength;
    wchar_t *inputTextBuffer;
    float posx;
    float posy;
    uint32_t horizontalAlignment;
    uint32_t verticalAlignment;
    void *work;
    void *arg;
} OrbisImeDialogSetting;

typedef int OrbisDialogStatus;

typedef struct {
    int32_t endstatus;
} OrbisDialogResult;

int sceImeDialogInit(OrbisImeDialogSetting *param, void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(OrbisDialogResult *result);
int sceImeDialogAbort(void);
int sceImeDialogTerm(void);
