/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef TA_ALLOCATOR_H
#define TA_ALLOCATOR_H
#include <stdlib.h>
#include "mimalloc.h"
/* Private explicit calls; never replace the firmware process allocator. */
static inline void *ta_malloc(size_t size) { return mi_malloc(size); }
static inline void *ta_calloc(size_t count, size_t size) { return mi_calloc(count, size); }
static inline void *ta_realloc(void *pointer, size_t size) { return mi_realloc(pointer, size); }
static inline void ta_free(void *pointer) { mi_free(pointer); }
#endif
