/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <sys/types.h>

size_t sceKernelGetDirectMemorySize(void);
int sceKernelAllocateDirectMemory(off_t searchStart, off_t searchEnd, size_t len, size_t alignment, int memoryType,
                                  off_t *physAddrOut);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, off_t directMemoryStart, size_t alignment);
int sceKernelReleaseDirectMemory(off_t start, size_t len);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelUsleep(unsigned int microseconds);

enum {
    ORBIS_KERNEL_WC_GARLIC = 3,
};
