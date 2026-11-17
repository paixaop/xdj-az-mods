/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZANALYSIS_NATIVE_H
#define AZANALYSIS_NATIVE_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Private core service. Native epochs and successful-load generations are
 * required together. No library row or USB identity is inferred for streaming. */
struct azn_analysis_source {
    uint32_t size, version;
    uint64_t token, epoch, load;
    uint32_t source_kind;
    unsigned char track_id[12];
    char audio_path[1024];
};
typedef int (*azn_analysis_patch_fn)(uintptr_t, void *, void **, const char *);
struct azn_analysis_source_api {
    uint32_t size, version;
    int (*install)(int build, azn_analysis_patch_fn patch);
    /* Worker only. Returns AZ_*; out is unchanged on failure. */
    int (*open)(unsigned deck, uint64_t epoch, uint64_t load, struct azn_analysis_source *out);
    int (*current)(uint64_t token);
    void (*close)(uint64_t token);
};
__attribute__((visibility("hidden"))) const struct azn_analysis_source_api *azn_analysis_sources(void);
#ifdef __cplusplus
}
#endif
#endif
