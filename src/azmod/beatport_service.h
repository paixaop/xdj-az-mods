/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_BEATPORT_SERVICE_H
#define AZ_BEATPORT_SERVICE_H
#include "azmod-mod.h"
#pragma GCC visibility push(hidden)
/* One instance in azmod.so. Installation precedes firmware startup. The host
 * qualifies the firmware/weak-reference prerequisites and supplies the private
 * settings path. No process constructor or independent hook chain is installed. */
void bp_login_install(int supported, const char *settings_path);
int bp_login_ready(void);
/* Overlay-thread calls, after external caller authentication. The loader owns
 * slots and initialization admission. These calls never borrow input options. */
int bp_login_configure(int owner, int initializing, const az_beatport_login_options *options);
int bp_login_status(int owner, az_beatport_login_status *out);
int bp_login_forget(int owner);
int bp_login_owner(void);
void bp_login_set_active(int enabled);
/* Native message thread only; never called by the storage worker. */
void bp_login_message_tick(void);
#pragma GCC visibility pop
#endif
