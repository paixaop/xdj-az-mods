/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_SLIP_DIAGNOSTIC_H
#define AZ_SLIP_DIAGNOSTIC_H
#include <stdint.h>
/* Core startup only. Instrumentation is process-owned; the bounded worker and
 * installed original entry remain valid until process exit, including failure. */
struct az_slip_diagnostic_host {
    void (*log)(const char *format, ...);
    int (*start_worker)(void *(*run)(void *), void *);
    int (*install)(uintptr_t address, void *replacement, void **original);
};
int az_slip_diagnostic_start(const char *firmware_tag, const struct az_slip_diagnostic_host *);
#endif
