/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_PVDI_MODEL_HOST_H
#define AZ_PVDI_MODEL_HOST_H

#include <stdbool.h>

struct pvdi_profile;

const struct pvdi_profile *pvdi_profile_for_app(const char *app);
bool pvdi_import(const struct pvdi_profile *profile, const char *graph_path,
                 const char *arm64_path, const char *output_path);

#endif
