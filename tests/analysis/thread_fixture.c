/* SPDX-License-Identifier: GPL-3.0-only */
/* Standalone host dependency uses the same production thread trampoline. */
#define _GNU_SOURCE
#include "../../src/azmod/analysis_native_adapter.h"
#include "../../src/common/azaltstack.h"
int az_analysis_thread_create(pthread_t *thread, const pthread_attr_t *attributes,
                              void *(*entry)(void *), void *argument) {
    return az_thread_create(thread, attributes, entry, argument);
}
