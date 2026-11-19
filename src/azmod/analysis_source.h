/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_ANALYSIS_SOURCE_H
#define AZ_ANALYSIS_SOURCE_H
#include "azmod-mod.h"
#include <stdint.h>
/* A core feature and a community initialization have separate lifetimes. */
enum { AZ_ANALYSIS_OWNER_CORE = 1, AZ_ANALYSIS_OWNER_MOD = 2 };
typedef struct az_analysis_owner {
    uint64_t generation;
    uint32_t slot, kind;
} az_analysis_owner;
typedef struct analysis_source_request {
    uint32_t loaded, components;
    int32_t deck;
    uint64_t load;
    az_track_ref track;
} analysis_source_request;
/* Owned until release, which runs only after all decoder children have joined.
 * The audio descriptor locator may be opened by a subprocess. An already-open
 * descriptor survives retirement; a new open after release fails. */
typedef struct analysis_source_input {
    uint64_t lease;
    az_analysis_owner owner;
    char audio_path[1024];
    az_beat *beats;
    uint32_t beat_count, needed;
    double bpm;
    az_track_ref track;
    uint64_t load_epoch;
    unsigned deck;
} analysis_source_input;
az_analysis_owner analysis_owner_begin(unsigned slot);
az_analysis_owner analysis_owner_get(unsigned slot);
az_analysis_owner analysis_owner_for_caller(unsigned slot);
void analysis_owner_thread_bind(az_analysis_owner owner);
az_analysis_owner analysis_owner_core(void);
az_analysis_owner analysis_owner_core_begin(void);
int analysis_owner_current(az_analysis_owner owner);
az_analysis_owner analysis_owner_retire(unsigned slot);
void analysis_owner_core_retire(void);
int analysis_source_acquire(az_analysis_owner owner, const analysis_source_request *request,
                            analysis_source_input *input);
int analysis_source_publish(az_analysis_owner owner, const analysis_source_input *input,
                            const az_analysis_result_v1 *result);
void analysis_source_release(analysis_source_input *input);
/* Nonblocking revocation; physical cleanup is performed by a service worker. */
void analysis_source_retire(az_analysis_owner owner);
#endif
