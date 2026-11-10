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
static std::atomic<const azn_file_cue_callbacks *> file_callbacks {nullptr};
static std::atomic<azn_file_input_observer_fn> file_input_observer {nullptr};
static void observe_file_input(azn_file_input_observer_fn observer) {
    file_input_observer.store(observer, std::memory_order_release);
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
    uintptr_t object = 0, control = 0;
};
static bool file_atom_acquire(FileAtomOwner &out, uintptr_t object, uintptr_t control) {
    if (!object) return !control;
    if (!ptr(object) || !ptr(control)) return false;
    auto count = reinterpret_cast<int32_t *>(control + 8);
    int32_t value = __atomic_load_n(count, __ATOMIC_ACQUIRE);
    for (unsigned attempt = 0; attempt < 8 && value > 0 && value < INT32_MAX; ++attempt) {
        if (__atomic_compare_exchange_n(count, &value, value + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            out.object = object;
            out.control = control;
            return true;
        }
    }
    return false;
}
static void file_atom_release(FileAtomOwner &owner) {
    const uintptr_t control = owner.control;
    owner = {};
    if (!control || __atomic_sub_fetch(reinterpret_cast<int32_t *>(control + 8), 1, __ATOMIC_ACQ_REL)) return;
    reinterpret_cast<void (*)(uintptr_t)>(object_word(object_word(control) + 16))(control);
    if (!__atomic_sub_fetch(reinterpret_cast<int32_t *>(control + 12), 1, __ATOMIC_ACQ_REL))
        reinterpret_cast<void (*)(uintptr_t)>(object_word(object_word(control) + 24))(control);
}
#include "sdk_status_file_input.inc"
struct FileRead {
    uintptr_t accessor, identity;
    const azn_file_cue_callbacks *callbacks = nullptr;
    uint64_t token = 0;
    bool began = false, merged = false, bad = false, tag_seen = false, cached = false;
    FileAtomOwner fresh_inputs[4] {};
    unsigned fresh_count = 0;
    FileInputUse inputs;
    azn_file_cue_row atoms[AZN_FILE_CUE_MAX];
    FileAtomOwner owners[AZN_FILE_CUE_MAX][2];
    uint64_t reused[AZN_FILE_CUE_MAX] {};
    uint32_t lengths[AZN_FILE_CUE_MAX];
    unsigned count;
    ~FileRead() {
        if (token) {
            uint64_t ignored[AZN_FILE_CUE_MAX] {};
            callbacks->complete(token, nullptr, 0, ignored);
        }
        for (auto &pair : owners)
            for (auto &owner : pair) file_atom_release(owner);
        for (auto &input : fresh_inputs) file_atom_release(input);
    }
};
static thread_local FileServe *file_serving = nullptr;
static thread_local FileRead *file_reading = nullptr;
struct FileCacheCall {
    uintptr_t owner, extension, object = 0, control = 0;
    unsigned loads = 0;
};
static thread_local FileCacheCall *file_cache_call = nullptr;
static int file_fresh_load(uintptr_t owner, uintptr_t output, uintptr_t extension) {
    FileFreshInput input;
    RecallScope<FileFreshInput *> input_scope(file_fresh_input, &input);
    const int result = reinterpret_cast<int (*)(uintptr_t, uintptr_t, uintptr_t)>(file_original[12])(owner, output, extension);
    if (input.read) {
        const bool successful = !result && !input.bad && input.opens && input.opens <= 2 &&
            input.confirmed == input.opens && (input.mask == 1 || input.mask == 2) && output;
        uint64_t token = input.api->seal(input.read, successful);
        input.read = 0;
        file_input_remember(input.api, token, input.mask,
                            successful ? object_word(output) : 0, successful ? object_word(output + 8) : 0);
    }
    auto call = file_cache_call;
    if (call && call->owner == owner && call->extension == extension) {
        ++call->loads;
        if (!result && output) {
            call->object = object_word(output);
            call->control = object_word(output + 8);
        }
    }
    return result;
}
static int file_cache_get(uintptr_t owner, uintptr_t output, uintptr_t extension) {
    FileCacheCall call {};
    call.owner = owner;
    call.extension = extension;
    RecallScope<FileCacheCall *> scope(file_cache_call, &call);
    const int result = reinterpret_cast<int (*)(uintptr_t, uintptr_t, uintptr_t)>(file_original[11])(owner, output, extension);
    auto read = file_reading;
    if (read) read->tag_seen = true;
    if (result) return result;
    const uintptr_t object = output ? object_word(output) : 0;
    const uintptr_t control = output ? object_word(output + 8) : 0;
    if (file_map_inputs) file_map_inputs->observe(object, control);
    bool fresh = object && control && call.loads == 1 && call.object == object && call.control == control;
    if (read) {
        read->inputs.observe(object, control);
        bool retained = false;
        for (unsigned i = 0; i < read->fresh_count; ++i)
            retained |= read->fresh_inputs[i].object == object && read->fresh_inputs[i].control == control;
        if (fresh && !retained) {
            if (read->fresh_count == 4 || !file_atom_acquire(read->fresh_inputs[read->fresh_count], object, control))
                fresh = false;
            else ++read->fresh_count;
        }
        fresh |= retained;
        if (!fresh) read->cached = true;
    }
    if (!fresh) {
        const auto observer = file_input_observer.load(std::memory_order_acquire);
        if (observer) observer(AZN_INPUT_CACHED, -1);
    }
    return result;
}
struct FileOrigins {
    unsigned count = 0;
    FileAtomOwner owners[AZN_FILE_CUE_MAX][2] {};
    azn_file_cue_row atoms[AZN_FILE_CUE_MAX] {};
    uint64_t tokens[AZN_FILE_CUE_MAX] {};
};
static FileOrigins file_origins[16];
static unsigned file_origin_next;
static std::atomic_flag file_origin_busy = ATOMIC_FLAG_INIT;
static void release_origins(FileOrigins &origin) {
    const auto cb = file_callbacks.load(std::memory_order_acquire);
    if (origin.count && cb && cb->hold) cb->hold(origin.tokens, origin.count, 0);
    for (auto &pair : origin.owners)
        for (auto &owner : pair) file_atom_release(owner);
}
static uint64_t reused_origin(const FileRead &read, unsigned row) {
    if (file_origin_busy.test_and_set(std::memory_order_acquire)) return 0;
    uint64_t token = 0;
    for (const auto &origin : file_origins) {
        for (unsigned i = 0; i < origin.count; ++i) {
            if (!origin.tokens[i] || std::memcmp(origin.owners[i], read.owners[row], sizeof read.owners[row]) ||
                std::memcmp(&origin.atoms[i], &read.atoms[row], sizeof read.atoms[row])) continue;
            token = origin.tokens[i];
            break;
        }
        if (token) break;
    }
    file_origin_busy.clear(std::memory_order_release);
    return token;
}
static void remember_origins(const FileRead &read, const uint64_t *tokens) {
    FileOrigins next {};
    next.count = read.count;
    bool retained = false;
    for (unsigned i = 0; i < read.count; ++i) {
        if (!tokens[i]) continue;
        for (unsigned j = 0; j < 2; ++j) {
            if (!file_atom_acquire(next.owners[i][j], read.owners[i][j].object, read.owners[i][j].control)) {
                for (auto &pair : next.owners)
                    for (auto &owner : pair) file_atom_release(owner);
                return;
            }
        }
        next.atoms[i] = read.atoms[i];
        next.tokens[i] = tokens[i];
        retained = true;
    }
    if (!retained) return;
    if (read.callbacks->hold) read.callbacks->hold(next.tokens, next.count, 1);
    if (file_origin_busy.test_and_set(std::memory_order_acquire)) {
        release_origins(next);
        return;
    }
    auto &slot = file_origins[file_origin_next++ % 16];
    FileOrigins retired = slot;
    slot = next;
    file_origin_busy.clear(std::memory_order_release);
    release_origins(retired);
}
static bool original_atom(uintptr_t p, uintptr_t table, azn_file_atom &out) {
    if (!p) return true;
    if (!typed(p, file_bias + table) || !scalar<uint8_t>(p + 0x38)) return false;
    const unsigned header = scalar<uint32_t>(p + 0x18), total = scalar<uint32_t>(p + 0x1c);
    const uintptr_t data = object_word(p + 0x30);
    if (header < 12 || total < header || total <= 14 || total > AZN_FILE_CUE_BYTES ||
        object_word(p + 0x28) != total || !ptr(data)) return false;
    out.header = header;
    out.total = total;
    azsha256_hex(reinterpret_cast<const void *>(data), total - 14, out.digest);
    return true;
}
static bool merged_rows(uintptr_t vector, FileRead &read) {
    if (read.merged) {
        read.bad = true;
        return false;
    }
    read.merged = true;
    if (!ptr(vector)) return false;
    const uintptr_t b = object_word(vector), e = object_word(vector + 8);
    if (!ptr(b) || e < b || (e - b) % 8 || (e - b) / 8 > AZN_FILE_CUE_MAX) return false;
    unsigned count = unsigned((e - b) / 8);
    for (unsigned i = 0; i < count; ++i) {
        uintptr_t merged = object_word(b + i * 8);
        if (!ptr(merged)) return false;
        uint64_t bytes = object_word(merged + 0x10);
        if (bytes < 4 || bytes > AZN_FILE_CUE_BYTES ||
            !original_atom(object_word(merged + 0x30), file_profile->atoms[0], read.atoms[i].pcpt) ||
            !original_atom(object_word(merged + 0x40), file_profile->atoms[1], read.atoms[i].pcp2) ||
            (!read.atoms[i].pcpt.total && !read.atoms[i].pcp2.total)) return false;
        for (unsigned j = 0; j < 2; ++j) {
            if (!file_atom_acquire(read.owners[i][j], object_word(merged + 0x30 + j * 0x10),
                                   object_word(merged + 0x38 + j * 0x10))) return false;
        }
        read.reused[i] = reused_origin(read, i);
        read.lengths[i] = uint32_t(bytes);
    }
    read.count = count;
    return true;
}
static int file_merge(uintptr_t accessor, uintptr_t first, uintptr_t second, unsigned kind, uintptr_t output) {
    int result = reinterpret_cast<int (*)(uintptr_t, uintptr_t, uintptr_t, unsigned, uintptr_t)>(file_original[3])(
        accessor, first, second, kind, output);
    auto read = file_reading;
    if (!result && read && read->accessor == accessor && kind == 1 && output &&
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == file_bias + file_profile->merge_return)
        merged_rows(object_word(output), *read);
    return result;
}
static bool wire_rows(const FileRead &read, uintptr_t block, FileServe &serve) {
    if (!ptr(block) || !read.count) return false;
    const uintptr_t start = object_word(block);
    const uint64_t bytes = object_word(block + 8);
    if (!ptr(start) || bytes > AZN_FILE_CUE_MAX * AZN_FILE_CUE_BYTES) return false;
    size_t offset = 0;
    for (unsigned i = 0; i < read.count; ++i) {
        unsigned size = read.lengths[i];
        if (offset > bytes || size > bytes - offset || size < 4 || scalar<uint32_t>(start + offset) != size) return false;
        serve.rows[i].bytes = size - 4;
        azsha256_hex(reinterpret_cast<const void *>(start + offset + 4), size - 4, serve.rows[i].digest);
        offset += size;
    }
    return offset == bytes;
}
static unsigned file_input_open(uintptr_t self, uintptr_t path, unsigned flags, int *error) {
    auto read = file_reading;
    const auto observer = file_input_observer.load(std::memory_order_acquire);
    const bool nominal = typed(self, file_bias + file_profile->input_table);
    auto fresh = file_fresh_input;
    if (nominal && fresh) file_input_start(*fresh, self, path);
    else if (fresh) fresh->bad = true;
    if (nominal && observer) observer(AZN_INPUT_OPENING, -1);
    if (read && !read->inputs.api && nominal && !read->began) {
        read->began = true;
        const uintptr_t storage = object_word(read->accessor + 0x18), identity = read->identity;
        if (ptr(storage))
            read->token = read->callbacks->begin(
                reinterpret_cast<const char *>(object_word(storage + 8)), object_word(storage + 16),
                reinterpret_cast<const uint16_t *>(object_word(identity)), object_word(identity + 8),
                reinterpret_cast<const uint16_t *>(object_word(identity + 32)), object_word(identity + 40));
    }
    const unsigned result = reinterpret_cast<unsigned (*)(uintptr_t, uintptr_t, unsigned, int *)>(file_original[10])(
        self, path, flags, error);
    if (!result && nominal) {
        /* CFpHandle::open called fopen64 in this invocation and committed the
         * new FILE before returning. The callback borrows its actual fd. */
        auto stream = reinterpret_cast<FILE *>(object_word(self + 0x28));
        if (stream) {
            const int descriptor = fileno(stream);
            if (fresh && fresh->read) {
                unsigned mask = fresh->api->opened(fresh->read, descriptor);
                if ((mask != 1 && mask != 2) || (fresh->mask && fresh->mask != mask)) fresh->bad = true;
                else { fresh->mask = mask; ++fresh->confirmed; }
            }
            if (read && read->token) read->callbacks->opened(read->token, descriptor);
            if (observer) observer(AZN_INPUT_OPENED, descriptor);
        }
    }
    if (fresh && result) fresh->bad = true;
    return result;
}
static int file_get(uintptr_t accessor, uintptr_t identity, uintptr_t output, uintptr_t count) {
    const auto cb = file_callbacks.load(std::memory_order_acquire);
    auto serve = file_serving;
    const bool eligible = cb && serve && serve->identity == identity && ptr(accessor) && ptr(identity) && output && count &&
                          reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == file_bias + file_profile->getter_return;
    FileRead read {};
    read.accessor = accessor;
    read.identity = identity;
    read.callbacks = cb;
    RecallScope<FileRead *> scope(file_reading, eligible ? &read : nullptr);
    int result = reinterpret_cast<int (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(file_original[2])(accessor, identity, output, count);
    if (!result && eligible && !read.bad && read.count == object_word(count) && wire_rows(read, object_word(output), *serve)) {
        uint64_t ids[AZN_FILE_CUE_MAX] = {};
        if (read.inputs.api) read.token = read.inputs.join();
        if (read.token) {
            const bool fresh = read.inputs.api ? read.inputs.seen && !read.inputs.bad : read.tag_seen && !read.cached;
            cb->complete(read.token, fresh ? read.atoms : nullptr, fresh ? read.count : 0, ids);
            read.token = 0;
            remember_origins(read, ids);
        }
        serve->n = read.count;
        for (unsigned i = 0; i < read.count; ++i)
            serve->rows[i].token = ids[i] ? ids[i] : read.reused[i];
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
    uint32_t seq = 0;
    unsigned phase = 0, count = 0;
    uintptr_t issuer = 0, query = 0, request = 0, response = 0, delivered = 0;
    FileWireRow rows[AZN_FILE_CUE_MAX] {};
};
struct FileGuard {
    static std::atomic_flag busy;
    static std::atomic<uint64_t> lost;
    bool held;
    FileGuard(uint32_t seq) : held(!busy.test_and_set(std::memory_order_acquire)) {
        if (!held) lost.fetch_or(UINT64_C(1) << (seq % 32), std::memory_order_release);
    }
    ~FileGuard() {
        if (held) busy.clear(std::memory_order_release);
    }
};
std::atomic_flag FileGuard::busy = ATOMIC_FLAG_INIT;
std::atomic<uint64_t> FileGuard::lost {0};
static FileRoute file_routes[32];
static uint32_t highest_file_request;
static bool file_sequence_wrapped;
static void file_prune() {
    uint64_t lost = FileGuard::lost.exchange(0, std::memory_order_acq_rel);
    while (lost) {
        unsigned i = unsigned(__builtin_ctzll(lost));
        file_routes[i].phase = 0;
        lost &= lost - 1;
    }
}
static unsigned message_kind(uintptr_t p) {
    if (!ptr(p) || scalar<uint32_t>(p + 4) != UINT32_C(0x872349ae)) return 0;
    return scalar<uint16_t>(p);
}
static void file_map_transport(unsigned stage, uintptr_t packet, unsigned peer);
static void file_map_reply(uintptr_t server, uintptr_t packet, unsigned peer);
static uintptr_t file_issue(uintptr_t ticket) {
    uintptr_t query = reinterpret_cast<uintptr_t (*)(uintptr_t)>(file_original[0])(ticket);
    if (!file_callbacks.load(std::memory_order_acquire) || message_kind(query) != 0x2b04) return query;
    uint32_t seq = scalar<uint32_t>(query + 8);
    if (!seq || scalar<uint32_t>(ticket + 0x20) != seq || scalar<uint16_t>(ticket + 0x1a) != 0x4e02 ||
        scalar<uint16_t>(ticket + 0x1c) != 0x2b04) return query;
    FileGuard guard(seq);
    if (guard.held) {
        file_prune();
        auto &r = file_routes[seq % 32];
        if (highest_file_request > seq && highest_file_request - seq > INT32_MAX) file_sequence_wrapped = true;
        if (r.seq == seq || file_sequence_wrapped) r.phase = 0;
        else if (seq > highest_file_request) {
            highest_file_request = seq;
            r = FileRoute {};
            r.seq = seq;
            r.phase = FileIssued;
            r.query = query;
            r.issuer = ticket;
        }
    }
    return query;
}
static void file_transport(unsigned stage, uintptr_t packet, unsigned peer) {
    file_map_transport(stage, packet, peer);
    if (peer || !file_callbacks.load(std::memory_order_acquire)) return;
    unsigned kind = message_kind(packet);
    if (kind != 0x2b04 && kind != 0x4e02) return;
    uint32_t seq = scalar<uint32_t>(packet + 8);
    FileGuard guard(seq);
    if (!guard.held) return;
    file_prune();
    auto &r = file_routes[seq % 32];
    if (!seq || r.seq != seq || !r.phase) return;
    if (kind == 0x2b04 && stage == AZN_FILE_SEND && r.phase == FileIssued && packet == r.query) r.phase = FileSent;
    else if (kind == 0x2b04 && stage == AZN_FILE_WIRE && r.phase == FileSent && packet == r.query) r.phase = FileWire;
    else if (kind == 0x2b04 && stage == AZN_FILE_SERVER && r.phase == FileWire) {
        r.request = packet;
        r.phase = FileReceived;
    } else if (kind == 0x4e02 && stage == AZN_FILE_SEND && r.phase == FileServed && packet == r.response) r.phase = FileResponse;
    else if (kind == 0x4e02 && stage == AZN_FILE_WIRE && r.phase == FileResponse && packet == r.response) r.phase = FileResponseWire;
    else if (kind == 0x4e02 && stage == AZN_FILE_CLIENT && r.phase == FileResponseWire) {
        r.delivered = packet;
        r.phase = FileDelivered;
    }
    /* Other registered listeners see the same decoded packet before the
     * accepting ticket. A second packet with this sequence is ambiguous. */
    else if (kind == 0x4e02 && stage == AZN_FILE_CLIENT && r.phase == FileDelivered && r.delivered == packet)
        return;
    else if ((kind == 0x2b04 && stage != AZN_FILE_CLIENT) || (kind == 0x4e02 && stage != AZN_FILE_SERVER)) r.phase = 0;
}
static void file_serve(uintptr_t server, uintptr_t request_ref, unsigned peer, uintptr_t identity) {
    FileServe serve {};
    if (file_callbacks.load(std::memory_order_acquire) && !peer && ptr(request_ref)) {
        uintptr_t request = object_word(request_ref);
        if (message_kind(request) == 0x2b04) {
            uint32_t seq = scalar<uint32_t>(request + 8);
            FileGuard guard(seq);
            if (guard.held) {
                file_prune();
                auto &r = file_routes[seq % 32];
                if (r.seq == seq && r.phase == FileReceived && r.request == request) {
                    serve.server = server;
                    serve.request = request;
                    serve.identity = identity;
                    serve.seq = seq;
                }
            }
        }
    }
    RecallScope<FileServe *> scope(file_serving, serve.seq ? &serve : nullptr);
    reinterpret_cast<void (*)(uintptr_t, uintptr_t, unsigned, uintptr_t)>(file_original[1])(server, request_ref, peer, identity);
}
static uintptr_t file_reply(uintptr_t server, uintptr_t packet, unsigned peer) {
    file_map_reply(server, packet, peer);
    auto serve = file_serving;
    if (!peer && serve && serve->server == server && serve->n && message_kind(packet) == 0x4e02 &&
        scalar<uint32_t>(packet + 8) == serve->seq) {
        FileGuard guard(serve->seq);
        if (guard.held) {
            file_prune();
            auto &r = file_routes[serve->seq % 32];
            if (r.seq == serve->seq && r.phase == FileReceived && r.request == serve->request) {
                r.phase = FileServed;
                r.response = packet;
                r.count = serve->n;
                std::memcpy(r.rows, serve->rows, serve->n * sizeof *r.rows);
            }
        }
    }
    return reinterpret_cast<uintptr_t (*)(uintptr_t, uintptr_t, unsigned)>(file_original[7])(server, packet, peer);
}
/* MemoryCueSet rejects duplicates by these six fields before insertion. Its
 * sort moves rows in this same counted Content. Compare this key only inside
 * the witnessed source object, after the original append succeeds. */
struct FileKey {
    uint32_t kind, rate;
    uint64_t clocks[4];
};
static FileKey file_key(uintptr_t row) {
    FileKey k {};
    k.kind = scalar<uint8_t>(row);
    k.rate = scalar<uint32_t>(row + 4);
    std::memcpy(k.clocks, reinterpret_cast<const void *>(row + 8), sizeof k.clocks);
    return k;
}
static bool same_file_key(const FileKey &a, const FileKey &b) {
    if (a.kind != b.kind || a.rate != b.rate || !a.rate || a.rate >= INT32_MAX) return false;
    const uint64_t absent = UINT64_C(0x7fffffffffffffff);
    for (unsigned i = 0; i < 4; ++i) {
        if (a.clocks[i] == absent || b.clocks[i] == absent) {
            if (a.clocks[i] != absent || b.clocks[i] != absent) return false;
        } else {
            const int order = compare_timestamps(a.clocks[i], b.clocks[i], i % 2 ? a.rate : 1000000u, a.rate, 1);
            if (order == AZ_TIME_INVALID || order != AZ_TIME_EQUAL) return false;
        }
    }
    return true;
}
struct FileBank {
    unsigned added = 0;
    uintptr_t content = 0;
    uint64_t pin = 0;
    struct Added {
        FileKey key {};
        uint64_t token = 0;
    } row[10];
    ~FileBank() {
        if (pin) azn_pin_drop(pin);
    }
};
struct FileImport {
    unsigned count = 0, decoded = 0;
    bool bad = false;
    FileWireRow wire[AZN_FILE_CUE_MAX] {};
    uintptr_t ticket = 0, decoded_row = 0;
    uint64_t token = 0;
    FileBank bank[2];
};
static thread_local FileImport *file_import = nullptr;
struct FileContent {
    uint64_t pin = 0, token[10] {};
};
static std::atomic_flag file_content_busy = ATOMIC_FLAG_INIT;
static FileContent imported_files[16];
static unsigned file_content_next;
static void hold_file_content(const FileContent &content, int acquire) {
    const auto cb = file_callbacks.load(std::memory_order_acquire);
    if (!cb || cb->size < offsetof(azn_file_cue_callbacks, hold) + sizeof cb->hold || !cb->hold) return;
    uint64_t tokens[10];
    size_t n = 0;
    for (uint64_t token : content.token)
        if (token) tokens[n++] = token;
    if (n) cb->hold(tokens, n, acquire);
}
static void finish_bank(FileImport &import, unsigned hot) {
    auto &f = import.bank[hot];
    const uintptr_t table = hot ? file_bias + file_profile->hot_content : profile.content;
    if (!f.pin || !f.added || !typed(f.content, table)) return;
    const uintptr_t b = object_word(f.content + 0x28), e = object_word(f.content + 0x30);
    if (!ptr(b) || e < b || (e - b) % 0x80 || (e - b) / 0x80 > (hot ? 8u : 10u)) return;
    FileContent next {};
    for (unsigned i = 0; i < f.added; ++i) {
        unsigned index = 0, occurrences = 0;
        for (unsigned j = 0; j < (e - b) / 0x80; ++j) {
            const auto k = file_key(b + j * 0x80);
            if (same_file_key(k, f.row[i].key)) {
                index = j;
                ++occurrences;
            }
        }
        unsigned key = hot ? f.row[i].key.kind : index + 1;
        if (occurrences == 1 && key && key <= (hot ? 8u : 10u) && !next.token[key - 1])
            next.token[key - 1] = f.row[i].token;
    }
    next.pin = f.pin;
    hold_file_content(next, 1);
    if (file_content_busy.test_and_set(std::memory_order_acquire)) {
        hold_file_content(next, 0);
        return;
    }
    f.pin = 0;
    FileContent &slot = imported_files[(file_content_next++) % 16];
    FileContent retired = slot;
    slot = next;
    file_content_busy.clear(std::memory_order_release);
    if (retired.pin) {
        hold_file_content(retired, 0);
        azn_pin_drop(retired.pin);
    }
}
static void finish_import(FileImport &f) {
    if (f.bad || f.count != f.decoded) return;
    finish_bank(f, 0);
    finish_bank(f, 1);
}
static uint64_t imported_memory_row(uintptr_t content, unsigned entry) {
    if (!entry || entry > 10 || file_content_busy.test_and_set(std::memory_order_acquire)) return 0;
    uint64_t result = 0;
    for (const auto &f : imported_files) {
        if (!f.pin || !f.token[entry - 1]) continue;
        Hold ref(f.pin);
        if (ref.token && ref.get()->object == content) {
            result = f.token[entry - 1];
            break;
        }
    }
    file_content_busy.clear(std::memory_order_release);
    return result;
}
static uintptr_t file_ticket(uintptr_t ticket) {
    FileImport f {};
    uintptr_t packet = object_word(ticket + 0x28);
    if (file_callbacks.load(std::memory_order_acquire) && message_kind(packet) == 0x4e02 &&
        scalar<uint16_t>(ticket + 0x1a) == 0x4e02 && scalar<uint16_t>(ticket + 0x1c) == 0x2b04) {
        uint32_t seq = scalar<uint32_t>(packet + 8);
        FileGuard guard(seq);
        if (guard.held) {
            file_prune();
            auto &r = file_routes[seq % 32];
            if (r.seq == seq && r.phase == FileDelivered && r.issuer == ticket && r.delivered == packet &&
                scalar<uint32_t>(ticket + 0x20) == seq) {
                f.ticket = ticket;
                f.count = r.count;
                std::memcpy(f.wire, r.rows, r.count * sizeof *r.rows);
                r.phase = 0;
            }
        }
    }
    RecallScope<FileImport *> scope(file_import, f.count ? &f : nullptr);
    return reinterpret_cast<uintptr_t (*)(uintptr_t)>(file_original[4])(ticket);
}
static void publish_import(uintptr_t ticket, uintptr_t memory, uintptr_t hot, uintptr_t result) {
    auto f = file_import;
    if (f && f->ticket == ticket && result && scalar<unsigned>(result) == 1 && object_word(ticket + 0x70) &&
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == file_bias + file_profile->finish_return)
        finish_import(*f);
    reinterpret_cast<void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t)>(file_original[8])(ticket, memory, hot, result);
}
#if defined(__aarch64__)
extern "C" void azn_file_decode_entry();
extern "C" void azn_invoke_file_decode(void *target, uintptr_t block, unsigned rate, uintptr_t row);
__asm__(".text\n.p2align 2\n.hidden azn_file_decode_entry\n.type azn_file_decode_entry,%function\n"
        "azn_file_decode_entry:\nmov x2,x8\nb azn_file_decode\n.size azn_file_decode_entry,.-azn_file_decode_entry\n"
        ".hidden azn_invoke_file_decode\n.type azn_invoke_file_decode,%function\n"
        "azn_invoke_file_decode:\nmov x4,x0\nmov x0,x1\nmov w1,w2\nmov x8,x3\nbr x4\n"
        ".size azn_invoke_file_decode,.-azn_invoke_file_decode\n");
extern "C" __attribute__((visibility("hidden"))) void azn_file_decode(uintptr_t block, unsigned rate, uintptr_t row) {
    auto f = file_import;
    if (f && !f->bad) {
        f->decoded_row = 0;
        f->token = 0;
        if (reinterpret_cast<uintptr_t>(__builtin_return_address(0)) != file_bias + file_profile->decoder_return ||
            f->decoded >= f->count || !ptr(block) || !ptr(row)) f->bad = true;
        else {
            const auto &expected = f->wire[f->decoded++];
            uintptr_t data = object_word(block);
            size_t n = object_word(block + 8);
            char digest[65] = {};
            if (ptr(data) && n == expected.bytes && n <= AZN_FILE_CUE_BYTES)
                azsha256_hex(reinterpret_cast<const void *>(data), n, digest);
            if (std::memcmp(digest, expected.digest, sizeof digest)) f->bad = true;
            else {
                f->decoded_row = row;
                f->token = expected.token;
            }
        }
    }
    azn_invoke_file_decode(file_original[5], block, rate, row);
}
#endif
template <unsigned Hot>
static unsigned file_append(uintptr_t content, uintptr_t row) {
    auto f = file_import;
    const uintptr_t table = Hot ? file_bias + file_profile->hot_content : profile.content;
    bool from_file = f && !f->bad && f->token && row == f->decoded_row &&
                     reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == file_bias + (Hot ? file_profile->hot_return : file_profile->append_return) &&
                     typed(content, table) && (Hot ? scalar<uint8_t>(row) >= 1 && scalar<uint8_t>(row) <= 8 : !scalar<uint8_t>(row));
    unsigned accepted = reinterpret_cast<unsigned (*)(uintptr_t, uintptr_t)>(file_original[Hot ? 9 : 6])(content, row);
    if (from_file && accepted) {
        auto &bank = f->bank[Hot];
        if (!bank.pin) {
            bank.pin = azn_owned_pin(content);
            bank.content = content;
        }
        if (!bank.pin || bank.content != content || bank.added >= (Hot ? 8u : 10u)) f->bad = true;
        else {
            auto &a = bank.row[bank.added++];
            a.key = file_key(row);
            a.token = f->token;
        }
        f->decoded_row = 0;
        f->token = 0;
    }
    return accepted;
}
static int file_install(int build, azn_setting_patch_fn patch, const azn_file_cue_callbacks *cb) {
#if defined(__aarch64__)
    if (build < 1 || build > 2 || !patch || !cb || cb->version != 2 ||
        cb->size < offsetof(azn_file_cue_callbacks, opened) + sizeof cb->opened ||
        !cb->begin || !cb->complete || !cb->hold || !cb->opened || !pack_profile_ready.load(std::memory_order_acquire)) return 0;
    const auto installed = file_callbacks.load(std::memory_order_acquire);
    if (installed) return installed == cb;
    file_profile = &file_profiles[build - 1];
    file_bias = recall_bias;
    if (profile.pack != (build == 1 ? UINT64_C(0x2612530) : UINT64_C(0x294c9b0)) + file_bias) return 0;
    const uint32_t first[][2] = {{0xa9bc7bfd, 0x910003fd}, {0xa9b87bfd, 0x910003fd}, {0xa9ba7bfd, 0xaa0003e7}, {build == 1 ? 0xa9b07bfdu : 0xa9b17bfdu, 0x910003fd}, {0xd10a83ff, 0xa9007bfd}, {0xa9aa7bfd, 0x910003fd}, {0xa9b47bfd, 0x910003fd}, {0xa9b97bfd, 0x910003fd}, {0xa9bc7bfd, 0x910003fd}, {0x39400022, 0x7103fc5f}, {0xa9bb7bfd, 0x910003fd}, {0xa9b97bfd, 0x910003fd}, {0xa9bb7bfd, 0x910003fd}};
    void *hooks[] = {reinterpret_cast<void *>(file_issue), reinterpret_cast<void *>(file_serve),
                     reinterpret_cast<void *>(file_get), reinterpret_cast<void *>(file_merge), reinterpret_cast<void *>(file_ticket),
                     reinterpret_cast<void *>(azn_file_decode_entry), reinterpret_cast<void *>(file_append<0>), reinterpret_cast<void *>(file_reply), reinterpret_cast<void *>(publish_import), reinterpret_cast<void *>(file_append<1>), reinterpret_cast<void *>(file_input_open), reinterpret_cast<void *>(file_cache_get), reinterpret_cast<void *>(file_fresh_load)};
    for (unsigned i = 0; i < 13; ++i)
        if (!file_original[i] &&
            std::memcmp(reinterpret_cast<void *>(file_bias + file_profile->methods[i]), first[i], sizeof first[i])) return 0;
    for (unsigned i = 0; i < 13; ++i)
        if (!file_original[i] &&
            (patch(file_bias + file_profile->methods[i], hooks[i], &file_original[i], "status-file-cue") != 1 || !file_original[i])) return 0;
    file_callbacks.store(cb, std::memory_order_release);
    return 1;
#else
    (void) build;
    (void) patch;
    (void) cb;
    return 0;
#endif
}
#include "sdk_status_file_maps.inc"
