/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_ANALYSIS_NATIVE_ADAPTER_H
#define AZ_ANALYSIS_NATIVE_ADAPTER_H
#include "azmod-mod.h"
#include <pthread.h>
/* Host glue: stable configuration, one native thread trampoline and guarded spool admission. */
const char *az_analysis_state_dir(void);
int az_analysis_thread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int az_analysis_spool_admit(int stick);
int az_status_loaded_reference(unsigned deck, uint64_t load, az_track_ref *out);
#endif
