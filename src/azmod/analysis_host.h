/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_ANALYSIS_HOST_H
#define AZ_ANALYSIS_HOST_H
#include "analysis_runtime.h"
#pragma GCC visibility push(hidden)
/* Startup and enablement run on the overlay thread. Notices run on the native
 * message thread; they only present copied results and never advance work. */
int analysis_host_start(const char *assets, int enabled);
void analysis_host_enable(int enabled);
void analysis_host_configure(uint32_t components);
void analysis_host_notices(void (*banner)(const char *, int), void (*diagnostic)(const char *));
/* A normal process exit releases its resources through the OS. An explicit
 * service shutdown must run off native/UI threads and joins all computation. */
void analysis_host_shutdown(void);
#pragma GCC visibility pop
#endif
