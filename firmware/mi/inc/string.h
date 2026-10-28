/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *memset(void *d, int c, unsigned n);              /* bsp/chip/libc.c */
void *memcpy(void *d, const void *s, unsigned n);
int memcmp(const void *a, const void *b, unsigned n);
#ifdef __cplusplus
}
#endif
