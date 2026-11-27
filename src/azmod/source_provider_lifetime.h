/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZMOD_SOURCE_PROVIDER_LIFETIME_H
#define AZMOD_SOURCE_PROVIDER_LIFETIME_H

#include "source_provider_dispatch.h"

#pragma GCC visibility push(hidden)

typedef int (*azspd_code_patch)(uintptr_t entry, void *replacement, void **original);

/* Native source-path observation on the exact qualified firmware profile. */
int azspd_paths_install(int firmware, azspd_code_patch code, azspd_patch slot);

struct azspd_path_state {
    uint32_t opened, owners, preview_opened;
    bool uncertain;
};

/* Storage worker. Register the exact absolute path before any native request
 * can see it. The pathname stays available until all native consumers settle. */
int azspd_path_watch(const char *path, uint64_t *ticket);
int azspd_path_status(uint64_t ticket, struct azspd_path_state *out);

/* Storage worker. The caller has revoked catalog access and independently
 * settled pending requests and analysis. Busy leaves the watch intact. */
int azspd_path_unwatch(uint64_t ticket);

/* Local USB and private carrier Folder analysis. Arm the exact raw identity
 * and requester before submission. AZ_OK from status means the native worker
 * removed their controller. Check the files separately for success. */
int azspd_analysis_install(int firmware, azspd_code_patch code);
int azspd_analysis_watch(const unsigned char raw_id[12], const unsigned char requester[12],
                         uint64_t *ticket);
int azspd_analysis_status(uint64_t ticket);
void azspd_analysis_forget(uint64_t ticket);

#pragma GCC visibility pop
#endif
