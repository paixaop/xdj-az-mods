/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZCACHEQUERY_H
#define AZCACHEQUERY_H
#include "azcachebirth.h"
/* Copied evidence within the same owned MusicLibraryServer callback, following
 * its selected-cache query with unchanged source/storage/provenance. Raw tag23
 * is no TrackID or committed load. No native result/row is retained. */
struct azcq_evidence {
    struct azcb_observation cache;
    uint32_t sequence, client, selector, start, count;
};
static inline void azcq_evidence_drop(struct azcq_evidence *e) { azcb_observation_drop(&e->cache); }
static inline void azcq_evidence_copy(struct azcq_evidence *dst, const struct azcq_evidence *src) {
    azcb_observation_copy(&dst->cache, &src->cache);
    dst->sequence = src->sequence;
    dst->client = src->client;
    dst->selector = src->selector;
    dst->start = src->start;
    dst->count = src->count;
}
__attribute__((visibility("hidden"))) bool azcq_current_owned(struct azcq_evidence *);
#endif
