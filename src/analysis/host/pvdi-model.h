/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_PVDI_MODEL_HOST_H
#define AZ_PVDI_MODEL_HOST_H

#include <stdbool.h>
#include <stdio.h>

struct pvdi_profile;

const struct pvdi_profile *pvdi_macos_profile_for_version(const char *version);
bool pvdi_import(const struct pvdi_profile *profile, const char *graph_path,
                 const char *arm64_path, const char *output_path);
bool pvdi_import_windows(FILE *executable, FILE *encrypted_graph, FILE *output);
bool pvdi_create_windows_model(const char *executable_path, const char *graph_path, const char *output_dir);

#endif
