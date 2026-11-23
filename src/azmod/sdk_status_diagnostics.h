/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SDK_STATUS_DIAGNOSTICS_H
#define SDK_STATUS_DIAGNOSTICS_H
#include "sdk_status_layout.h"
#include "sdk_status_proof.h"
#define DS_SUBS 32u
#define DS_BYTES (16u * 1024u * 1024u)
/* One-millisecond upper-bound buckets; the last bucket is over 250 ms. */
#define DS_LATENCY_BUCKETS 252
struct ds_metrics {
    uint64_t high_water, evictions, capture_gaps, revision_losses, callbacks, resyncs;
    uint64_t last_lost_topics;
    uint64_t latency[2][DS_LATENCY_BUCKETS];
    uint64_t dispatch[2][DS_LATENCY_BUCKETS];
};
#define DS_PROOF_CAPACITY 16384u
struct ds_proof_record {
    uint64_t id, interval, sequence, epoch, input_lost, lost, routes, pending;
    uint64_t settled, ready, unavailable, handle, topics, cursor_before;
    az_track_ref source;
    struct ds_proof_context context;
    unsigned kind, deck, file_only, flags, input_reason, reason, committed;
};
struct ds_metrics_snapshot {
    struct ds_metrics deck[4];
    size_t bytes, peak;
    uint64_t slow[DS_SUBS], longest[DS_SUBS], sequence[4];
    unsigned proof_enabled, proof_count, proof_overflow;
    size_t fixed_bytes, proof_bytes, proof_touched;
    uint64_t capture_base[4], revision_base[4], resync_base[4], sequence_base[4];
    const struct ds_proof_record *proof;
};

/* Published proof slots are append-only. Count and immutable slots describe one
 * acquired cut; the worker never observes the mutable reducer or its lock. */
unsigned ds_owner_subscriptions(int);
void ds_capture_metrics(struct ds_metrics_snapshot *);
void ds_enable_proofs(void);
void ds_start_metrics(int, size_t);
void ds_write_metrics(void);
#endif
