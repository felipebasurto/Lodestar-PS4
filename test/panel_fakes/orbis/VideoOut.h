/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

enum {
    ORBIS_VIDEO_USER_MAIN = 0xff,
    ORBIS_VIDEO_OUT_BUS_MAIN = 0,
    ORBIS_VIDEO_OUT_TILING_MODE_LINEAR = 1,
    ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9 = 0,
    ORBIS_VIDEO_OUT_FLIP_VSYNC = 1,
};

typedef struct {
    uint32_t pixelFormat;
    uint32_t tilingMode;
    uint32_t aspectRatio;
    uint32_t width;
    uint32_t height;
    uint32_t pitchInPixel;
} OrbisVideoOutBufferAttribute;

typedef struct {
    uint64_t count;
    uint64_t processTime;
    uint64_t tsc;
    int64_t flipArg;
    uint64_t submitTsc;
    int32_t numFlipPending;
} OrbisVideoOutFlipStatus;

int sceVideoOutOpen(int32_t userId, int32_t type, int32_t index, const void *param);
int sceVideoOutClose(int32_t handle);
void sceVideoOutSetBufferAttribute(OrbisVideoOutBufferAttribute *attr, uint32_t pixelFormat, uint32_t tilingMode,
                                   uint32_t aspectRatio, uint32_t width, uint32_t height, uint32_t pitchInPixel);
int sceVideoOutRegisterBuffers(int32_t handle, int32_t startIndex, void *const *addresses, int32_t bufferNum,
                               const OrbisVideoOutBufferAttribute *attr);
int sceVideoOutUnregisterBuffers(int32_t handle, int32_t attributeIndex);
int sceVideoOutSubmitFlip(int32_t handle, int32_t bufferIndex, uint32_t flipMode, int64_t flipArg);
int sceVideoOutGetFlipStatus(int32_t handle, OrbisVideoOutFlipStatus *status);
