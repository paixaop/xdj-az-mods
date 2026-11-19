/* SPDX-License-Identifier: GPL-3.0-only */
/* Observes completed AnalyzedDataAccessor cue and grid writes. The native FileIdentityInfo
 * contains separate UTF-16 audio and ANLZ paths. Its accessor's storage owner
 * provides the UTF-8 root used by the native FilesetInfo constructor. */
struct sw_ticket {
    unsigned deck;
    uint64_t epoch, cut, started;
    az_track_ref ref;
    struct ss_file file[2];
};
struct sw_batch {
    struct sw_ticket item[4];
    unsigned count, lane;
};
static uint32_t sw_codepoint(const uint16_t *units, size_t count, size_t *index) {
    uint32_t codepoint = units[*index];
    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
        if (++*index >= count || units[*index] < 0xdc00 || units[*index] > 0xdfff) return 0;
        return 0x10000 + ((codepoint - 0xd800) << 10) + (units[*index] - 0xdc00);
    }
    return codepoint >= 0xdc00 && codepoint <= 0xdfff ? 0 : codepoint;
}
static unsigned sw_utf8(uint32_t codepoint, unsigned char bytes[4]) {
    unsigned count = 0;
    if (codepoint <= 0x7f) bytes[count++] = (unsigned char) codepoint;
    else {
        if (codepoint < 0x800) bytes[count++] = 0xc0 | (codepoint >> 6);
        else {
            if (codepoint < 0x10000) bytes[count++] = 0xe0 | (codepoint >> 12);
            else {
                bytes[count++] = 0xf0 | (codepoint >> 18);
                bytes[count++] = 0x80 | ((codepoint >> 12) & 63);
            }
            bytes[count++] = 0x80 | ((codepoint >> 6) & 63);
        }
        bytes[count++] = 0x80 | (codepoint & 63);
    }
    return count;
}
static int sw_name(const uint16_t *units, size_t count, const char *text) {
    if ((!units && count) || count > 1023 || !text) return 0;
    const unsigned char *cursor = (const unsigned char *) text;
    for (size_t i = 0; i < count; i++) {
        uint32_t codepoint = sw_codepoint(units, count, &i);
        if (!codepoint) return 0;
        unsigned char bytes[4];
        unsigned length = sw_utf8(codepoint, bytes);
        for (unsigned j = 0; j < length; j++)
            if (*cursor++ != bytes[j]) return 0;
    }
    return !*cursor;
}
static int sw_binding_ready(const struct ss_binding *binding) {
    return binding->ready && !binding->retired && binding->epoch &&
           (binding->topics & (SS_DAT | SS_EXT)) && !binding->image.rc;
}
struct sw_locator {
    const char *root;
    size_t root_length;
    const uint16_t *audio, *analysis;
    size_t audio_length, analysis_length;
};
static int sw_binding_paths(const struct ss_binding *binding, const struct sw_locator *locator) {
    const char *mount = binding->image.src.mount;
    size_t length = strlen(mount);
    return locator->root_length == length + 8 && !memcmp(locator->root, mount, length) &&
           !memcmp(locator->root + length, "/PIONEER", 8) && (binding->image.metadata.valid & AZ_META_PATH) &&
           !strncmp(binding->image.metadata.path, mount, length) &&
           sw_name(locator->audio, locator->audio_length, binding->image.metadata.path + length) &&
           sw_name(locator->analysis, locator->analysis_length, binding->image.file[0].relative);
}
static void sw_ticket_start(struct sw_batch *batch, unsigned deck, struct ss_binding *binding) {
    struct sw_ticket *ticket = &batch->item[batch->count++];
    ticket->deck = deck;
    ticket->epoch = binding->epoch;
    ticket->cut = binding->cut;
    ticket->ref = binding->ref;
    ticket->started = ss_new_id(&ss_serial);
    if (binding->writing) binding->overlapping = 1;
    ++binding->writing;
    binding->serial = ticket->started;
    memcpy(ticket->file, binding->image.file, sizeof ticket->file);
}
static void sw_start(struct sw_batch *batch, const char *root, size_t root_n,
                     const uint16_t *audio, size_t audio_n, const uint16_t *analysis, size_t analysis_n) {
    if (!root || root_n > 1023 || !audio_n || !analysis_n) return;
    struct sw_locator locator = {root, root_n, audio, analysis, audio_n, analysis_n};
    pthread_mutex_lock(&ss_mu);
    for (unsigned deck = 0; deck < 4; deck++) {
        struct ss_binding *binding = &ss_decks[deck];
        if (sw_binding_ready(binding) && sw_binding_paths(binding, &locator)) sw_ticket_start(batch, deck, binding);
    }
    pthread_mutex_unlock(&ss_mu);
}
static void sw_release(struct sw_batch *batch) {
    if (!batch->count) return;
    pthread_mutex_lock(&ss_mu);
    for (unsigned i = 0; i < batch->count; i++) {
        const struct sw_ticket *t = &batch->item[i];
        struct ss_binding *b = &ss_decks[t->deck];
        if (b->cut == t->cut && b->epoch == t->epoch && b->writing && !--b->writing) {
            b->overlapping = 0;
            azfile_native_refresh(t->deck, t->epoch);
        }
    }
    pthread_mutex_unlock(&ss_mu);
    if (batch->count) {
        ss_signal();
        st_worker_wake();
    }
    batch->count = 0;
}
/* One native save may concern the same track on several units. Share the
 * immutable containers and storage revision, but keep each unit's individual
 * epoch, subscriber interval, native witness and provider incarnation. */
struct sw_observation {
    const struct sw_batch *batch;
    const struct sw_ticket *first;
    unsigned index, members, lane, nfiles;
    uint64_t topics, observed, revision;
    struct st_source source;
    struct ss_saved_revision *saved;
    struct ss_write_proof proof[2];
    char siblings[2][65];
};
static unsigned sw_group_members(const struct sw_batch *batch, unsigned index) {
    const struct sw_ticket *first = &batch->item[index];
    unsigned members = 0;
    for (unsigned i = index; i < batch->count; i++)
        if (st_ref_equal(&first->ref, &batch->item[i].ref) &&
            !memcmp(first->file, batch->item[i].file, sizeof first->file)) members |= 1u << i;
    return members;
}
static int sw_saved_file_path(const struct ss_file *file, const struct st_source *source, struct stat *status) {
    char path[1024];
    return file->path[0] && st_source_path(source->mount, file->relative, path) && !strcmp(path, file->path) &&
           !stat(path, status) && S_ISREG(status->st_mode) &&
           !((uint64_t) status->st_dev == source->device && (uint64_t) status->st_ino == source->inode);
}
static void sw_read_saved_file(struct sw_observation *observation, unsigned index) {
    struct ss_file file = observation->first->file[index];
    struct ss_saved_revision *saved = observation->saved;
    struct ss_write_proof *proof = &observation->proof[index];
    struct stat status;
    if (saved) saved->rc[index] = file.rc;
    if (!sw_saved_file_path(&file, &observation->source, &status)) return;
    file.signature = st_stat_sig(&status);
    file.device = (uint64_t) status.st_dev;
    file.inode = (uint64_t) status.st_ino;
    if (!ss_read_digest(&file, proof->digest, saved ? &saved->bytes[index] : NULL, saved ? &saved->sizes[index] : NULL)) return;
    proof->device = file.device;
    proof->inode = file.inode;
    proof->signature = file.signature;
    if (saved && saved->bytes[index]) {
        saved->rc[index] = AZ_OK;
        saved->total += saved->sizes[index];
        ss_sibling_digest(observation->lane, saved->bytes[index], saved->sizes[index], observation->siblings[index]);
    }
}
static void sw_revision_start(struct sw_observation *observation) {
    observation->revision = ss_new_id(&ss_serial);
    struct ss_saved_revision *saved = observation->saved;
    if (!saved) return;
    saved->ref = observation->first->ref;
    saved->revision = observation->revision;
    saved->observed = observation->observed;
    saved->lane = observation->lane;
}
static int sw_target_current(const struct ss_binding *binding, const struct sw_ticket *ticket) {
    return binding->cut == ticket->cut && binding->epoch == ticket->epoch && !binding->retired &&
           st_ref_equal(&binding->ref, &ticket->ref);
}
static uint64_t sw_single_lane_proof(const struct sw_observation *observation, const struct ss_binding *binding,
                                     unsigned index, int same_file) {
    return !binding->overlapping && same_file && observation->siblings[index][0] &&
                   !memcmp(observation->siblings[index], binding->siblings[observation->lane][index], sizeof observation->siblings[index])
               ? observation->topics
               : 0;
}
static int sw_target_file_proof(const struct sw_observation *observation, struct ss_binding *binding, unsigned index) {
    const struct ss_write_proof *proof = &observation->proof[index];
    const struct ss_file *file = &observation->first->file[index];
    if (!proof->signature) return file->rc == AZ_UNSUPPORTED;
    int same_file = proof->device == file->device && proof->inode == file->inode;
    int complete = observation->saved && observation->saved->bytes[index] && same_file;
    if (binding->writing > 1) return complete;
    struct ss_write_proof done = *proof;
    done.revision = observation->revision;
    done.ordinal = binding->completed[index].ordinal + 1;
    done.only = sw_single_lane_proof(observation, binding, index, same_file);
    binding->completed[index] = done;
    return complete;
}
static int sw_target_complete(const struct sw_observation *observation, struct ss_binding *binding) {
    int complete = observation->saved && observation->saved->total && !binding->overlapping && observation->revision;
    for (unsigned index = 0; index < observation->nfiles; index++) {
        int file_complete = sw_target_file_proof(observation, binding, index);
        if (!file_complete) complete = 0;
    }
    return complete;
}
static void sw_pin_target(const struct sw_observation *observation, unsigned deck, struct ss_binding *binding) {
    struct ss_saved_revision *saved = observation->saved;
    unsigned lane = observation->lane;
    saved->targets |= 1u << deck;
    saved->target[deck].cut = binding->cut;
    saved->target[deck].epoch = binding->epoch;
    saved->target[deck].interest = binding->interest[lane];
    saved->target[deck].provider = binding->provider[ss_saved_provider[lane]];
    saved->target[deck].source_pin = ss_pins[deck].epoch == binding->epoch ? ss_pins[deck].token : 0;
    ++binding->pinned[lane];
    binding->pending &= ~observation->topics;
}
static void sw_queue_target(const struct sw_observation *observation, unsigned deck, struct ss_binding *binding, int complete) {
    if (!(binding->topics & observation->topics)) return;
    if (complete && ss_saved_count < SS_SAVE_QUEUE && observation->saved->total <= SS_SAVE_PIN_BYTES - ss_saved_bytes)
        sw_pin_target(observation, deck, binding);
    else {
        binding->interest[observation->lane] = ss_new_id(&ss_serial);
        binding->pinned[observation->lane] = 0;
        binding->pending |= observation->topics;
        ds_publish_file(deck, binding->epoch, &binding->ref, observation->observed, NULL, 0, observation->topics, AZ_STATUS_REVISION_LOST);
    }
}
static void sw_publish_group(struct sw_observation *observation) {
    pthread_mutex_lock(&ss_mu);
    sw_revision_start(observation);
    for (unsigned i = observation->index; i < observation->batch->count; i++) {
        if (!(observation->members & (1u << i))) continue;
        const struct sw_ticket *ticket = &observation->batch->item[i];
        struct ss_binding *binding = &ss_decks[ticket->deck];
        if (!sw_target_current(binding, ticket)) continue;
        int complete = sw_target_complete(observation, binding);
        sw_queue_target(observation, ticket->deck, binding, complete);
    }
    if (observation->saved && observation->saved->targets) {
        ss_saved_queue[ss_saved_count++] = observation->saved;
        ss_saved_bytes += observation->saved->total;
        observation->saved = NULL;
    }
    pthread_mutex_unlock(&ss_mu);
}
static void sw_finish_group(struct sw_observation *observation) {
    if (!observation->first->started || ss_source(&observation->first->ref, &observation->source)) return;
    observation->saved = calloc(1, sizeof *observation->saved);
    for (unsigned i = 0; i < observation->nfiles; i++) sw_read_saved_file(observation, i);
    if (!ss_source(&observation->first->ref, &observation->source)) sw_publish_group(observation);
    ss_release_saved(observation->saved);
}
static void sw_finish(struct sw_batch *batch) {
    unsigned handled = 0;
    uint64_t observed = st_now();
    for (unsigned i = 0; i < batch->count; i++) {
        if (handled & (1u << i)) continue;
        struct sw_observation observation = {.batch = batch, .first = &batch->item[i], .index = i, .members = sw_group_members(batch, i), .lane = batch->lane, .nfiles = batch->lane == SS_SAVED_GRID ? 1u : 2u, .topics = AZ_STATUS_TOPIC(ss_saved_topic[batch->lane]), .observed = observed};
        handled |= observation.members;
        sw_finish_group(&observation);
    }
    sw_release(batch);
}
struct sw_target {
    uintptr_t address;
    uint32_t bytes[4];
};
static const struct sw_target sw_targets[2][4] = {
    {{0x180ddd8, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x180e7a0, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x180eb20, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x180b2a8, {0xa9bb7bfd, 0xaa0003e7, 0xaa0103e8, 0x12003c49}}},
    {{0x1969148, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x1969b10, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x1969e90, {0xa9ba7bfd, 0xaa0003e7, 0xaa0103e8, 0x910003fd}}, {0x1966618, {0xa9bb7bfd, 0xaa0003e7, 0xaa0103e8, 0x12003c49}}}};
static _Atomic int sw_ready;
static void *sw_original[4];
static void sw_capture(struct sw_batch *b, uintptr_t accessor, uintptr_t identity) {
    if (!atomic_load_explicit(&sw_ready, memory_order_acquire) || !accessor || !identity) return;
    uintptr_t storage = sm_ptr(accessor, 0x18);
    if (storage)
        sw_start(b, (const char *) sm_ptr(storage, 8), sm_ptr(storage, 16),
                 (const uint16_t *) sm_ptr(identity, 0), sm_ptr(identity, 8),
                 (const uint16_t *) sm_ptr(identity, 32), sm_ptr(identity, 40));
}
static uint64_t sw_old(uintptr_t self, uintptr_t identity, uintptr_t a, uintptr_t b) {
    struct sw_batch scope __attribute__((cleanup(sw_release))) = {0};
    sw_capture(&scope, self, identity);
    uint64_t rc = ((uint64_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)) sw_original[0])(self, identity, a, b);
    if ((uint32_t) rc == 0) sw_finish(&scope);
    return rc;
}
static uint64_t sw_cue(unsigned index, uintptr_t self, uintptr_t identity, uintptr_t data) {
    struct sw_batch scope __attribute__((cleanup(sw_release))) = {0};
    sw_capture(&scope, self, identity);
    uint64_t rc = ((uint64_t (*)(uintptr_t, uintptr_t, uintptr_t)) sw_original[index])(self, identity, data);
    if ((uint32_t) rc == 0) sw_finish(&scope);
    return rc;
}
static uint64_t sw_new(uintptr_t self, uintptr_t identity, uintptr_t data) { return sw_cue(1, self, identity, data); }
static uint64_t sw_delete(uintptr_t self, uintptr_t identity, uintptr_t data) { return sw_cue(2, self, identity, data); }
static uint64_t sw_offset(uintptr_t self, uintptr_t identity, unsigned offset) {
    struct sw_batch scope __attribute__((cleanup(sw_release))) = {.lane = SS_SAVED_GRID};
    sw_capture(&scope, self, identity);
    uint64_t rc = ((uint64_t (*)(uintptr_t, uintptr_t, unsigned)) sw_original[3])(self, identity, offset);
    if ((uint32_t) rc == 0) sw_finish(&scope);
    return rc;
}
static int sdk_status_save_install(int build, azjs_installer install) {
    void *hooks[] = {sw_old, sw_new, sw_delete, sw_offset};
    if ((build != 1 && build != 2) || !install) return 0;
    const struct sw_target *t = sw_targets[build - 1];
    for (unsigned i = 0; i < 4; i++)
        if (memcmp((void *) t[i].address, t[i].bytes, sizeof t[i].bytes)) return 0;
    for (unsigned i = 0; i < 4; i++)
        if (!install(t[i].address, t[i].address, hooks[i], &sw_original[i], 0) || !sw_original[i]) return 0;
    atomic_store_explicit(&sw_ready, 1, memory_order_release);
    return 1;
}
