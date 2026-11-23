/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_ANALYSIS_SETTINGS_H
#define AZ_ANALYSIS_SETTINGS_H
#include "azconf.h"
#pragma GCC visibility push(hidden)
/* One-time settings data migration. A core value already present wins. */
int analysis_settings_migrate(struct azconf *settings);
#pragma GCC visibility pop
#endif
