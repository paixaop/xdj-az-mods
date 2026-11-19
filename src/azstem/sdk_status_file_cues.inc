/* SPDX-License-Identifier: GPL-3.0-only */
/* The local PersistentStorageServer combines PCPT and PCP2 atoms before
 * serializing its reply. The stock ticket filters, deduplicates and sorts its
 * rows. Capture both sides of that transformation under the original call. */
#include "../common/azstatus_cue_origin.h"
#include "../common/azsha256.h"
struct FileCueProfile {
    uintptr_t methods[13], atoms[2], getter_return, merge_return, decoder_return, append_return, finish_return, hot_return, hot_content, input_table, path_table;
};
static const FileCueProfile file_profiles[2] = {
    {{0x1794d60, 0x181ce08, 0x180a138, 0x18051e0, 0x1798a20, 0x178e390, 0x109c5b0, 0x181ab60, 0x1794c58, 0x109c720, 0x1814f50, 0x17cf4c0, 0x17cf218},
     {0x2938198, 0x2938220},
     0x181d058,
     0x1809a84,
     0x1798fe4,
     0x179901c,
     0x1798b38,
     0x1799208,
     0x282e808,
     0x293cf98,
     0x2938e08},
    {{0x18f0180, 0x1978020, 0x19645f8, 0x19605b0, 0x18f3e40, 0x18e9920, 0x112c5d0, 0x1975d78, 0x18f0078, 0x112c740, 0x1970300, 0x192a8f0, 0x192a648},
     {0x2ca9528, 0x2ca95b0},
     0x1978270,
     0x1963f44,
     0x18f4404,
     0x18f443c,
     0x18f3f58,
     0x18f4628,
     0x2b7c200,
     0x2cae330,
     0x2caa1a0}};
static const FileCueProfile *file_profile;
static uintptr_t file_bias;
static void *file_original[13];
static _Atomic(const struct azn_file_cue_callbacks *) file_callbacks = NULL;
static _Atomic(azn_file_input_observer_fn) file_input_observer;
static void observe_file_input(azn_file_input_observer_fn observer) {
    atomic_store_explicit(&(file_input_observer), observer, memory_order_release);
}
struct FileWireRow {
    uint64_t token;
    uint32_t bytes;
    char digest[65];
};
struct FileServe {
    uintptr_t server, request, identity;
    uint32_t seq;
    unsigned n;
    FileWireRow rows[AZN_FILE_CUE_MAX];
};
/* The two original merged atom members are GNU shared_ptr pairs. Both stock
 * profiles retain/release their strong count at control+8 and dispose/destroy
 * through control vslots 2/3. Keep those actual owners, so an address reused
 * after cache eviction cannot inherit a previous physical-input token. */
struct FileAtomOwner {
    uintptr_t object, control;
};
static bool file_atom_acquire(FileAtomOwner *out, uintptr_t object, uintptr_t control) {
    if (!object) return !control;
    if (!ptr(object) || !ptr(control)) return false;
    __auto_type count = ((int32_t *) (control + 8));
    int32_t value = __atomic_load_n(count, __ATOMIC_ACQUIRE);
    for (unsigned attempt = 0; attempt < 8 && value > 0 && value < INT32_MAX; ++attempt) {
        if (__atomic_compare_exchange_n(count, &value, value + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            out->object = object;
            out->control = control;
            return true;
        }
    }
    return false;
}
static void file_atom_release(FileAtomOwner *owner) {
    const uintptr_t control = owner->control;
    (*owner) = (FileAtomOwner) {0};
    if (!control || __atomic_sub_fetch(((int32_t *) (control + 8)), 1, __ATOMIC_ACQ_REL)) return;
    ((void (*)(uintptr_t))(object_word(object_word(control) + 16)))(control);
    if (!__atomic_sub_fetch(((int32_t *) (control + 12)), 1, __ATOMIC_ACQ_REL))
        ((void (*)(uintptr_t))(object_word(object_word(control) + 24)))(control);
}
#include "sdk_status_file_input.inc"
struct FileRead {
    uintptr_t accessor, identity;
    const struct azn_file_cue_callbacks *callbacks;
    uint64_t token;
    bool began, merged, bad, tag_seen, cached;
    FileAtomOwner fresh_inputs[4];
    unsigned fresh_count;
    FileInputUse inputs;
    struct azn_file_cue_row atoms[AZN_FILE_CUE_MAX];
    FileAtomOwner owners[AZN_FILE_CUE_MAX][2];
    uint64_t reused[AZN_FILE_CUE_MAX];
    uint32_t lengths[AZN_FILE_CUE_MAX];
    unsigned count;
};
static void file_read_drop(FileRead *self) {
    if (self->token) {
        uint64_t ignored[AZN_FILE_CUE_MAX] = {0};
        self->callbacks->complete(self->token, NULL, 0, ignored);
    }
    for (unsigned i = 0; i < AZN_FILE_CUE_MAX; ++i)
        for (unsigned j = 0; j < 2; ++j) file_atom_release(&(self->owners[i][j]));
    for (unsigned i = 0; i < 4; ++i) file_atom_release(&(self->fresh_inputs[i]));
    file_inputs_drop(&self->inputs);
}

static _Thread_local FileServe *file_serving = NULL;
static _Thread_local FileRead *file_reading = NULL;
struct FileCacheCall {
    uintptr_t owner, extension, object, control;
    unsigned loads;
};
static _Thread_local FileCacheCall *file_cache_call = NULL;
static bool file_fresh_success(const FileFreshInput *input, int result, uintptr_t output) {
    return !result && !input->bad && input->opens && input->opens <= 2 && input->confirmed == input->opens &&
           (input->mask == 1 || input->mask == 2) && output;
}
static void file_fresh_finish(FileFreshInput *input, int result, uintptr_t output) {
    if (!input->read) return;
    bool successful = file_fresh_success(input, result, output);
    uint64_t token = input->api->seal(input->read, successful);
    input->read = 0;
    file_input_remember(input->api, token, input->mask, successful ? object_word(output) : 0, successful ? object_word(output + 8) : 0);
}
static void file_cache_loaded(uintptr_t owner, uintptr_t extension, uintptr_t output, int result) {
    FileCacheCall *call = file_cache_call;
    if (!call || call->owner != owner || call->extension != extension) return;
    ++call->loads;
    if (!result && output) {
        call->object = object_word(output);
        call->control = object_word(output + 8);
    }
}
static int file_fresh_load(uintptr_t owner, uintptr_t output, uintptr_t extension) {
    FileFreshInput input __attribute__((cleanup(file_fresh_drop))) = {.api = file_inputs_api()};
    POINTER_SCOPE(input_scope, file_fresh_input, &input);
    const int result = ((int (*)(uintptr_t, uintptr_t, uintptr_t)) file_original[12])(owner, output, extension);
    file_fresh_finish(&input, result, output);
    file_cache_loaded(owner, extension, output, result);
    return result;
}
static bool file_cache_fresh(const FileCacheCall *call, uintptr_t object, uintptr_t control) {
    return object && control && call->loads == 1 && call->object == object && call->control == control;
}
static bool file_read_retains(const FileRead *read, uintptr_t object, uintptr_t control) {
    for (unsigned i = 0; i < read->fresh_count; ++i)
        if (read->fresh_inputs[i].object == object && read->fresh_inputs[i].control == control) return true;
    return false;
}
static bool file_cache_retain(FileRead *read, uintptr_t object, uintptr_t control, bool fresh) {
    file_inputs_observe(&read->inputs, object, control);
    bool retained = file_read_retains(read, object, control);
    if (fresh && !retained) {
        if (read->fresh_count == 4 || !file_atom_acquire(&read->fresh_inputs[read->fresh_count], object, control)) fresh = false;
        else ++read->fresh_count;
    }
    fresh |= retained;
    if (!fresh) read->cached = true;
    return fresh;
}
static void file_cached_notify(void) {
    azn_file_input_observer_fn observer = atomic_load_explicit(&(file_input_observer), memory_order_acquire);
    if (observer) observer(AZN_INPUT_CACHED, -1);
}
static int file_cache_get(uintptr_t owner, uintptr_t output, uintptr_t extension) {
    FileCacheCall call = {.owner = owner, .extension = extension};
    POINTER_SCOPE(scope, file_cache_call, &call);
    const int result = ((int (*)(uintptr_t, uintptr_t, uintptr_t)) file_original[11])(owner, output, extension);
    FileRead *read = file_reading;
    if (read) read->tag_seen = true;
    if (result) return result;
    const uintptr_t object = output ? object_word(output) : 0;
    const uintptr_t control = output ? object_word(output + 8) : 0;
    if (file_map_inputs) file_inputs_observe(file_map_inputs, object, control);
    bool fresh = file_cache_fresh(&call, object, control);
    if (read) fresh = file_cache_retain(read, object, control, fresh);
    if (!fresh) file_cached_notify();
    return result;
}
struct FileOrigins {
    unsigned count;
    FileAtomOwner owners[AZN_FILE_CUE_MAX][2];
    struct azn_file_cue_row atoms[AZN_FILE_CUE_MAX];
    uint64_t tokens[AZN_FILE_CUE_MAX];
};
static FileOrigins file_origins[16];
static unsigned file_origin_next;
static atomic_flag file_origin_busy = ATOMIC_FLAG_INIT;
static void release_origins(FileOrigins *origin) {
    __auto_type cb = atomic_load_explicit(&(file_callbacks), memory_order_acquire);
    if (origin->count && cb && cb->hold) cb->hold(origin->tokens, origin->count, 0);
    for (unsigned i = 0; i < AZN_FILE_CUE_MAX; ++i)
        for (unsigned j = 0; j < 2; ++j) file_atom_release(&(origin->owners[i][j]));
}
static uint64_t reused_origin(const FileRead *read, unsigned row) {
    if (atomic_flag_test_and_set_explicit(&(file_origin_busy), memory_order_acquire)) return 0;
    uint64_t token = 0;
    for (unsigned origin_index = 0; origin_index < sizeof(file_origins) / sizeof(file_origins)[0]; ++origin_index) {
        const FileOrigins *origin = &(file_origins[origin_index]);
        for (unsigned i = 0; i < origin->count; ++i) {
            if (!origin->tokens[i] || memcmp(origin->owners[i], read->owners[row], sizeof read->owners[row]) ||
                memcmp(&origin->atoms[i], &read->atoms[row], sizeof read->atoms[row])) continue;
            token = origin->tokens[i];
            break;
        }
        if (token) break;
    }
    atomic_flag_clear_explicit(&(file_origin_busy), memory_order_release);
    return token;
}
static void file_origin_release_owners(FileOrigins *origin) {
    for (unsigned i = 0; i < AZN_FILE_CUE_MAX; ++i)
        for (unsigned j = 0; j < 2; ++j) file_atom_release(&origin->owners[i][j]);
}
static bool file_origin_hold_row(FileOrigins *next, const FileRead *read, unsigned i) {
    for (unsigned j = 0; j < 2; ++j)
        if (!file_atom_acquire(&next->owners[i][j], read->owners[i][j].object, read->owners[i][j].control)) return false;
    return true;
}
static void remember_origins(const FileRead *read, const uint64_t *tokens) {
    FileOrigins next = {0};
    next.count = read->count;
    bool retained = false;
    for (unsigned i = 0; i < read->count; ++i) {
        if (!tokens[i]) continue;
        if (!file_origin_hold_row(&next, read, i)) {
            file_origin_release_owners(&next);
            return;
        }
        next.atoms[i] = read->atoms[i];
        next.tokens[i] = tokens[i];
        retained = true;
    }
    if (!retained) return;
    if (read->callbacks->hold) read->callbacks->hold(next.tokens, next.count, 1);
    if (atomic_flag_test_and_set_explicit(&(file_origin_busy), memory_order_acquire)) {
        release_origins(&(next));
        return;
    }
    __auto_type slot = &(file_origins[file_origin_next++ % 16]);
    FileOrigins retired = (*slot);
    (*slot) = next;
    atomic_flag_clear_explicit(&(file_origin_busy), memory_order_release);
    release_origins(&(retired));
}
static bool file_atom_bytes(uintptr_t p, unsigned header, unsigned total, uintptr_t data) {
    return header >= 12 && total >= header && total > 14 && total <= AZN_FILE_CUE_BYTES && object_word(p + 0x28) == total && ptr(data);
}
static bool original_atom(uintptr_t p, uintptr_t table, struct azn_file_atom *out) {
    if (!p) return true;
    if (!typed(p, file_bias + table) || !SCALAR(uint8_t, p + 0x38)) return false;
    const unsigned header = SCALAR(uint32_t, p + 0x18), total = SCALAR(uint32_t, p + 0x1c);
    const uintptr_t data = object_word(p + 0x30);
    if (!file_atom_bytes(p, header, total, data)) return false;
    out->header = header;
    out->total = total;
    azsha256_hex(((const void *) (data)), total - 14, out->digest);
    return true;
}
static bool file_merged_vector(uintptr_t b, uintptr_t e) {
    return ptr(b) && e >= b && !((e - b) % 8) && (e - b) / 8 <= AZN_FILE_CUE_MAX;
}
static bool file_merged_atoms(uintptr_t merged, struct azn_file_cue_row *row) {
    return original_atom(object_word(merged + 0x30), file_profile->atoms[0], &row->pcpt) &&
           original_atom(object_word(merged + 0x40), file_profile->atoms[1], &row->pcp2) && (row->pcpt.total || row->pcp2.total);
}
static bool file_merged_row(uintptr_t merged, FileRead *read, unsigned i) {
    if (!ptr(merged)) return false;
    uint64_t bytes = object_word(merged + 0x10);
    if (bytes < 4 || bytes > AZN_FILE_CUE_BYTES || !file_merged_atoms(merged, &read->atoms[i])) return false;
    for (unsigned j = 0; j < 2; ++j)
        if (!file_atom_acquire(&read->owners[i][j], object_word(merged + 0x30 + j * 0x10), object_word(merged + 0x38 + j * 0x10))) return false;
    read->reused[i] = reused_origin(read, i);
    read->lengths[i] = (uint32_t) bytes;
    return true;
}
static bool merged_rows(uintptr_t vector, FileRead *read) {
    if (read->merged) {
        read->bad = true;
        return false;
    }
    read->merged = true;
    if (!ptr(vector)) return false;
    const uintptr_t b = object_word(vector), e = object_word(vector + 8);
    if (!file_merged_vector(b, e)) return false;
    unsigned count = (unsigned) ((e - b) / 8);
    for (unsigned i = 0; i < count; ++i)
        if (!file_merged_row(object_word(b + i * 8), read, i)) return false;
    read->count = count;
    return true;
}
static int file_merge(uintptr_t accessor, uintptr_t first, uintptr_t second, unsigned kind, uintptr_t output) {
    int result = ((int (*)(uintptr_t, uintptr_t, uintptr_t, unsigned, uintptr_t))(file_original[3]))(
        accessor, first, second, kind, output);
    __auto_type read = file_reading;
    if (!result && read && read->accessor == accessor && kind == 1 && output &&
        ((uintptr_t) (__builtin_return_address(0))) == file_bias + file_profile->merge_return)
        merged_rows(object_word(output), read);
    return result;
}
static bool file_wire_row_valid(uintptr_t start, uint64_t bytes, size_t offset, unsigned size) {
    return offset <= bytes && size <= bytes - offset && size >= 4 && SCALAR(uint32_t, start + offset) == size;
}
static bool wire_rows(const FileRead *read, uintptr_t block, FileServe *serve) {
    if (!ptr(block) || !read->count) return false;
    const uintptr_t start = object_word(block);
    const uint64_t bytes = object_word(block + 8);
    if (!ptr(start) || bytes > AZN_FILE_CUE_MAX * AZN_FILE_CUE_BYTES) return false;
    size_t offset = 0;
    for (unsigned i = 0; i < read->count; ++i) {
        unsigned size = read->lengths[i];
        if (!file_wire_row_valid(start, bytes, offset, size)) return false;
        serve->rows[i].bytes = size - 4;
        azsha256_hex(((const void *) (start + offset + 4)), size - 4, serve->rows[i].digest);
        offset += size;
    }
    return offset == bytes;
}
static void file_legacy_begin(FileRead *read, bool nominal) {
    if (!read || read->inputs.api || !nominal || read->began) return;
    read->began = true;
    const uintptr_t storage = object_word(read->accessor + 0x18), identity = read->identity;
    if (ptr(storage))
        read->token = read->callbacks->begin((const char *) object_word(storage + 8), object_word(storage + 16),
                                             (const uint16_t *) object_word(identity), object_word(identity + 8), (const uint16_t *) object_word(identity + 32), object_word(identity + 40));
}
static void file_fresh_opened(FileFreshInput *fresh, int descriptor) {
    if (!fresh || !fresh->read) return;
    unsigned mask = fresh->api->opened(fresh->read, descriptor);
    if ((mask != 1 && mask != 2) || (fresh->mask && fresh->mask != mask)) fresh->bad = true;
    else {
        fresh->mask = mask;
        ++fresh->confirmed;
    }
}
static void file_input_opened(uintptr_t self, FileFreshInput *fresh, FileRead *read, azn_file_input_observer_fn observer) {
    /* CFpHandle::open called fopen64 in this invocation and committed the
     * new FILE before returning. The callback borrows its actual fd. */
    FILE *stream = (FILE *) object_word(self + 0x28);
    if (!stream) return;
    const int descriptor = fileno(stream);
    file_fresh_opened(fresh, descriptor);
    if (read && read->token) read->callbacks->opened(read->token, descriptor);
    if (observer) observer(AZN_INPUT_OPENED, descriptor);
}
static void file_input_opening(FileFreshInput *fresh, bool nominal, uintptr_t self, uintptr_t path) {
    if (nominal && fresh) file_input_start(fresh, self, path);
    else if (fresh) fresh->bad = true;
}
static unsigned file_input_open(uintptr_t self, uintptr_t path, unsigned flags, int *error) {
    FileRead *read = file_reading;
    azn_file_input_observer_fn observer = atomic_load_explicit(&(file_input_observer), memory_order_acquire);
    const bool nominal = typed(self, file_bias + file_profile->input_table);
    FileFreshInput *fresh = file_fresh_input;
    file_input_opening(fresh, nominal, self, path);
    if (nominal && observer) observer(AZN_INPUT_OPENING, -1);
    file_legacy_begin(read, nominal);
    const unsigned result = ((unsigned (*)(uintptr_t, uintptr_t, unsigned, int *)) file_original[10])(self, path, flags, error);
    if (!result && nominal) file_input_opened(self, fresh, read, observer);
    if (fresh && result) fresh->bad = true;
    return result;
}
static bool file_get_eligible(const struct azn_file_cue_callbacks *cb, const FileServe *serve, uintptr_t accessor,
                              uintptr_t identity, uintptr_t output, uintptr_t count, uintptr_t caller) {
    return cb && serve && serve->identity == identity && ptr(accessor) && ptr(identity) && output && count && caller == file_bias + file_profile->getter_return;
}
static bool file_read_fresh(const FileRead *read) {
    return read->inputs.api ? read->inputs.seen && !read->inputs.bad : read->tag_seen && !read->cached;
}
static void file_get_complete(FileRead *read, FileServe *serve) {
    uint64_t ids[AZN_FILE_CUE_MAX] = {0};
    if (read->inputs.api) read->token = file_inputs_join(&read->inputs);
    if (read->token) {
        const bool fresh = file_read_fresh(read);
        read->callbacks->complete(read->token, fresh ? read->atoms : NULL, fresh ? read->count : 0, ids);
        read->token = 0;
        remember_origins(read, ids);
    }
    serve->n = read->count;
    for (unsigned i = 0; i < read->count; ++i) serve->rows[i].token = ids[i] ? ids[i] : read->reused[i];
}
static int file_get(uintptr_t accessor, uintptr_t identity, uintptr_t output, uintptr_t count) {
    __auto_type cb = atomic_load_explicit(&(file_callbacks), memory_order_acquire);
    __auto_type serve = file_serving;
    const bool eligible = file_get_eligible(cb, serve, accessor, identity, output, count, (uintptr_t) __builtin_return_address(0));
    FileRead read __attribute__((cleanup(file_read_drop))) = {.inputs = {.api = file_inputs_api()}};
    read.accessor = accessor;
    read.identity = identity;
    read.callbacks = cb;
    POINTER_SCOPE(scope, file_reading, eligible ? &read : NULL);
    int result = ((int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))(file_original[2]))(accessor, identity, output, count);
    if (!result && eligible && !read.bad && read.count == object_word(count) && wire_rows(&(read), object_word(output), serve)) {
        file_get_complete(&read, serve);
    }
    return result;
}
/* All pointers below are comparison keys. Only a native owner lends an
 * object for reading. A collision, duplicate or unfinished transaction can
 * remove a candidate but cannot mint a new association. */
enum {
    FileIssued = 1,
    FileSent,
    FileWire,
    FileReceived,
    FileServed,
    FileResponse,
    FileResponseWire,
    FileDelivered
};
struct FileRoute {
    uint32_t seq;
    unsigned phase, count;
    uintptr_t issuer, query, request, response, delivered;
    FileWireRow rows[AZN_FILE_CUE_MAX];
};
static atomic_flag file_guard_busy = ATOMIC_FLAG_INIT;
static _Atomic(uint64_t) file_guard_lost;
struct FileGuard {
    bool held;
};
static FileGuard file_guard_acquire(uint32_t seq) {
    bool held = !atomic_flag_test_and_set_explicit(&file_guard_busy, memory_order_acquire);
    if (!held) atomic_fetch_or_explicit(&file_guard_lost, UINT64_C(1) << (seq % 32), memory_order_release);
    return (FileGuard) {held};
}
static void file_guard_drop(FileGuard *guard) {
    if (guard->held) atomic_flag_clear_explicit(&file_guard_busy, memory_order_release);
}

static FileRoute file_routes[32];
static uint32_t highest_file_request;
static bool file_sequence_wrapped;
static void file_prune() {
    uint64_t lost = atomic_exchange_explicit(&(file_guard_lost), 0, memory_order_acq_rel);
    while (lost) {
        unsigned i = (unsigned) (__builtin_ctzll(lost));
        file_routes[i].phase = 0;
        lost &= lost - 1;
    }
}
static unsigned message_kind(uintptr_t p) {
    if (!ptr(p) || SCALAR(uint32_t, p + 4) != UINT32_C(0x872349ae)) return 0;
    return SCALAR(uint16_t, p);
}
static void file_map_transport(unsigned stage, uintptr_t packet, unsigned peer);
static void file_map_reply(uintptr_t server, uintptr_t packet, unsigned peer);
static bool file_issue_ticket(uintptr_t ticket, uint32_t seq) {
    return seq && SCALAR(uint32_t, ticket + 0x20) == seq && SCALAR(uint16_t, ticket + 0x1a) == 0x4e02 &&
           SCALAR(uint16_t, ticket + 0x1c) == 0x2b04;
}
static void file_route_issue(FileRoute *r, uintptr_t ticket, uintptr_t query, uint32_t seq) {
    if (highest_file_request > seq && highest_file_request - seq > INT32_MAX) file_sequence_wrapped = true;
    if (r->seq == seq || file_sequence_wrapped) r->phase = 0;
    else if (seq > highest_file_request) {
        highest_file_request = seq;
        *r = (FileRoute) {0};
        r->seq = seq;
        r->phase = FileIssued;
        r->query = query;
        r->issuer = ticket;
    }
}
static uintptr_t file_issue(uintptr_t ticket) {
    uintptr_t query = ((uintptr_t (*)(uintptr_t)) file_original[0])(ticket);
    if (!atomic_load_explicit(&(file_callbacks), memory_order_acquire) || message_kind(query) != 0x2b04) return query;
    uint32_t seq = SCALAR(uint32_t, query + 8);
    if (!file_issue_ticket(ticket, seq)) return query;
    FileGuard guard __attribute__((cleanup(file_guard_drop))) = file_guard_acquire(seq);
    if (guard.held) {
        file_prune();
        file_route_issue(&file_routes[seq % 32], ticket, query, seq);
    }
    return query;
}
static bool file_route_request(FileRoute *r, unsigned stage, uintptr_t packet) {
    switch (stage) {
    case AZN_FILE_SEND:
        if (r->phase != FileIssued || packet != r->query) return false;
        r->phase = FileSent;
        return true;
    case AZN_FILE_WIRE:
        if (r->phase != FileSent || packet != r->query) return false;
        r->phase = FileWire;
        return true;
    case AZN_FILE_SERVER:
        if (r->phase != FileWire) return false;
        r->request = packet;
        r->phase = FileReceived;
        return true;
    default: return false;
    }
}
static bool file_route_client(FileRoute *r, uintptr_t packet) {
    if (r->phase == FileResponseWire) {
        r->delivered = packet;
        r->phase = FileDelivered;
        return true;
    }
    /* Other registered listeners see the same decoded packet before the
     * accepting ticket. A second packet with this sequence is ambiguous. */
    return r->phase == FileDelivered && r->delivered == packet;
}
static bool file_route_response(FileRoute *r, unsigned stage, uintptr_t packet) {
    switch (stage) {
    case AZN_FILE_SEND:
        if (r->phase != FileServed || packet != r->response) return false;
        r->phase = FileResponse;
        return true;
    case AZN_FILE_WIRE:
        if (r->phase != FileResponse || packet != r->response) return false;
        r->phase = FileResponseWire;
        return true;
    case AZN_FILE_CLIENT: return file_route_client(r, packet);
    default: return false;
    }
}
static bool file_route_unexpected(unsigned stage, unsigned kind) {
    return (kind == 0x2b04 && stage != AZN_FILE_CLIENT) || (kind == 0x4e02 && stage != AZN_FILE_SERVER);
}
static void file_route_transport(FileRoute *r, uint32_t seq, unsigned stage, unsigned kind, uintptr_t packet) {
    if (!seq || r->seq != seq || !r->phase) return;
    bool accepted = kind == 0x2b04 ? file_route_request(r, stage, packet) : file_route_response(r, stage, packet);
    if (!accepted && file_route_unexpected(stage, kind)) r->phase = 0;
}
static void file_transport(unsigned stage, uintptr_t packet, unsigned peer) {
    file_map_transport(stage, packet, peer);
    if (peer || !atomic_load_explicit(&(file_callbacks), memory_order_acquire)) return;
    unsigned kind = message_kind(packet);
    if (kind != 0x2b04 && kind != 0x4e02) return;
    uint32_t seq = SCALAR(uint32_t, packet + 8);
    FileGuard guard __attribute__((cleanup(file_guard_drop))) = file_guard_acquire(seq);
    if (!guard.held) return;
    file_prune();
    file_route_transport(&file_routes[seq % 32], seq, stage, kind, packet);
}
static void file_serve_begin(FileServe *serve, uintptr_t server, uintptr_t request_ref, unsigned peer, uintptr_t identity) {
    if (!atomic_load_explicit(&(file_callbacks), memory_order_acquire) || peer || !ptr(request_ref)) return;
    uintptr_t request = object_word(request_ref);
    if (message_kind(request) != 0x2b04) return;
    uint32_t seq = SCALAR(uint32_t, request + 8);
    FileGuard guard __attribute__((cleanup(file_guard_drop))) = file_guard_acquire(seq);
    if (!guard.held) return;
    file_prune();
    FileRoute *r = &file_routes[seq % 32];
    if (r->seq != seq || r->phase != FileReceived || r->request != request) return;
    serve->server = server;
    serve->request = request;
    serve->identity = identity;
    serve->seq = seq;
}
static void file_serve(uintptr_t server, uintptr_t request_ref, unsigned peer, uintptr_t identity) {
    FileServe serve = {0};
    file_serve_begin(&serve, server, request_ref, peer, identity);
    POINTER_SCOPE(scope, file_serving, serve.seq ? &serve : NULL);
    ((void (*)(uintptr_t, uintptr_t, unsigned, uintptr_t)) file_original[1])(server, request_ref, peer, identity);
}
static bool file_reply_eligible(const FileServe *serve, uintptr_t server, uintptr_t packet, unsigned peer) {
    return !peer && serve && serve->server == server && serve->n && message_kind(packet) == 0x4e02 &&
           SCALAR(uint32_t, packet + 8) == serve->seq;
}
static void file_reply_record(const FileServe *serve, uintptr_t packet) {
    FileGuard guard __attribute__((cleanup(file_guard_drop))) = file_guard_acquire(serve->seq);
    if (!guard.held) return;
    file_prune();
    FileRoute *r = &file_routes[serve->seq % 32];
    if (r->seq != serve->seq || r->phase != FileReceived || r->request != serve->request) return;
    r->phase = FileServed;
    r->response = packet;
    r->count = serve->n;
    memcpy(r->rows, serve->rows, serve->n * sizeof *r->rows);
}
static uintptr_t file_reply(uintptr_t server, uintptr_t packet, unsigned peer) {
    file_map_reply(server, packet, peer);
    FileServe *serve = file_serving;
    if (file_reply_eligible(serve, server, packet, peer)) file_reply_record(serve, packet);
    return ((uintptr_t (*)(uintptr_t, uintptr_t, unsigned)) file_original[7])(server, packet, peer);
}
/* MemoryCueSet rejects duplicates by these six fields before insertion. Its
 * sort moves rows in this same counted Content. Compare this key only inside
 * the witnessed source object, after the original append succeeds. */
struct FileKey {
    uint32_t kind, rate;
    uint64_t clocks[4];
};
static FileKey file_key(uintptr_t row) {
    FileKey k = {0};
    k.kind = SCALAR(uint8_t, row);
    k.rate = SCALAR(uint32_t, row + 4);
    memcpy(k.clocks, ((const void *) (row + 8)), sizeof k.clocks);
    return k;
}
static bool same_file_clock(uint64_t a, uint64_t b, uint32_t rate, unsigned index) {
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    if (a == absent || b == absent) return a == absent && b == absent;
    const int order = compare_timestamps(a, b, index % 2 ? rate : 1000000u, rate, 1);
    return order != AZ_TIME_INVALID && order == AZ_TIME_EQUAL;
}
static bool same_file_key(const FileKey *a, const FileKey *b) {
    if (a->kind != b->kind || a->rate != b->rate || !a->rate || a->rate >= INT32_MAX) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (!same_file_clock(a->clocks[i], b->clocks[i], a->rate, i)) return false;
    return true;
}
struct FileBank {
    unsigned added;
    uintptr_t content;
    uint64_t pin;
    struct Added {
        FileKey key;
        uint64_t token;
    } row[10];
};
struct FileImport {
    unsigned count, decoded;
    bool bad;
    FileWireRow wire[AZN_FILE_CUE_MAX];
    uintptr_t ticket, decoded_row;
    uint64_t token;
    FileBank bank[2];
};
static void file_import_drop(FileImport *f) {
    for (unsigned i = 2; i > 0; --i)
        if (f->bank[i - 1].pin) azn_pin_drop(f->bank[i - 1].pin);
}

static _Thread_local FileImport *file_import = NULL;
struct FileContent {
    uint64_t pin, token[10];
};
static atomic_flag file_content_busy = ATOMIC_FLAG_INIT;
static FileContent imported_files[16];
static unsigned file_content_next;
static void hold_file_content(const FileContent *content, int acquire) {
    __auto_type cb = atomic_load_explicit(&(file_callbacks), memory_order_acquire);
    if (!cb || cb->size < offsetof(struct azn_file_cue_callbacks, hold) + sizeof cb->hold || !cb->hold) return;
    uint64_t tokens[10];
    size_t n = 0;
    for (unsigned i = 0; i < 10; ++i)
        if (content->token[i]) tokens[n++] = content->token[i];
    if (n) cb->hold(tokens, n, acquire);
}
static bool file_bank_bounds(uintptr_t b, uintptr_t e, unsigned hot) {
    return ptr(b) && e >= b && !((e - b) % 0x80) && (e - b) / 0x80 <= (hot ? 8u : 10u);
}
static unsigned file_bank_key(const FileBank *f, unsigned i, uintptr_t b, unsigned count, unsigned hot) {
    unsigned index = 0, occurrences = 0;
    for (unsigned j = 0; j < count; ++j) {
        FileKey k = file_key(b + j * 0x80);
        if (same_file_key(&k, &f->row[i].key)) {
            index = j;
            ++occurrences;
        }
    }
    unsigned key = hot ? f->row[i].key.kind : index + 1;
    return occurrences == 1 ? key : 0;
}
static void file_bank_tokens(const FileBank *f, unsigned hot, uintptr_t b, unsigned count, FileContent *next) {
    for (unsigned i = 0; i < f->added; ++i) {
        unsigned key = file_bank_key(f, i, b, count, hot);
        if (key && key <= (hot ? 8u : 10u) && !next->token[key - 1]) next->token[key - 1] = f->row[i].token;
    }
}
static void file_bank_publish(FileBank *f, FileContent *next) {
    next->pin = f->pin;
    hold_file_content(next, 1);
    if (atomic_flag_test_and_set_explicit(&(file_content_busy), memory_order_acquire)) {
        hold_file_content(next, 0);
        return;
    }
    f->pin = 0;
    FileContent *slot = &imported_files[file_content_next++ % 16];
    FileContent retired = *slot;
    *slot = *next;
    atomic_flag_clear_explicit(&(file_content_busy), memory_order_release);
    if (retired.pin) {
        hold_file_content(&retired, 0);
        azn_pin_drop(retired.pin);
    }
}
static void finish_bank(FileImport *import, unsigned hot) {
    FileBank *f = &import->bank[hot];
    const uintptr_t table = hot ? file_bias + file_profile->hot_content : profile.content;
    if (!f->pin || !f->added || !typed(f->content, table)) return;
    const uintptr_t b = object_word(f->content + 0x28), e = object_word(f->content + 0x30);
    if (!file_bank_bounds(b, e, hot)) return;
    FileContent next = {0};
    file_bank_tokens(f, hot, b, (unsigned) ((e - b) / 0x80), &next);
    file_bank_publish(f, &next);
}
static void finish_import(FileImport *f) {
    if (f->bad || f->count != f->decoded) return;
    finish_bank(f, 0);
    finish_bank(f, 1);
}
static uint64_t imported_memory_row(uintptr_t content, unsigned entry) {
    if (!entry || entry > 10 || atomic_flag_test_and_set_explicit(&(file_content_busy), memory_order_acquire)) return 0;
    uint64_t result = 0;
    for (unsigned f_index = 0; f_index < sizeof(imported_files) / sizeof(imported_files)[0]; ++f_index) {
        const FileContent *f = &(imported_files[f_index]);
        if (!f->pin || !f->token[entry - 1]) continue;
        HOLD(ref, f->pin);
        if (ref.token && pin_for(ref.token)->object == content) {
            result = f->token[entry - 1];
            break;
        }
    }
    atomic_flag_clear_explicit(&(file_content_busy), memory_order_release);
    return result;
}
static bool file_ticket_route(const FileRoute *r, uintptr_t ticket, uintptr_t packet, uint32_t seq) {
    return r->seq == seq && r->phase == FileDelivered && r->issuer == ticket && r->delivered == packet && SCALAR(uint32_t, ticket + 0x20) == seq;
}
static void file_ticket_begin(FileImport *f, uintptr_t ticket) {
    uintptr_t packet = object_word(ticket + 0x28);
    if (!atomic_load_explicit(&(file_callbacks), memory_order_acquire) || message_kind(packet) != 0x4e02 ||
        SCALAR(uint16_t, ticket + 0x1a) != 0x4e02 || SCALAR(uint16_t, ticket + 0x1c) != 0x2b04) return;
    uint32_t seq = SCALAR(uint32_t, packet + 8);
    FileGuard guard __attribute__((cleanup(file_guard_drop))) = file_guard_acquire(seq);
    if (!guard.held) return;
    file_prune();
    FileRoute *r = &file_routes[seq % 32];
    if (!file_ticket_route(r, ticket, packet, seq)) return;
    f->ticket = ticket;
    f->count = r->count;
    memcpy(f->wire, r->rows, r->count * sizeof *r->rows);
    r->phase = 0;
}
static uintptr_t file_ticket(uintptr_t ticket) {
    FileImport f __attribute__((cleanup(file_import_drop))) = {0};
    file_ticket_begin(&f, ticket);
    POINTER_SCOPE(scope, file_import, f.count ? &f : NULL);
    return ((uintptr_t (*)(uintptr_t)) file_original[4])(ticket);
}
static void publish_import(uintptr_t ticket, uintptr_t memory, uintptr_t hot, uintptr_t result) {
    __auto_type f = file_import;
    if (f && f->ticket == ticket && result && SCALAR(unsigned, result) == 1 && object_word(ticket + 0x70) &&
        ((uintptr_t) (__builtin_return_address(0))) == file_bias + file_profile->finish_return)
        finish_import(f);
    ((void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t))(file_original[8]))(ticket, memory, hot, result);
}
#if defined(__aarch64__)
void azn_file_decode_entry();
void azn_invoke_file_decode(void *target, uintptr_t block, unsigned rate, uintptr_t row);
__asm__(".text\n.p2align 2\n.global azn_file_decode_entry\n.hidden azn_file_decode_entry\n.type azn_file_decode_entry,%function\n"
        "azn_file_decode_entry:\nmov x2,x8\nb azn_file_decode\n.size azn_file_decode_entry,.-azn_file_decode_entry\n"
        ".global azn_invoke_file_decode\n.hidden azn_invoke_file_decode\n.type azn_invoke_file_decode,%function\n"
        "azn_invoke_file_decode:\nmov x4,x0\nmov x0,x1\nmov w1,w2\nmov x8,x3\nbr x4\n"
        ".size azn_invoke_file_decode,.-azn_invoke_file_decode\n");
static bool file_decode_eligible(const FileImport *f, uintptr_t block, uintptr_t row, uintptr_t caller) {
    return caller == file_bias + file_profile->decoder_return && f->decoded < f->count && ptr(block) && ptr(row);
}
static void file_decode_witness(FileImport *f, uintptr_t block, uintptr_t row) {
    const FileWireRow *expected = &f->wire[f->decoded++];
    uintptr_t data = object_word(block);
    size_t n = object_word(block + 8);
    char digest[65] = {0};
    if (ptr(data) && n == expected->bytes && n <= AZN_FILE_CUE_BYTES) azsha256_hex((const void *) data, n, digest);
    if (memcmp(digest, expected->digest, sizeof digest)) f->bad = true;
    else {
        f->decoded_row = row;
        f->token = expected->token;
    }
}
__attribute__((visibility("hidden"))) void azn_file_decode(uintptr_t block, unsigned rate, uintptr_t row) {
    FileImport *f = file_import;
    if (f && !f->bad) {
        f->decoded_row = 0;
        f->token = 0;
        if (!file_decode_eligible(f, block, row, (uintptr_t) __builtin_return_address(0))) f->bad = true;
        else file_decode_witness(f, block, row);
    }
    azn_invoke_file_decode(file_original[5], block, rate, row);
}

#endif
static bool file_append_origin(const FileImport *f, uintptr_t row, uintptr_t caller, unsigned hot) {
    return f && !f->bad && f->token && row == f->decoded_row && caller == file_bias + (hot ? file_profile->hot_return : file_profile->append_return);
}
static bool file_hot_row(uintptr_t row) { return SCALAR(uint8_t, row) >= 1 && SCALAR(uint8_t, row) <= 8; }
static void file_append_capture(FileImport *f, uintptr_t content, uintptr_t row, unsigned hot) {
    FileBank *bank = &f->bank[hot];
    if (!bank->pin) {
        bank->pin = azn_owned_pin(content);
        bank->content = content;
    }
    if (!bank->pin || bank->content != content || bank->added >= (hot ? 8u : 10u)) f->bad = true;
    else {
        struct Added *a = &bank->row[bank->added++];
        a->key = file_key(row);
        a->token = f->token;
    }
    f->decoded_row = 0;
    f->token = 0;
}
static unsigned file_append_0(uintptr_t content, uintptr_t row) {
    FileImport *f = file_import;
    bool from_file = file_append_origin(f, row, (uintptr_t) __builtin_return_address(0), 0) &&
                     typed(content, profile.content) && !SCALAR(uint8_t, row);
    unsigned accepted = ((unsigned (*)(uintptr_t, uintptr_t)) file_original[6])(content, row);
    if (from_file && accepted) file_append_capture(f, content, row, 0);
    return accepted;
}
static unsigned file_append_1(uintptr_t content, uintptr_t row) {
    FileImport *f = file_import;
    const uintptr_t table = file_bias + file_profile->hot_content;
    bool from_file = file_append_origin(f, row, (uintptr_t) __builtin_return_address(0), 1) &&
                     typed(content, table) && file_hot_row(row);
    unsigned accepted = ((unsigned (*)(uintptr_t, uintptr_t)) file_original[9])(content, row);
    if (from_file && accepted) file_append_capture(f, content, row, 1);
    return accepted;
}
#if defined(__aarch64__)
static bool file_callbacks_valid(const struct azn_file_cue_callbacks *cb) {
    return cb && cb->version == 2 && cb->size >= offsetof(struct azn_file_cue_callbacks, opened) + sizeof cb->opened &&
           cb->begin && cb->complete && cb->hold && cb->opened;
}
static bool file_install_request(int build, azn_setting_patch_fn patch, const struct azn_file_cue_callbacks *cb) {
    return build >= 1 && build <= 2 && patch && file_callbacks_valid(cb) && atomic_load_explicit(&(pack_profile_ready), memory_order_acquire);
}
static bool file_prologues_valid(int build) {
    const uint32_t first[][2] = {{0xa9bc7bfd, 0x910003fd}, {0xa9b87bfd, 0x910003fd}, {0xa9ba7bfd, 0xaa0003e7}, {build == 1 ? 0xa9b07bfdu : 0xa9b17bfdu, 0x910003fd}, {0xd10a83ff, 0xa9007bfd}, {0xa9aa7bfd, 0x910003fd}, {0xa9b47bfd, 0x910003fd}, {0xa9b97bfd, 0x910003fd}, {0xa9bc7bfd, 0x910003fd}, {0x39400022, 0x7103fc5f}, {0xa9bb7bfd, 0x910003fd}, {0xa9b97bfd, 0x910003fd}, {0xa9bb7bfd, 0x910003fd}};
    for (unsigned i = 0; i < 13; ++i)
        if (!file_original[i] && memcmp((void *) (file_bias + file_profile->methods[i]), first[i], sizeof first[i])) return false;
    return true;
}
static bool file_install_hooks(azn_setting_patch_fn patch) {
    void *hooks[] = {(void *) file_issue, (void *) file_serve, (void *) file_get, (void *) file_merge, (void *) file_ticket,
                     (void *) azn_file_decode_entry, (void *) file_append_0, (void *) file_reply, (void *) publish_import,
                     (void *) file_append_1, (void *) file_input_open, (void *) file_cache_get, (void *) file_fresh_load};
    for (unsigned i = 0; i < 13; ++i)
        if (!file_original[i] && (patch(file_bias + file_profile->methods[i], hooks[i], &file_original[i], "status-file-cue") != 1 || !file_original[i])) return false;
    return true;
}
#endif
static int file_install(int build, azn_setting_patch_fn patch, const struct azn_file_cue_callbacks *cb) {
#if defined(__aarch64__)
    if (!file_install_request(build, patch, cb)) return 0;
    if (profile.pack != (build == 1 ? UINT64_C(0x2612530) : UINT64_C(0x294c9b0)) + recall_bias) return 0;
    __auto_type installed = atomic_load_explicit(&(file_callbacks), memory_order_acquire);
    if (installed) return installed == cb;
    file_profile = &file_profiles[build - 1];
    file_bias = recall_bias;
    if (!file_prologues_valid(build) || !file_install_hooks(patch)) return 0;
    atomic_store_explicit(&(file_callbacks), cb, memory_order_release);
    return 1;
#else
    (void) build;
    (void) patch;
    (void) cb;
    return 0;
#endif
}
#include "sdk_status_file_maps.inc"
