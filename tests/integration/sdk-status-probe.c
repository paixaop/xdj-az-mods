/* SPDX-License-Identifier: GPL-3.0-only */
/* Public, no-tick acceptance probe. Firmware controls drive the callback. */
#include "azmod-mod.h"
#include <inttypes.h>
#include <time.h>
#define ID "status-audit"
#ifndef STATUS_HZ
#define STATUS_HZ 1
#endif
static const az_core_v1 *core;
static az_deck_subscription handle;
static void cue_row(const char *kind, const az_deck_status_event_v1 *e, const az_cue_entry *row) {
    static const char hex[] = "0123456789abcdef";
    char comment[sizeof row->comment * 2 + 1];
    for (unsigned i = 0; i < sizeof row->comment; ++i) {
        unsigned c = (unsigned char)row->comment[i];
        comment[2 * i] = hex[c >> 4]; comment[2 * i + 1] = hex[c & 15];
    }
    comment[sizeof comment - 1] = 0;
    core->log(ID, "%s d=%u seq=%" PRIu64 " size=%u version=%u valid=%x classification=%u id=%u slot=%u type=%u rgb=%u in=%.9f out=%.9f saved=%d modified=%d auto=%d color_id=%u",
              kind, e->deck, e->sequence, row->size, row->version, row->valid, row->classification,
              row->entry_id, row->slot, row->type, row->rgb, row->start_s, row->end_s,
              row->saved, row->modified, row->auto_active, row->color_id);
    core->log(ID, "%s-comment d=%u seq=%" PRIu64 " bytes=%u hex=%s",
              kind, e->deck, e->sequence, (unsigned)sizeof row->comment, comment);
}
static uint64_t clock_ns(void) {
    struct timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? 0 : (uint64_t) now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}
static void changed(const az_deck_status_event_v1 *e, void *user) {
    uint64_t entered = clock_ns();
    if (!e || e->size < sizeof *e || e->version != AZ_DATA_VERSION || e->subscription != handle ||
        !e->view || e->view->size < sizeof *e->view || user != &handle) return;
    const az_deck_status_view_v1 *v = e->view;
    core->log(ID, "d=%u seq=%" PRIu64 " epoch=%" PRIu64 " kind=%u changed=%" PRIx64 " lost=%" PRIx64 " reason=%x observed=%" PRIu64 " published=%" PRIu64 " entry=%" PRIu64,
              e->deck, e->sequence, e->load_epoch, e->kind, e->changed_topics, e->lost_topics, e->loss_reason, e->observed_ns, e->published_ns, entered);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_LOAD))
        core->log(ID, "load d=%u seq=%" PRIu64 " state=%d attempt=%" PRIu64 " source=%d track=%u supported=%" PRIx64 " valid=%" PRIx64,
                  e->deck, e->sequence, v->load->state, v->load->attempt, v->load->source, v->load->track.track_id, v->load->header.supported, v->load->header.valid);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_METADATA))
        core->log(ID, "metadata d=%u seq=%" PRIu64 " id=%u title=%s artist=%s duration=%.9f valid=%" PRIx64 " duration_p=%u precision=%.9f",
                  e->deck, e->sequence, v->metadata->content_id, v->metadata->title ? v->metadata->title : "", v->metadata->artist ? v->metadata->artist : "", v->metadata->duration_s, v->metadata->header.valid, v->metadata->duration_s_info.info.provenance, v->metadata->duration_precision_s);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_LOOP)) {
        core->log(ID, "loop d=%u seq=%" PRIu64 " in=%.9f out=%.9f engaged=%d win=%.9f wout=%.9f supported=%" PRIx64 " valid=%" PRIx64 " in_rate=%u out_rate=%u win_rate=%u wout_rate=%u",
                  e->deck, e->sequence, v->loop->retained_in.seconds, v->loop->retained_out.seconds, v->loop->engaged, v->loop->working_in.seconds, v->loop->working_out.seconds, v->loop->header.supported, v->loop->header.valid,
                  v->loop->retained_in.rate_hz, v->loop->retained_out.rate_hz, v->loop->working_in.rate_hz, v->loop->working_out.rate_hz);
        const az_status_cue_ref_v1 *ref = v->loop->saved_entry;
        core->log(ID, "loop-source d=%u seq=%" PRIu64 " reloop=%d availability=%u revision=%" PRIu64 " mode=%d cue_field=%u cue_id=%u cue_revision=%" PRIu64 " cue_provider=%" PRIu64 " cue_p=%u cue_epoch=%" PRIu64 " cue_avail=%u",
                  e->deck, e->sequence, v->loop->reloop_available, v->loop->reloop_available_info.info.availability,
                  v->loop->reloop_available_info.info.revision, v->loop->mode, ref ? ref->cue_field : 0,
                  ref ? ref->entry.entry_id : 0, ref ? ref->collection_revision : 0, ref ? ref->provider_id : 0,
                  ref ? ref->provenance : 0, ref ? ref->load_epoch : 0, v->loop->saved_entry_info.info.availability);
        if (ref) {
            const az_cue_entry *rows = NULL, *target = NULL;
            const az_status_field_info_v1 *info = NULL;
            switch (ref->cue_field) {
                case AZ_STATUS_CUES_LIVE_HOT: rows = v->cues->live_hot; info = &v->cues->live_hot_info; break;
                case AZ_STATUS_CUES_LIVE_MEMORY: rows = v->cues->live_memory; info = &v->cues->live_memory_info; break;
                case AZ_STATUS_CUES_SAVED_HOT: rows = v->cues->saved_hot; info = &v->cues->saved_hot_info; break;
                case AZ_STATUS_CUES_SAVED_MEMORY: rows = v->cues->saved_memory; info = &v->cues->saved_memory_info; break;
            }
            unsigned matches = 0;
            int hot = ref->cue_field == AZ_STATUS_CUES_LIVE_HOT || ref->cue_field == AZ_STATUS_CUES_SAVED_HOT;
            for (unsigned i = 0; info && rows && i < info->count; ++i)
                if (hot ? rows[i].slot == ref->entry.slot : rows[i].entry_id == ref->entry.entry_id) {
                    if (!target) target = &rows[i];
                    ++matches;
                }
            core->log(ID, "loop-target d=%u seq=%" PRIu64 " field=%u availability=%u scope=%u epoch=%" PRIu64 " provider=%" PRIu64 " revision=%" PRIu64 " provenance=%u count=%u matches=%u",
                      e->deck, e->sequence, ref->cue_field, info ? info->info.availability : 0, info ? info->scope : 0,
                      info ? info->scope_id : 0, info ? info->provider_id : 0, info ? info->info.revision : 0,
                      info ? info->info.provenance : 0, info ? info->count : 0, matches);
            cue_row("loop-reference-row", e, &ref->entry);
            if (target) cue_row("loop-target-row", e, target);
        }
    }
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_POSITION))
        core->log(ID, "position d=%u seq=%" PRIu64 " head=%" PRId64 "/%u remaining=%.3f estimate=%.3f valid=%" PRIx64 " slip=%" PRId64 "/%u slip_avail=%u",
                  e->deck, e->sequence, v->position->head.samples, v->position->head.rate_hz, v->position->remaining_s,
                  v->position->estimated_s, v->position->header.valid, v->position->slip_head.samples, v->position->slip_head.rate_hz,
                  v->position->slip_head_info.info.availability);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_TRANSPORT))
        core->log(ID, "transport d=%u seq=%" PRIu64 " playback=%d raw=%d/%d/%d rate=%.3f valid=%" PRIx64 " direction=%d",
                  e->deck, e->sequence, v->transport->state, v->transport->raw_mode_is_2, v->transport->raw_play_pause, v->transport->raw_aux, v->transport->rate, v->transport->header.valid, v->transport->direction);
    if (e->occurrence) core->log(ID, "occurrence d=%u seq=%" PRIu64 " kind=%u ordinal=%" PRIu64 " endpoints=%u from=%.6f to=%.6f",
                                 e->deck, e->sequence, e->occurrence->kind, e->occurrence->ordinal, e->occurrence->endpoints, e->occurrence->from.seconds, e->occurrence->to.seconds);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_TEMPO))
        core->log(ID, "tempo d=%u seq=%" PRIu64 " current=%.6f original=%.6f valid=%" PRIx64 " availability=%u scope=%u scope_id=%" PRIu64 " slider=%.6f lock=%d original_p=%u original_rev=%" PRIu64,
                  e->deck, e->sequence, v->tempo->current_bpm, v->tempo->original_bpm, v->tempo->header.valid, v->tempo->current_bpm_info.info.availability, v->tempo->current_bpm_info.scope, v->tempo->current_bpm_info.scope_id, v->tempo->slider_percent, v->tempo->slider_locked,
                  v->tempo->original_bpm_info.info.provenance, v->tempo->original_bpm_info.info.revision);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_SYNC))
        core->log(ID, "sync d=%u seq=%" PRIu64 " enabled=%d master=%d mode=%d valid=%" PRIx64 " locked=%d following=%d",
                  e->deck, e->sequence, v->sync->enabled, v->sync->master, v->sync->raw_mode, v->sync->header.valid, v->sync->locked, v->sync->following);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_PHASE))
        core->log(ID, "phase d=%u seq=%" PRIu64 " beat=%.6f bar=%.6f valid=%" PRIx64,
                  e->deck, e->sequence, v->phase->beat, v->phase->bar, v->phase->header.valid);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_CUES))
        core->log(ID, "cues d=%u seq=%" PRIu64 " main=%u live_hot=%u live_memory=%u saved_hot=%u saved_memory=%u valid=%" PRIx64,
                  e->deck, e->sequence, v->cues->main_info.count, v->cues->live_hot_info.count, v->cues->live_memory_info.count, v->cues->saved_hot_info.count, v->cues->saved_memory_info.count, v->cues->header.valid);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_CUES)) {
        const az_status_field_info_v1 *m = &v->cues->live_memory_info;
        core->log(ID, "memory d=%u seq=%" PRIu64 " availability=%u scope=%" PRIu64 " provider=%" PRIu64 " revision=%" PRIu64 " count=%u",
                  e->deck, e->sequence, m->info.availability, m->scope_id, m->provider_id, m->info.revision, m->count);
        for (uint32_t i = 0; v->cues->live_memory && i < m->count && i < 2; i++) {
            const az_cue_entry *row = &v->cues->live_memory[i];
            core->log(ID, "memory-row d=%u seq=%" PRIu64 " index=%u id=%u slot=%u type=%u valid=%x in=%.6f out=%.6f comment=%.127s",
                      e->deck, e->sequence, i, row->entry_id, row->slot, row->type, row->valid, row->start_s, row->end_s, row->comment);
        }
    }
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_SETTINGS))
        core->log(ID, "settings d=%u seq=%" PRIu64 " mt=%d keylock=%d quantize=%d slip=%d pad=%d valid=%" PRIx64,
                  e->deck, e->sequence, v->settings->master_tempo, v->settings->key_lock, v->settings->quantize, v->settings->slip, v->settings->pad_mode, v->settings->header.valid);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_LOAD))
        core->log(ID, "track d=%u seq=%" PRIu64 " source=%" PRIu64 " incarnation=%" PRIu64 " library=%" PRIu64 " id=%u availability=%u",
                  e->deck, e->sequence, v->load->track.source_id, v->load->track.source_generation, v->load->track.library_id, v->load->track.track_id, v->load->track_info.info.availability);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_GRID)) {
        core->log(ID, "grid d=%u seq=%" PRIu64 " current=%u saved=%u valid=%" PRIx64,
                  e->deck, e->sequence, v->grid->current_info.count, v->grid->saved_info.count, v->grid->header.valid);
        const az_beat *rows[] = {v->grid->current, v->grid->saved};
        const az_status_field_info_v1 *info[] = {&v->grid->current_info, &v->grid->saved_info};
        for (unsigned i = 0; i < 2; i++) {
            if (!rows[i] || info[i]->info.availability != AZ_AVAIL_READY || !info[i]->count) continue;
            core->log(ID, "grid-limits d=%u seq=%" PRIu64 " field=%u provider=%" PRIu64 " revision=%" PRIu64 " first=%.9f last=%.9f", e->deck, e->sequence, i, info[i]->provider_id, info[i]->info.revision,
                      rows[i][0].time_s, rows[i][info[i]->count - 1].time_s);
        }
    }
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_PHRASES))
        core->log(ID, "phrases d=%u seq=%" PRIu64 " current=%u saved=%u valid=%" PRIx64,
                  e->deck, e->sequence, v->phrases->current_info.count, v->phrases->saved_info.count, v->phrases->header.valid);
    if (e->changed_topics & (AZ_STATUS_TOPIC(AZ_STATUS_GRID) | AZ_STATUS_TOPIC(AZ_STATUS_PHRASES))) {
        const az_status_grid_ref_v1 *refs[] = {&v->phrases->current_grid, &v->phrases->saved_grid};
        const az_status_field_info_v1 *info[] = {&v->phrases->current_grid_info, &v->phrases->saved_grid_info};
        for (unsigned i = 0; i < 2; i++) {
            const az_status_grid_ref_v1 *ref = refs[i];
            const az_status_field_info_v1 *grid = ref->grid_field == AZ_STATUS_GRID_CURRENT ? &v->grid->current_info : &v->grid->saved_info;
            core->log(ID, "phrase-%s-map d=%u seq=%" PRIu64 " availability=%u field=%u epoch=%" PRIu64 " provider=%" PRIu64 " revision=%" PRIu64 " provenance=%u grid_avail=%u grid_epoch=%" PRIu64 " grid_provider=%" PRIu64 " grid_revision=%" PRIu64 " grid_p=%u",
                      i ? "saved" : "current", e->deck, e->sequence, info[i]->info.availability, ref->grid_field,
                      ref->load_epoch, ref->provider_id, ref->revision, ref->provenance, grid->info.availability,
                      grid->scope_id, grid->provider_id, grid->info.revision, grid->info.provenance);
        }
    }
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_OVERVIEW))
        core->log(ID, "overview d=%u seq=%" PRIu64 " count=%u valid=%" PRIx64,
                  e->deck, e->sequence, v->overview->saved ? v->overview->saved->sample_count : 0, v->overview->header.valid);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_STEMS))
        core->log(ID, "stems d=%u seq=%" PRIu64 " available=%d enabled=%d valid=%" PRIx64,
                  e->deck, e->sequence, v->stems->available, v->stems->enabled, v->stems->header.valid);
    if (e->changed_topics & AZ_STATUS_TOPIC(AZ_STATUS_KEY))
        core->log(ID, "key d=%u seq=%" PRIu64 " original=%s shift=%d effective=%s valid=%" PRIx64 " original_p=%u original_rev=%" PRIu64 " effective_p=%u",
                  e->deck, e->sequence, v->key->original ? v->key->original : "", v->key->shift_semitones, v->key->effective ? v->key->effective : "", v->key->header.valid, v->key->original_info.info.provenance, v->key->original_info.info.revision, v->key->effective_info.info.provenance);
    if (e->phrase_transition) core->log(ID, "phrase d=%u seq=%" PRIu64 " kind=%u from=%u/%u to=%u/%u",
                                        e->deck, e->sequence, e->phrase_transition->discontinuity, e->phrase_transition->from.kind, e->phrase_transition->from.index, e->phrase_transition->to.kind, e->phrase_transition->to.index);
    core->log(ID, "end d=%u seq=%" PRIu64 " occurrence=%u phrase=%u", e->deck, e->sequence,
              e->occurrence != NULL, e->phrase_transition != NULL);
}
static int init(const az_core_v1 *c) {
    if (!AZ_HAS(c, library_service) || !AZ_HAS(c, log)) return -1;
    const az_library_v1 *lib = c->library_service(AZ_LIBRARY_ABI);
    if (!AZ_LIB_HAS(lib, on_deck_status) || !AZ_LIB_HAS(lib, deck_status_release)) return -1;
    core = c;
    const az_deck_watch_v1 watch = {sizeof watch, AZ_DATA_VERSION, AZ_STATUS_ALL_DECKS, STATUS_HZ, AZ_STATUS_ALL_V1};
    return lib->on_deck_status(ID, &watch, changed, &handle, &handle) == AZ_OK ? 0 : -1;
}
static const char *const tags[] = {"any", NULL};
static const az_mod_v1 mod = {.abi = AZ_MOD_ABI, .size = sizeof mod, .id = ID, .fw_tags = tags, .init = init};
AZ_MOD_EXPORT const az_mod_v1 *az_mod_v1_entry(void) { return &mod; }
