/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef ANALYSIS_EVENTS_H
#define ANALYSIS_EVENTS_H
#include "azmod-mod.h"
struct analysis_listener {
    az_analysis_complete_fn callback;
    void *user;
    uint64_t after;
};
/* Producer copies only, never calls listeners. Registrations and delivery are
 * message-thread operations. All listeners have independent journal cursors. */
void analysis_events_publish(const az_analysis_complete_v1 *event);
int analysis_events_listen(struct analysis_listener *, az_analysis_complete_fn, void *);
int analysis_events_take(struct analysis_listener *, az_analysis_complete_v1 *);
int analysis_events_core_listen(az_analysis_complete_fn, void *);
void analysis_events_core_tick(void);
#endif
