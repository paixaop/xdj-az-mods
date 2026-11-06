/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azcuepersist.h"
#include "../../src/azmod/azmod-mod.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <time.h>
#include <thread>
#include <vector>

namespace {
struct alignas(8) Bytes {
    unsigned char bytes[0x200];
};
Bytes maintainer = {}, queue = {}, property = {}, handler = {}, receiver = {}, payload = {}, foreign_receiver = {};
void (*request_entry)(void *, const void *);
bool (*handler_entry)(void *);
void (*reply_entry)(void *, const void *, const void *, const void *, const void *, const int *);
void (*registered_entry)(void *);
uintptr_t handler_table[4] = {}, receiver_table[3] = {};
unsigned producer_calls, request_calls, handler_calls, task_calls, ready_calls, confirm_calls, allocations, destructions;
bool identity_valid = true, effect_confirmed = true, invalidate_in_producer = false, publication_checks = false;
std::string observed_comment;
uint32_t observed_color;
unsigned char observed_auto;
void setword(void *base, unsigned offset, uintptr_t word) { std::memcpy(static_cast<unsigned char *>(base) + offset, &word, sizeof word); }
uintptr_t getword(void *base, unsigned offset) {
    uintptr_t word;
    std::memcpy(&word, static_cast<unsigned char *>(base) + offset, sizeof word);
    return word;
}
Bytes source_root = {}, memory_set = {}, hot_set = {};
using Helper = int (*)(void *, const void *, const void *, const void *, const void *, unsigned);
Helper memory_entry, hot_entry;
unsigned helper_calls = 0, publications = 0, ref_copies = 0, ref_releases = 0;
std::vector<unsigned char *> owned_contents;
Bytes helper_request = {}, helper_cue = {};
bool helper_pending = false, helper_cue_owned = false;
unsigned current_operation = 0;
int64_t locator_shift_us = 0;
void copy_row(void *out, const void *in) {
    std::memcpy(out, in, 0x80);
    const char *text = reinterpret_cast<const char *>(getword(const_cast<void *>(in), 0x70));
    setword(out, 0x70, reinterpret_cast<uintptr_t>(::strdup(text ? text : "")));
}
void destroy_row(void *row) {
    std::free(reinterpret_cast<void *>(getword(row, 0x70)));
    std::memset(row, 0, 0x80);
}
void copy_ref(void *out, const void *in) {
    std::memcpy(out, in, 8);
    if (getword(out, 0)) ++ref_copies;
}
void release_ref(void *p) {
    if (getword(p, 0)) ++ref_releases;
    setword(p, 0, 0);
}
void vector_append(void *p, const void *row) {
    uintptr_t begin = getword(p, 0), end = getword(p, 8);
    size_t length = end - begin;
    void *next = std::realloc(reinterpret_cast<void *>(begin), length + 0x80);
    assert(next);
    setword(p, 0, reinterpret_cast<uintptr_t>(next));
    setword(p, 8, reinterpret_cast<uintptr_t>(next) + length + 0x80);
    setword(p, 16, reinterpret_cast<uintptr_t>(next) + length + 0x80);
    copy_row(static_cast<char *>(next) + length, row);
}
void vector_destroy(void *p) {
    uintptr_t begin = getword(p, 0), end = getword(p, 8);
    for (uintptr_t at = begin; at != end; at += 0x80) std::free(reinterpret_cast<void *>(getword(reinterpret_cast<void *>(at), 0x70)));
    std::free(reinterpret_cast<void *>(begin));
    std::memset(p, 0, 24);
}
void from_vector(void *out, const void *v) {
    ++ref_copies;
    auto *content = new unsigned char[0x50]();
    uintptr_t vector[3] = {};
    uintptr_t begin = getword(const_cast<void *>(v), 0), end = getword(const_cast<void *>(v), 8);
    for (uintptr_t at = begin; at != end; at += 0x80) vector_append(vector, reinterpret_cast<void *>(at));
    std::memcpy(content + 0x28, vector, 24);
    owned_contents.push_back(content);
    setword(out, 0, reinterpret_cast<uintptr_t>(content));
}
bool content_publish(void *set, const void *ref, void *listener) {
    assert(!listener);
    ++publications;
    ++ref_copies;
    ++ref_releases;
    setword(set, 0x28, getword(const_cast<void *>(ref), 0));
    return true;
}
void reply(int result);
bool receipt_in_helper = false, hot_fresh = false, invalidate_reply = false, empty_reply_roots = false, rebuild_reply = false;
int expected_publication = 1;
int native_memory(void *, const void *, const void *, const void *, const void *, unsigned) {
    ++helper_calls;
    if (receipt_in_helper) {
        unsigned before = ready_calls;
        reply(0);
        registered_entry(payload.bytes);
        assert(ready_calls == before);
    }
    return 1;
}
int native_hot(void *, const void *, const void *, const void *, const void *, unsigned) {
    ++helper_calls;
    return hot_fresh ? 0 : 1;
}
uint64_t full_hash(const azcuepersist_witness &w) {
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&](const void *p, size_t n) {const auto*b=static_cast<const unsigned char *>(p);for(size_t i=0;i<n;++i)hash=(hash^b[i])*1099511628211ULL; };
    mix(w.source_id, 12);
    uintptr_t mb = getword(reinterpret_cast<void *>(w.memory_content), 0x28), me = getword(reinterpret_cast<void *>(w.memory_content), 0x30);
    uintptr_t hb = getword(reinterpret_cast<void *>(w.hot_content), 0x28), he = getword(reinterpret_cast<void *>(w.hot_content), 0x30);
    uint64_t count = (me - mb) / 0x80;
    mix(&count, 8);
    mix(reinterpret_cast<void *>(mb), me - mb);
    mix(reinterpret_cast<void *>(hb), he - hb);
    return hash;
}
azcuepersist_witness witness() {
    azcuepersist_witness w = {};
    w.source_info = reinterpret_cast<uintptr_t>(source_root.bytes);
    w.memory_set = reinterpret_cast<uintptr_t>(memory_set.bytes);
    w.hot_set = reinterpret_cast<uintptr_t>(hot_set.bytes);
    w.memory_content = getword(memory_set.bytes, 0x28);
    w.hot_content = getword(hot_set.bytes, 0x28);
    w.revision = 1;
    for (unsigned i = 0; i < 12; ++i) w.source_id[i] = i + 1;
    w.fingerprint = full_hash(w);
    return w;
}
int submit(const azexit_cue_request *r, void *m, void *n, unsigned kind, const void *p, uint64_t cookie) {
    auto w = witness();
    current_operation = r->operation;
    uintptr_t content = kind ? w.hot_content : w.memory_content;
    Bytes locator = {};
    copy_row(locator.bytes, reinterpret_cast<void *>(getword(reinterpret_cast<void *>(content), 0x28) + (kind ? kind - 1 : 0) * 0x80));
    setword(locator.bytes, 8, getword(locator.bytes, 8) + locator_shift_us);
    w.selected_cue = reinterpret_cast<uintptr_t>(locator.bytes);
    int result = azsdk_cue_persist_submit(r, m, n, kind, p, cookie, &w);
    destroy_row(locator.bytes);
    return result;
}
void run_helper(int expected_result = 0) {
    if (!helper_pending) return;
    helper_pending = false;
    unsigned kind = handler.bytes[0x38];
    uintptr_t set = reinterpret_cast<uintptr_t>(kind ? hot_set.bytes : memory_set.bytes);
    int result = -1;
    std::thread worker([&] { result = (kind ? hot_entry : memory_entry)(nullptr, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, &set, 1); });
    worker.join();
    assert(result == expected_result);
}
void init_collections() {
    for (unsigned hot = 0; hot < 2; ++hot) {
        uintptr_t v[3] = {};
        for (unsigned i = 0; i < (hot ? 8u : 10u); ++i) {
            Bytes r = {};
            r.bytes[0] = hot ? i + 1 : 0;
            uint32_t rate = 44100;
            std::memcpy(r.bytes + 4, &rate, 4);
            r.bytes[0x7c] = 3;
            setword(r.bytes, 8, 1000 + i);
            setword(r.bytes, 16, 100 + i);
            setword(r.bytes, 24, 2000 + i);
            setword(r.bytes, 32, 200 + i);
            setword(r.bytes, 0x70, reinterpret_cast<uintptr_t>("preserved"));
            vector_append(v, r.bytes);
        }
        uintptr_t content = 0;
        from_vector(&content, v);
        vector_destroy(v);
        setword(hot ? hot_set.bytes : memory_set.bytes, 0x28, content);
    }
}
uint64_t deadline() {
    timespec t;
    assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec + 10000000000;
}
struct NativeString {
    void *data;
    ~NativeString() {}
};
NativeString allocate(uint64_t size) {
    ++allocations;
    NativeString text = {std::malloc(size)};
    assert(text.data);
    return text;
}
void assign(void *dest, const void *source) {
    void *pointer;
    std::memcpy(&pointer, source, sizeof pointer);
    std::free(reinterpret_cast<void *>(getword(dest, 0)));
    char *copy = ::strdup(static_cast<const char *>(pointer));
    assert(copy);
    setword(dest, 0, reinterpret_cast<uintptr_t>(copy));
}
void destroy(void *string) {
    ++destructions;
    std::free(reinterpret_cast<void *>(getword(string, 0)));
}
void color(void *dest, unsigned r, unsigned g, unsigned b) {
    uint32_t rgb = 0xff000000u | (r << 16) | (g << 8) | b;
    std::memcpy(dest, &rgb, 4);
}
bool native_handler(void *self) {
    assert(self == handler.bytes);
    ++handler_calls;
    observed_comment = static_cast<const char *>(reinterpret_cast<void *>(getword(self, 0xa8)));
    std::memcpy(&observed_color, static_cast<unsigned char *>(self) + 0xb0, 4);
    observed_auto = static_cast<unsigned char *>(self)[0x60];
    return true; /* admission is deliberately not completion */
}
void native_request(void *owner, const void *record) {
    assert(owner == maintainer.bytes);
    ++request_calls;
    std::memset(handler.bytes, 0, sizeof handler.bytes);
    std::memset(receiver.bytes, 0, sizeof receiver.bytes);
    setword(handler.bytes, 0, reinterpret_cast<uintptr_t>(handler_table));
    setword(receiver.bytes, 0, reinterpret_cast<uintptr_t>(receiver_table));
    setword(receiver.bytes, 0x10, reinterpret_cast<uintptr_t>(owner));
    uint32_t tag = 44;
    std::memcpy(handler.bytes + 8, &tag, 4);
    std::memcpy(receiver.bytes + 8, &tag, 4);
    for (unsigned i = 0; i < 16; ++i) handler.bytes[0x10 + i] = static_cast<unsigned char>(i + 1);
    setword(handler.bytes, 0xc0, reinterpret_cast<uintptr_t>(receiver.bytes));
    setword(handler.bytes, 0xa8, reinterpret_cast<uintptr_t>(::strdup("preserved")));
    setword(owner, 0x160, reinterpret_cast<uintptr_t>(handler.bytes));
    unsigned kind = static_cast<const unsigned char *>(record)[4];
    uintptr_t c = getword(kind ? hot_set.bytes : memory_set.bytes, 0x28), row = getword(reinterpret_cast<void *>(c), 0x28) + (kind ? kind - 1 : 0) * 0x80;
    std::free(reinterpret_cast<void *>(getword(handler.bytes, 0xa8)));
    copy_row(handler.bytes + 0x38, reinterpret_cast<void *>(row));
    uint32_t serial = 1234;
    std::memcpy(handler.bytes + 0x30, &serial, 4);
    std::memcpy(helper_request.bytes + 8, &serial, 4);
    if (current_operation == AZ_CMD_SAVED_AUTO || current_operation == AZ_CMD_CUE_COMMENT || current_operation == AZ_CMD_CUE_COLOR || current_operation == AZ_CMD_HOT_SET) {
        /* The firmware's Property conversion loses unrequested stored fields. */
        destroy_row(handler.bytes + 0x38);
        copy_row(handler.bytes + 0x38, reinterpret_cast<void *>(row));
        std::free(reinterpret_cast<void *>(getword(handler.bytes, 0xa8)));
        setword(handler.bytes, 0xa8, reinterpret_cast<uintptr_t>(::strdup("")));
        uint32_t zero = 0;
        if (current_operation != AZ_CMD_HOT_SET) std::memcpy(handler.bytes + 0x3c, &zero, 4);
        std::memcpy(handler.bytes + 0xb0, &zero, 4);
        handler.bytes[0x60] = 0;
        handler.bytes[0xb4] = 0;
    }
    assert(handler_entry(handler.bytes));
    if (helper_cue_owned) destroy_row(helper_cue.bytes);
    copy_row(helper_cue.bytes, handler.bytes + 0x38);
    helper_cue_owned = true;
    helper_pending = current_operation == AZ_CMD_CUE_COMMENT || current_operation == AZ_CMD_CUE_COLOR || current_operation == AZ_CMD_SAVED_AUTO || current_operation == AZ_CMD_HOT_SET;
}
void produce(void *owner, unsigned kind, const void *prop, bool save) {
    ++producer_calls;
    alignas(8) unsigned char record[0x108] = {};
    record[0] = save;
    std::memcpy(record + 4, &kind, 4);
    std::memcpy(record + 8, prop, 0xf8);
    if (invalidate_in_producer) identity_valid = false;
    request_entry(owner, record);
}
void producer(void *owner, unsigned kind, const void *prop) { produce(owner, kind, prop, true); }
void erase(void *owner, unsigned kind, const void *prop) { produce(owner, kind, prop, false); }
void native_reply(void *self, const void *, const void *track, const void *, const void *, const int *result) {
    std::memset(payload.bytes, 0, sizeof payload.bytes);
    setword(payload.bytes, 0, getword(self, 0x10));
    std::memcpy(payload.bytes + 8, result, 4);
    std::memcpy(payload.bytes + 12, static_cast<unsigned char *>(self) + 8, 4);
    std::memcpy(payload.bytes + 16, track, 16);
}
void native_registered(void *self) {
    assert(self == payload.bytes);
    ++task_calls;
    if (getword(self, 0) != reinterpret_cast<uintptr_t>(maintainer.bytes)) return;
    if (getword(maintainer.bytes, 0x160)) {
        std::free(reinterpret_cast<void *>(getword(handler.bytes, 0xa8)));
        setword(handler.bytes, 0xa8, 0);
    }
    setword(maintainer.bytes, 0x158, 0);
    setword(maintainer.bytes, 0x160, 0);
}
void *install(uintptr_t address, void *hook, void **original) {
    *original = reinterpret_cast<void *>(address);
    if (address == reinterpret_cast<uintptr_t>(native_request)) request_entry = reinterpret_cast<decltype(request_entry)>(hook);
    else if (address == reinterpret_cast<uintptr_t>(native_handler)) handler_entry = reinterpret_cast<decltype(handler_entry)>(hook);
    else if (address == reinterpret_cast<uintptr_t>(native_reply)) reply_entry = reinterpret_cast<decltype(reply_entry)>(hook);
    else if (address == reinterpret_cast<uintptr_t>(native_registered)) registered_entry = reinterpret_cast<decltype(registered_entry)>(hook);
    else if (address == reinterpret_cast<uintptr_t>(native_memory)) memory_entry = reinterpret_cast<Helper>(hook);
    else if (address == reinterpret_cast<uintptr_t>(native_hot)) hot_entry = reinterpret_cast<Helper>(hook);
    else assert(false);
    return hook;
}
int validate(const azexit_cue_request *, void *owner, uint64_t cookie, void *) { return identity_valid && owner == maintainer.bytes && cookie == 7; }
int confirm(const azexit_cue_request *r, void *, uint64_t, uint64_t *revision, void *) {
    ++confirm_calls;
    if (publication_checks) {
        auto fresh = witness();
        fresh.revision = 91;
        assert(azsdk_cue_persist_confirm_publication(r, &fresh) == expected_publication);
        auto wrong = fresh;
        wrong.source_id[0] ^= 1;
        assert(!azsdk_cue_persist_confirm_publication(r, &wrong));
        auto wrong_request = *r;
        ++wrong_request.entry_id;
        assert(!azsdk_cue_persist_confirm_publication(&wrong_request, &fresh));
        /* A genuine native publication with identical logical rows is still
         * foreign: it is not the content retained before this request published. */
        for (unsigned hot = 0; hot < 2; ++hot) {
            void *root = hot ? static_cast<void *>(hot_set.bytes) : static_cast<void *>(memory_set.bytes);
            uintptr_t original_content = getword(root, 0x28), vector[3];
            std::memcpy(vector, reinterpret_cast<void *>(original_content + 0x28), 24);
            uintptr_t foreign = 0;
            from_vector(&foreign, vector);
            content_publish(root, &foreign, nullptr);
            release_ref(&foreign);
            auto foreign_witness = witness();
            foreign_witness.revision = 92;
            assert(azsdk_cue_persist_confirm_publication(r, &foreign_witness) == 2);
            content_publish(root, &original_content, nullptr);
        }
        fresh = witness();
        fresh.revision = 93;
        assert(azsdk_cue_persist_confirm_publication(r, &fresh) == expected_publication);
    }
    *revision = 91;
    return effect_confirmed;
}
void ready(unsigned deck, uint64_t token, void *) {
    assert(deck == 0 && token);
    ++ready_calls;
}
azexit_cue_request request(unsigned operation, uint64_t token) {
    azexit_cue_request value = {};
    value.size = sizeof value;
    value.operation = operation;
    value.identity = {token, 3, deadline(), 0, 0};
    value.entry_id = 1;
    value.expected_revision = 1;
    value.flags = AZ_COMMAND_PERSIST | AZ_COMMAND_NATIVE_LOAD;
    return value;
}
void reply(int result) {
    unsigned char track[16];
    for (unsigned i = 0; i < 16; ++i) track[i] = static_cast<unsigned char>(i + 1);
    uintptr_t memory_root = reinterpret_cast<uintptr_t>(memory_set.bytes), hot_root = reinterpret_cast<uintptr_t>(hot_set.bytes);
    uintptr_t memory_content = getword(memory_set.bytes, 0x28), hot_content = getword(hot_set.bytes, 0x28);
    if (rebuild_reply) {
        for (unsigned hot = 0; hot < 2; ++hot) {
            void *root = hot ? static_cast<void *>(hot_set.bytes) : static_cast<void *>(memory_set.bytes);
            uintptr_t old = getword(root, 0x28), vector[3], fresh = 0;
            std::memcpy(vector, reinterpret_cast<void *>(old + 0x28), 24);
            from_vector(&fresh, vector);
            content_publish(root, &fresh, nullptr);
            release_ref(&fresh);
        }
    }
    if (invalidate_reply) {
        setword(memory_set.bytes, 0x28, 0);
        setword(hot_set.bytes, 0x28, 0);
    }
    if (empty_reply_roots) memory_root = hot_root = 0;
    reply_entry(receiver.bytes, helper_request.bytes, track, &memory_root, &hot_root, &result);
    if (invalidate_reply) {
        setword(memory_set.bytes, 0x28, memory_content);
        setword(hot_set.bytes, 0x28, hot_content);
    }
}
void finish(azexit_cue_request &r, unsigned outcome, int native_result = 0) {
    bool had_helper = helper_pending;
    run_helper();
    if (had_helper) {
        unsigned kind = handler.bytes[0x38];
        uintptr_t content = getword(kind ? hot_set.bytes : memory_set.bytes, 0x28);
        uintptr_t begin = getword(reinterpret_cast<void *>(content), 0x28), end = getword(reinterpret_cast<void *>(content), 0x30);
        assert((end - begin) / 0x80 == (kind ? 8u : 10u));
        auto *row = reinterpret_cast<unsigned char *>(begin + (kind ? kind - 1 : 0) * 0x80);
        uint32_t source_rate;
        std::memcpy(&source_rate, property.bytes + 0x60, 4);
        assert(azcue_timestamps_equal(azcue_read_timestamps(row), azcue_read_timestamps(handler.bytes + 0x38), source_rate));
        if (r.operation == AZ_CMD_CUE_COMMENT) assert(!std::strcmp(reinterpret_cast<char *>(getword(row, 0x70)), r.comment));
        if (r.operation == AZ_CMD_CUE_COLOR || r.operation == AZ_CMD_HOT_SET) {
            uint32_t rgb;
            std::memcpy(&rgb, row + 0x78, 4);
            assert(rgb == (0xff000000u | r.rgb));
        }
        if (r.operation == AZ_CMD_SAVED_AUTO) assert(row[0x28] == static_cast<unsigned char>(r.position_s));
        assert(!std::strcmp(reinterpret_cast<const char *>(getword(helper_cue.bytes, 0x70)), reinterpret_cast<const char *>(getword(row, 0x70))));
        assert(!std::memcmp(helper_cue.bytes, row, 8) && !std::memcmp(helper_cue.bytes + 0x28, row + 0x28, 8));
        assert(azcue_timestamps_equal(azcue_read_timestamps(helper_cue.bytes), azcue_read_timestamps(row), source_rate));
        assert(!std::memcmp(helper_cue.bytes + 0x78, row + 0x78, 5));
    }
    std::thread response([native_result] { reply(native_result); });
    response.join();
    unsigned previous = ready_calls;
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, reinterpret_cast<azexit_result *>(property.bytes + 0x100)) == AZ_PENDING);
    std::thread task([] { registered_entry(payload.bytes); });
    task.join();
    assert(ready_calls == previous + 1);
    azexit_result out = {};
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &out) == AZ_OK && out.outcome == outcome);
    assert(out.token == r.identity.token && out.load_generation == 3);
    assert(out.revision == (outcome == AZ_COMMAND_APPLIED ? 91u : 0u));
}
} // namespace
int main() {
    azcuepersist_profile profile = {};
    profile.save = reinterpret_cast<uintptr_t>(producer);
    profile.erase = reinterpret_cast<uintptr_t>(erase);
    profile.request = reinterpret_cast<uintptr_t>(native_request);
    profile.handler_request = reinterpret_cast<uintptr_t>(native_handler);
    profile.reception_reply = reinterpret_cast<uintptr_t>(native_reply);
    profile.registered_task = reinterpret_cast<uintptr_t>(native_registered);
    profile.handler_table = reinterpret_cast<uintptr_t>(handler_table);
    profile.reception_table = reinterpret_cast<uintptr_t>(receiver_table);
    profile.string_allocate = reinterpret_cast<uintptr_t>(allocate);
    profile.string_assign = reinterpret_cast<uintptr_t>(assign);
    profile.string_destroy = reinterpret_cast<uintptr_t>(destroy);
    profile.color_construct = reinterpret_cast<uintptr_t>(color);
    profile.cue_copy = reinterpret_cast<uintptr_t>(copy_row);
    profile.cue_destroy = reinterpret_cast<uintptr_t>(destroy_row);
    profile.memory_helper = reinterpret_cast<uintptr_t>(native_memory);
    profile.hot_helper = reinterpret_cast<uintptr_t>(native_hot);
    profile.content_copy = reinterpret_cast<uintptr_t>(copy_ref);
    profile.content_release = reinterpret_cast<uintptr_t>(release_ref);
    profile.vector_append = reinterpret_cast<uintptr_t>(vector_append);
    profile.vector_destroy = reinterpret_cast<uintptr_t>(vector_destroy);
    profile.content_from_vector = reinterpret_cast<uintptr_t>(from_vector);
    profile.hot_content_from_vector = reinterpret_cast<uintptr_t>(from_vector);
    profile.content_publish = reinterpret_cast<uintptr_t>(content_publish);
    profile.hot_content_publish = reinterpret_cast<uintptr_t>(content_publish);
    init_collections();
    azcuepersist_callbacks callbacks = {validate, confirm, ready, nullptr};
    assert(!azsdk_cue_persist_capabilities());
    assert(azsdk_cue_persist_configure(&profile, install, &callbacks));
    assert(!azsdk_cue_persist_configure(&profile, install, &callbacks));
    setword(maintainer.bytes, 0x150, reinterpret_cast<uintptr_t>(queue.bytes));
    for (unsigned i = 0; i < 12; ++i) property.bytes[0x30 + i] = static_cast<unsigned char>(i + 7);
    setword(property.bytes, 0x40, 12345);
    uint64_t token = 1;
    for (unsigned rate : {44100u, 48000u, 96000u, 192000u}) for (int direction : {-1, 1}) {
        std::memcpy(property.bytes + 0x60, &rate, 4);
        auto r = request(AZ_CMD_CUE_COMMENT, token++);
        std::strcpy(r.comment, "within source sample window");
        uint64_t inside = 1000000 / rate;
        locator_shift_us = direction * int64_t(inside);
        assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
        locator_shift_us = 0;
        setword(helper_cue.bytes, 8, getword(helper_cue.bytes, 8) + direction * inside);
        setword(helper_cue.bytes, 24, getword(helper_cue.bytes, 24) - direction * inside);
        finish(r, AZ_COMMAND_APPLIED);
        r = request(AZ_CMD_CUE_COMMENT, token++);
        std::strcpy(r.comment, "outside source sample window");
        assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
        setword(helper_cue.bytes, 8, getword(helper_cue.bytes, 8) + direction * (inside + 1));
        run_helper(1);
        finish(r, AZ_COMMAND_UNCERTAIN);
    }
    unsigned property_rate = 44100;
    std::memcpy(property.bytes + 0x60, &property_rate, 4);
    for (unsigned operation : {AZ_CMD_HOT_SET, AZ_CMD_HOT_CLEAR, AZ_CMD_MEMORY_SET, AZ_CMD_MEMORY_DELETE,
                               AZ_CMD_SAVED_SAVE, AZ_CMD_SAVED_DELETE, AZ_CMD_SAVED_AUTO, AZ_CMD_CUE_COMMENT, AZ_CMD_CUE_COLOR}) {
        assert(azsdk_cue_persist_capabilities() & (UINT64_C(1) << operation));
        auto r = request(operation, token++);
        r.rgb = operation == AZ_CMD_CUE_COLOR ? 0xff7100 : 0x123456;
        if (operation == AZ_CMD_CUE_COMMENT) std::strcpy(r.comment, "caf\xc3\xa9 \xf0\x9f\x8e\xb5");
        if (operation == AZ_CMD_SAVED_AUTO) r.position_s = 1;
        unsigned kind = (operation == AZ_CMD_HOT_SET || operation == AZ_CMD_HOT_CLEAR) ? 1 : 0;
        unsigned before = producer_calls;
        assert(submit(&r, maintainer.bytes, maintainer.bytes, kind, property.bytes, 7) == AZ_PENDING);
        assert(producer_calls == before + 1);
        azexit_result out = {999, 999, 999, 999, 999};
        assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &out) == AZ_PENDING && out.token == 999);
        assert(submit(&r, maintainer.bytes, maintainer.bytes, kind, property.bytes, 7) == AZ_BUSY);
        if (operation == AZ_CMD_CUE_COMMENT) assert(observed_comment == r.comment && allocations == destructions);
        if (operation == AZ_CMD_CUE_COLOR || operation == AZ_CMD_HOT_SET) assert(observed_color == (0xff000000u | r.rgb));
        if (operation == AZ_CMD_SAVED_AUTO) assert(observed_auto == 1);
        /* Unrelated native receipts with identical result/tag/track must not
         * complete this SDK request or consume its owned receipt. */
        std::memcpy(foreign_receiver.bytes, receiver.bytes, sizeof receiver.bytes);
        setword(foreign_receiver.bytes, 0x10, reinterpret_cast<uintptr_t>(queue.bytes));
        unsigned notifications = ready_calls;
        int result = 0;
        reply_entry(foreign_receiver.bytes, nullptr, handler.bytes + 0x10, nullptr, nullptr, &result);
        registered_entry(payload.bytes);
        assert(ready_calls == notifications);
        finish(r, AZ_COMMAND_APPLIED);
    }
    for (int result : {3, 5}) {
        auto result_request = request(AZ_CMD_SAVED_SAVE, token++);
        assert(submit(&result_request, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
        finish(result_request, result == 5 ? AZ_COMMAND_APPLIED : AZ_COMMAND_UNCERTAIN, result);
    }
    auto r = request(AZ_CMD_CUE_COMMENT, token++);
    std::strcpy(r.comment, "\xc0\x80");
    unsigned before = producer_calls;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_INVALID && producer_calls == before);
    r = request(AZ_CMD_SAVED_SAVE, token++);
    setword(queue.bytes, 0x68, 8);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_BUSY && producer_calls == before);
    setword(queue.bytes, 0x68, 0);
    identity_valid = false;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_STALE && producer_calls == before);
    identity_valid = true;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    effect_confirmed = false;
    reply(0);
    registered_entry(payload.bytes);
    azexit_result delayed = {999, 999, 999, 999, 999};
    unsigned submissions = producer_calls, notifications = ready_calls;
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &delayed) == AZ_PENDING && delayed.token == 999);
    assert(producer_calls == submissions && ready_calls == notifications);
    effect_confirmed = true;
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &delayed) == AZ_OK && delayed.outcome == AZ_COMMAND_APPLIED);
    assert(producer_calls == submissions && delayed.revision == 91);

    r = request(AZ_CMD_SAVED_SAVE, token++);
    r.identity.deadline_ns = deadline() - UINT64_C(10000000000) + UINT64_C(50000000);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    reply(0);
    registered_entry(payload.bytes);
    effect_confirmed = false;
    submissions = producer_calls;
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &delayed) == AZ_PENDING);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &delayed) == AZ_OK && delayed.outcome == AZ_COMMAND_UNCERTAIN && !delayed.revision);
    assert(producer_calls == submissions);
    effect_confirmed = true;
    r = request(AZ_CMD_SAVED_SAVE, token++);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    azsdk_cue_persist_retire(maintainer.bytes);
    unsigned dispatches = handler_calls;
    assert(!handler_entry(handler.bytes) && handler_calls == dispatches);
    azexit_result abandoned = {};
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_BUSY);
    reply(0);
    registered_entry(payload.bytes); /* retired slot drains without owner polling */
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &abandoned) == AZ_STALE);
    r = request(AZ_CMD_SAVED_SAVE, token++);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    finish(r, AZ_COMMAND_APPLIED);
    r = request(AZ_CMD_CUE_COMMENT, token++);
    unsigned actual_requests = request_calls, actual_handlers = handler_calls;
    invalidate_in_producer = true;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_STALE);
    assert(request_calls == actual_requests && handler_calls == actual_handlers);
    invalidate_in_producer = false;
    identity_valid = true;
    r = request(AZ_CMD_SAVED_SAVE, token++);
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    finish(r, AZ_COMMAND_APPLIED);
    assert(request_calls + 1 == producer_calls && task_calls >= 11 && confirm_calls >= 10);
    assert(publications >= 4);
    for (unsigned operation : {AZ_CMD_CUE_COMMENT, AZ_CMD_CUE_COLOR}) {
        r = request(operation, token++);
        r.rgb = 0xaabbcc;
        std::strcpy(r.comment, "hot unicode \xe2\x9c\x93");
        assert(submit(&r, maintainer.bytes, maintainer.bytes, 1, property.bytes, 7) == AZ_PENDING);
        finish(r, AZ_COMMAND_APPLIED);
    }
    /* All collection updates execute later on a repository worker, with no
     * submitting TLS. Foreign request IDs and tracks forward unchanged. */
    r = request(AZ_CMD_CUE_COMMENT, token++);
    std::strcpy(r.comment, "async unicode \xe2\x9c\x93");
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    uintptr_t memory = reinterpret_cast<uintptr_t>(memory_set.bytes);
    unsigned saved_publications = publications;
    uint32_t old_serial;
    std::memcpy(&old_serial, helper_request.bytes + 8, 4);
    uint32_t wrong_serial = old_serial + 1;
    std::memcpy(helper_request.bytes + 8, &wrong_serial, 4);
    assert(memory_entry(nullptr, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, &memory, 1) == 1 && publications == saved_publications);
    std::memcpy(helper_request.bytes + 8, &old_serial, 4);
    handler.bytes[0x10] ^= 1;
    assert(memory_entry(nullptr, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, &memory, 1) == 1 && publications == saved_publications);
    handler.bytes[0x10] ^= 1;
    finish(r, AZ_COMMAND_APPLIED);
    for (unsigned failure = 0; failure < 9; ++failure) {
        r = request(AZ_CMD_CUE_COLOR, token++);
        r.rgb = 0x00c3ff;
        if (failure == 0) r.identity.deadline_ns = deadline() - 10000000000ULL + 50000000ULL;
        uintptr_t content = getword(memory_set.bytes, 0x28), begin = getword(reinterpret_cast<void *>(content), 0x28);
        uint64_t saved_in = getword(reinterpret_cast<void *>(begin + 0x80), 8), saved_out = getword(reinterpret_cast<void *>(begin + 0x80), 16);
        uint64_t saved_out_usec = getword(reinterpret_cast<void *>(begin + 0x80), 24), saved_out_sample = getword(reinterpret_cast<void *>(begin + 0x80), 32);
        if (failure == 2) {
            setword(reinterpret_cast<void *>(begin + 0x80), 8, getword(reinterpret_cast<void *>(begin), 8));
            setword(reinterpret_cast<void *>(begin + 0x80), 16, getword(reinterpret_cast<void *>(begin), 16));
            setword(reinterpret_cast<void *>(begin + 0x80), 24, getword(reinterpret_cast<void *>(begin), 24));
            setword(reinterpret_cast<void *>(begin + 0x80), 32, getword(reinterpret_cast<void *>(begin), 32));
        }
        int submitted = submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7);
        if (failure == 2) {
            assert(submitted == AZ_STALE);
            setword(reinterpret_cast<void *>(begin + 0x80), 8, saved_in);
            setword(reinterpret_cast<void *>(begin + 0x80), 16, saved_out);
            setword(reinterpret_cast<void *>(begin + 0x80), 24, saved_out_usec);
            setword(reinterpret_cast<void *>(begin + 0x80), 32, saved_out_sample);
            continue;
        }
        assert(submitted == AZ_PENDING);
        saved_publications = publications;
        if (failure == 0) std::this_thread::sleep_for(std::chrono::milliseconds(60));
        if (failure == 1) {
            auto *c = reinterpret_cast<unsigned char *>(getword(hot_set.bytes, 0x28));
            auto *row = reinterpret_cast<unsigned char *>(getword(c, 0x28));
            row[0x28] ^= 1;
        }
        if (failure == 3) setword(helper_cue.bytes, 8, getword(helper_cue.bytes, 8) + 1000000 / property_rate + 1);
        if (failure >= 6) {
            unsigned offset = failure == 6 ? 16 : failure == 7 ? 24
                                                               : 32;
            setword(helper_cue.bytes, offset, getword(helper_cue.bytes, offset) + (offset == 24 ? 1000000 / property_rate + 1 : 1));
        }
        uintptr_t old_hot_content = getword(hot_set.bytes, 0x28);
        if (failure == 4) setword(hot_set.bytes, 0x28, content);
        if (failure == 5) identity_valid = false;
        helper_pending = false;
        assert(memory_entry(nullptr, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, &memory, 1) == 1 && publications == saved_publications);
        if (failure == 1) {
            auto *c = reinterpret_cast<unsigned char *>(getword(hot_set.bytes, 0x28));
            auto *row = reinterpret_cast<unsigned char *>(getword(c, 0x28));
            row[0x28] ^= 1;
        }
        if (failure == 4) setword(hot_set.bytes, 0x28, old_hot_content);
        identity_valid = true;
        if (failure == 2) {
            setword(reinterpret_cast<void *>(begin + 0x80), 8, saved_in);
            setword(reinterpret_cast<void *>(begin + 0x80), 16, saved_out);
            setword(reinterpret_cast<void *>(begin + 0x80), 24, saved_out_usec);
            setword(reinterpret_cast<void *>(begin + 0x80), 32, saved_out_sample);
        }
        finish(r, AZ_COMMAND_UNCERTAIN, 5);
    }

    /* Distinct OUT points remain distinct even with identical IN coordinates. */
    uintptr_t content = getword(memory_set.bytes, 0x28), begin = getword(reinterpret_cast<void *>(content), 0x28);
    uint64_t saved_in = getword(reinterpret_cast<void *>(begin + 0x80), 8), saved_sample = getword(reinterpret_cast<void *>(begin + 0x80), 16);
    setword(reinterpret_cast<void *>(begin + 0x80), 8, getword(reinterpret_cast<void *>(begin), 8));
    setword(reinterpret_cast<void *>(begin + 0x80), 16, getword(reinterpret_cast<void *>(begin), 16));
    uint64_t other_out = getword(reinterpret_cast<void *>(begin + 0x80), 24), other_out_sample = getword(reinterpret_cast<void *>(begin + 0x80), 32);
    r = request(AZ_CMD_CUE_COMMENT, token++);
    std::strcpy(r.comment, "exact complete tuple");
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    finish(r, AZ_COMMAND_APPLIED);
    content = getword(memory_set.bytes, 0x28);
    begin = getword(reinterpret_cast<void *>(content), 0x28);
    assert(getword(reinterpret_cast<void *>(begin + 0x80), 24) == other_out && getword(reinterpret_cast<void *>(begin + 0x80), 32) == other_out_sample);
    setword(reinterpret_cast<void *>(begin + 0x80), 8, saved_in);
    setword(reinterpret_cast<void *>(begin + 0x80), 16, saved_sample);
    /* A receipt arriving while the helper owns the slot must wait until all
     * native clone refs are released, then issue exactly one continuation. */
    r = request(AZ_CMD_CUE_COMMENT, token++);
    std::strcpy(r.comment, "receipt while owned");
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    receipt_in_helper = true;
    unsigned previous_ready = ready_calls;
    run_helper(1);
    receipt_in_helper = false;
    assert(ready_calls == previous_ready + 1);
    azexit_result concurrent = {};
    assert(azsdk_cue_persist_poll(&r, maintainer.bytes, &concurrent) == AZ_OK && concurrent.outcome == AZ_COMMAND_UNCERTAIN);
    /* Native successful fresh Hot insertion is forwarded with no clone. */
    r = request(AZ_CMD_HOT_SET, token++);
    r.expected_revision = 0;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 1, property.bytes, 7) == AZ_PENDING);
    hot_fresh = true;
    helper_pending = false;
    saved_publications = publications;
    uintptr_t hot_root = reinterpret_cast<uintptr_t>(hot_set.bytes);
    assert(hot_entry(nullptr, helper_request.bytes, handler.bytes + 0x10, helper_cue.bytes, &hot_root, 1) == 0 && publications == saved_publications);
    hot_fresh = false;
    finish(r, AZ_COMMAND_APPLIED);
    r = request(AZ_CMD_CUE_COMMENT, token++);
    auto stale_witness = witness();
    stale_witness.fingerprint ^= 1;
    unsigned before_producer = producer_calls;
    assert(azsdk_cue_persist_submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7, &stale_witness) == AZ_STALE && producer_calls == before_producer);
    r = request(AZ_CMD_SAVED_AUTO, token++);
    r.position_s = 0;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    auto before_publication = witness();
    assert(!azsdk_cue_persist_confirm_publication(&r, &before_publication));
    publication_checks = true;
    rebuild_reply = true;
    finish(r, AZ_COMMAND_APPLIED);
    rebuild_reply = false;
    publication_checks = false;
    r = request(AZ_CMD_SAVED_AUTO, token++);
    r.position_s = 1;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    publication_checks = true;
    invalidate_reply = true;
    expected_publication = 2;
    finish(r, AZ_COMMAND_APPLIED);
    publication_checks = false;
    invalidate_reply = false, empty_reply_roots = false;
    expected_publication = 1;
    r = request(AZ_CMD_SAVED_AUTO, token++);
    r.position_s = 0;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
    publication_checks = true;
    empty_reply_roots = true;
    expected_publication = 2;
    finish(r, AZ_COMMAND_APPLIED);
    publication_checks = false;
    empty_reply_roots = false;
    expected_publication = 1;
    r = request(AZ_CMD_CUE_COLOR, token++);
    r.rgb = 0x2ba6d9;
    unsigned rejected_before = producer_calls;
    assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_UNSUPPORTED && producer_calls == rejected_before);
    for (unsigned rgb : {0x000000u, 0xff00c6u, 0xff0000u, 0xff7100u, 0xffff00u, 0x00e300u, 0x00c3ffu, 0x0051ffu, 0x9c08ffu}) {
        r = request(AZ_CMD_CUE_COLOR, token++);
        r.rgb = rgb;
        assert(submit(&r, maintainer.bytes, maintainer.bytes, 0, property.bytes, 7) == AZ_PENDING);
        finish(r, AZ_COMMAND_APPLIED);
    }
    if (helper_cue_owned) destroy_row(helper_cue.bytes);
    for (auto content : owned_contents) {
        uintptr_t v[3];
        std::memcpy(v, content + 0x28, 24);
        vector_destroy(v);
        delete[] content;
    }
    ref_releases += 2; /* final fixture publication roots */
    assert(ref_copies == ref_releases);
}
