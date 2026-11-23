/* SPDX-License-Identifier: GPL-3.0-only */
/* Optional diagnostic file, atomically replaced off the overlay/native threads.
 * The counters exist independently of the diagnostic writer. */
#include "sdk_status_engine.h"
#include "sdk_status_diagnostics.h"
#ifndef az_thread_create
#include "../common/azaltstack.h"
#endif
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
static size_t ds_metrics_native_bytes;
#ifndef DS_METRICS_PATH
#define DS_METRICS_PATH "/tmp/azmod-deck-status.stats"
#endif
static void ds_write_proof(FILE *file, const struct ds_proof_record *r) {
    const struct ds_proof_context *p = &r->context;
    fprintf(file, "proof=%" PRIu64 " kind=%u deck=%u epoch=%" PRIu64 " sequence=%" PRIu64 " interval=%" PRIu64 " file_only=%u cause=0x%x flags=0x%x committed=%u"
                  " input_lost=0x%" PRIx64 " input_reason=0x%x lost=0x%" PRIx64 " reason=0x%x routes=0x%" PRIx64 " source_id=%" PRIu64 " source_generation=%" PRIu64 " library_id=%" PRIu64 " track_id=%u"
                  " binding_cut=%" PRIu64 " binding_serial=%" PRIu64 " connection=%" PRIu64 " db_before=%" PRIu64 " db_after=%" PRIu64 " device=%" PRIu64 " inode=%" PRIu64 " mount_id=%" PRIu64 " pending=0x%" PRIx64 " settled=0x%" PRIx64 " ready=0x%" PRIx64 " unavailable=0x%" PRIx64 " handle=%" PRIu64 " topics=0x%" PRIx64 " cursor_before=%" PRIu64,
            r->id, r->kind, r->deck, r->epoch, r->sequence, r->interval, r->file_only, p->cause, r->flags, r->committed,
            r->input_lost, r->input_reason, r->lost, r->reason, r->routes,
            r->source.source_id, r->source.source_generation, r->source.library_id, r->source.track_id,
            p->binding_cut, p->binding_serial, p->connection, p->db_before, p->db_after, p->device, p->inode, p->mount_id,
            r->pending, r->settled, r->ready, r->unavailable, r->handle, r->topics, r->cursor_before);
    for (unsigned f = 0; f < 2; f++)
        fprintf(file, " file%u_device=%" PRIu64 " file%u_inode=%" PRIu64 " file%u_signature=%" PRIu64,
                f, p->file_device[f], f, p->file_inode[f], f, p->file_signature[f]);
    fputs(" providers=", file);
    for (unsigned i = 0; i < 4; i++) fprintf(file, "%s%" PRIu64, i ? "," : "", p->providers[i]);
    fputs(" revisions=", file);
    for (unsigned i = 0; i < AZ_STATUS_TOPIC_COUNT; i++) fprintf(file, "%s%" PRIu64, i ? "," : "", p->revisions[i]);
    fputc('\n', file);
}
static int ds_percentile(const uint64_t *hist, uint64_t count, unsigned pct) {
    if (!count) return 0;
    uint64_t rank = count / 100 * pct + (count % 100 * pct + 99) / 100;
    for (unsigned i = 0; i < DS_LATENCY_BUCKETS; i++) {
        if (rank <= hist[i]) return i == DS_LATENCY_BUCKETS - 1 ? -1 : (int) i;
        rank -= hist[i];
    }
    return -1;
}
static FILE *ds_metrics_file(char *temporary) {
    int descriptor = mkstemp(temporary);
    if (descriptor < 0) return NULL;
    FILE *file = fdopen(descriptor, "w");
    if (!file) {
        close(descriptor);
        unlink(temporary);
    }
    return file;
}
static void ds_write_metrics_header(FILE *file, const struct ds_metrics_snapshot *snapshot) {
    size_t fixed = snapshot->fixed_bytes + ds_metrics_native_bytes;
    fprintf(file, "monotonic_ns=%" PRIu64 " revision_bytes=%zu revision_high_bytes=%zu revision_budget_bytes=%u fixed_reducer_bytes=%zu\n",
            ds_now(), snapshot->bytes, snapshot->peak, DS_BYTES, fixed);
    fprintf(file, "provenance_version=1 provenance_enabled=%u provenance_count=%u provenance_capacity=%u provenance_overflow=%u provenance_bytes=%zu provenance_touched_bytes=%zu\n",
            snapshot->proof_enabled, snapshot->proof_count, DS_PROOF_CAPACITY, snapshot->proof_overflow, snapshot->proof_bytes, snapshot->proof_touched);
    for (unsigned d = 0; d < 4; d++)
        fprintf(file, "provenance_base_deck=%u capture_gap_cuts=%" PRIu64 " revision_loss_cuts=%" PRIu64 " resyncs=%" PRIu64 " sequence=%" PRIu64 "\n",
                d, snapshot->capture_base[d], snapshot->revision_base[d], snapshot->resync_base[d], snapshot->sequence_base[d]);
    /* Published slots are immutable. The acquired count bounds this snapshot;
     * later appends touch distinct slots and require no long I/O mutex hold. */
    for (unsigned i = 0; i < snapshot->proof_count; i++) ds_write_proof(file, &snapshot->proof[i]);
}
static void ds_write_latency(FILE *file, unsigned d, const struct ds_metrics *m) {
    for (unsigned kind = 0; kind < 2; kind++) {
        const uint64_t *hist = m->latency[kind], *dispatch = m->dispatch[kind];
        uint64_t count = 0, dispatched = 0;
        for (unsigned i = 0; i < DS_LATENCY_BUCKETS; i++) {
            count += hist[i];
            dispatched += dispatch[i];
        }
        fprintf(file, "deck=%u class=%s count=%" PRIu64 " p95_upper_ms=%d p99_upper_ms=%d over_250ms=%" PRIu64 " dispatch_p95_upper_ms=%d dispatch_p99_upper_ms=%d\n",
                d, kind ? "motion" : "discrete", count, ds_percentile(hist, count, 95), ds_percentile(hist, count, 99),
                hist[DS_LATENCY_BUCKETS - 1], ds_percentile(dispatch, dispatched, 95), ds_percentile(dispatch, dispatched, 99));
    }
}
static void ds_write_metrics_deck(FILE *file, unsigned d, const struct ds_metrics *m, uint64_t sequence) {
    fprintf(file, "deck=%u sequence=%" PRIu64 " discrete_high=%" PRIu64 " evictions=%" PRIu64 " capture_gap_cuts=%" PRIu64 " revision_loss_cuts=%" PRIu64 " callbacks=%" PRIu64 " resyncs=%" PRIu64 " last_lost_topics=0x%" PRIx64 "\n",
            d, sequence, m->high_water, m->evictions, m->capture_gaps, m->revision_losses, m->callbacks,
            m->resyncs, m->last_lost_topics);
    ds_write_latency(file, d, m);
}
void ds_write_metrics(void) {
    struct ds_metrics_snapshot snapshot;
    ds_capture_metrics(&snapshot);
    char temporary[] = DS_METRICS_PATH ".XXXXXX";
    FILE *file = ds_metrics_file(temporary);
    if (!file) return;
    ds_write_metrics_header(file, &snapshot);
    for (unsigned d = 0; d < 4; d++) ds_write_metrics_deck(file, d, &snapshot.deck[d], snapshot.sequence[d]);
    for (unsigned i = 0; i < DS_SUBS; i++)
        if (snapshot.longest[i])
            fprintf(file, "owner=%u callbacks_over_33ms=%" PRIu64 " longest_callback_ns=%" PRIu64 "\n",
                    i, snapshot.slow[i], snapshot.longest[i]);
    int ok = !ferror(file);
    if (fclose(file) || !ok || rename(temporary, DS_METRICS_PATH)) unlink(temporary);
}
static void *ds_metrics_worker(void *unused) {
    (void) unused;
    for (;;) {
        ds_write_metrics();
        struct timespec delay = {1, 0};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
    }
    return NULL;
}
void ds_start_metrics(int enabled, size_t native_bytes) {
    if (!enabled) return;
    ds_metrics_native_bytes = native_bytes;
    ds_enable_proofs();
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_OTHER);
    struct sched_param priority = {0};
    pthread_attr_setschedparam(&attr, &priority);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    (void) az_thread_create(&thread, &attr, ds_metrics_worker, NULL);
    pthread_attr_destroy(&attr);
}
