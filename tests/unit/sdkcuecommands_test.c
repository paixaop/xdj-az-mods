/* SPDX-License-Identifier: GPL-3.0-only */
/* Orchestration fixture: native receipts, store revisions and DSP settlement are
 * independently controlled. No firmware is executed. */
#include "../../src/common/azcuecommands.h"
#include "../../src/common/azcueactor.h"
#include "../../src/common/azcuepersist.h"
#include "../../src/common/azcueprofile.h"
#include "../../src/common/azcuerecall.h"
#include "../../src/common/azcuerecallbridge.h"
#include "../../src/common/azcuestore.h"
#include "../../src/common/aztimestamp.h"
#include "../../src/common/azloadobserver.h"
#include "../../src/azmod/azmod-mod.h"
#include <assert.h>
#include <string.h>
#include <sched.h>
#include <stdlib.h>

#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
void azsdk_cue_commands_test_cache_lock(unsigned, void (*)(void *), void *);
typedef struct {
    azcs_native_cue cue;
    azcs_native_property property;
} Row;
typedef struct {
    Row data[64];
    size_t count;
} Rows;
static Rows rows;
typedef struct WitnessRows {
    Rows rows;
    size_t memory_count;
    uint64_t key;
    struct WitnessRows *next;
} WitnessRows;
static WitnessRows *witnessed_rows;
static WitnessRows *find_witness(uint64_t key) {
    for (WitnessRows *p = witnessed_rows; p; p = p->next)
        if (p->key == key) return p;
    return NULL;
}
static void save_witness(uint64_t key, Rows values, size_t memory_count) {
    WitnessRows *p = find_witness(key);
    if (!p) {
        p = calloc(1, sizeof *p);
        assert(p);
        p->key = key;
        p->next = witnessed_rows;
        witnessed_rows = p;
    }
    p->rows = values;
    p->memory_count = memory_count;
}
static __attribute__((destructor)) void release_witnesses(void) {
    while (witnessed_rows) {
        WitnessRows *next = witnessed_rows->next;
        free(witnessed_rows);
        witnessed_rows = next;
    }
}
static void rows_insert(Rows *values, size_t index, Row value) {
    assert(index <= values->count && values->count < sizeof values->data / sizeof values->data[0]);
    memmove(values->data + index + 1, values->data + index, (values->count - index) * sizeof value);
    values->data[index] = value;
    ++values->count;
}
static uint64_t generation = 42, revision = 1;
static uint32_t source_clock = 48000;
static bool main_valid = true;
static int stored_memory_count = -1, fail_property = -1;
static unsigned property_calls = 0, canonical_calls = 0, witness_calls = 0;
static bool fail_witness = false;
static struct azcuepersist_witness submitted_witness = {0};
static azcs_native_property main_property;
static uintptr_t manager[64];
static struct azcue_actor_callbacks actor;
static struct azcuepersist_callbacks persist;
static struct azexit_cue_request mailbox = (__typeof__(mailbox)) {}, active_persist = (__typeof__(active_persist)) {};
static struct azexit_result terminal = (__typeof__(terminal)) {}, bridge_result = (__typeof__(bridge_result)) {};
static unsigned phase, persist_calls, stages, recalls, native_kind;
static unsigned resume_busy;
static unsigned persist_busy;
static unsigned persist_stale;
static unsigned enqueue_busy;
static bool hold_background_query;
static unsigned loaded_decks = 1, background_attempts[4];
static uint64_t cancelled_background_query;
static unsigned background_cancel_busy, background_release_busy;
static uint64_t cookie, submitted_sample;
static bool receipt, bridge_done, bridge_cancel_success;
static struct azexit_cue_request stage_request = {0};
static bool override_hot_view = false, missing_hot_view = false, override_stage_sample = false;
static bool fail_snapshot = false, fail_stage_capture = false;
static azcs_native_property hot_view = {0};
static struct azexit_cue_stage_sample stage_sample = (__typeof__(stage_sample)) {};
static bool publication_matches = true, force_native_failure = false;
static int publication_mode = 1;
static bool inject_foreign_after_confirm = false, expire_after_confirm = false;
static struct azexit_cue_request submissions[1024];
static size_t submission_count;
static uint64_t submitted_cookies[1024];
static size_t cookie_count;
#define put(T, p, offset, value)                                       \
    do {                                                               \
        T put_value = (value);                                         \
        memcpy((char *) (p) + (offset), &put_value, sizeof put_value); \
    } while (0)
#define get(T, p, offset) ({ T get_value;memcpy(&get_value,(const char *)(p)+(offset),sizeof get_value);get_value; })
static void fixture_property(azcs_native_property *p, uint64_t sample, bool loop) {
    (*p) = (__typeof__((*p))) {};
    {
        unsigned range_values_1[] = {0x30u, 0x88u};
        for (unsigned range_index_1 = 0; range_index_1 < sizeof(range_values_1) / sizeof(range_values_1)[0]; ++range_index_1) {
            unsigned offset = range_values_1[range_index_1];

            p->bytes[offset] = 1;
            p->bytes[offset + 4] = 1;
            p->bytes[offset + 6] = 1;
            put(int32_t, p, offset + 8, 1);
            put(uintptr_t, p, offset + 0x20, 0x10000);
            put(uint32_t, p, offset + 0x30, 48000);
        }
    }
    put(uint64_t, p, 0x40, sample);
    put(uint64_t, p, 0x68, sample * 1000000 / 48000);
    put(uint64_t, p, 0x98, loop ? sample + 48000 : 0);
    put(uint64_t, p, 0xc0, loop ? (sample + 48000) * 1000000 / 48000 : UINT64_C(0x7fffffffffffffff));
}
static Row row(unsigned kind, uint64_t sample, bool loop, const char *comment, unsigned rgb) {
    Row r = {0};
    r.cue.bytes[0] = ((unsigned char) (kind));
    put(uintptr_t, &r.cue, 0x70, (uintptr_t) (comment));
    put(uint32_t, &r.cue, 0x78, (kind || rgb ? 0xff000000u : 0u) | rgb);
    fixture_property(&(r.property), sample, loop);
    return r;
}
static uint64_t all_persist() {
    uint64_t mask = 0;
    {
        unsigned range_values_2[] = {AZ_CMD_SAVED_SAVE, AZ_CMD_SAVED_DELETE, AZ_CMD_SAVED_AUTO, AZ_CMD_HOT_SET, AZ_CMD_HOT_CLEAR,
                                     AZ_CMD_MEMORY_SET, AZ_CMD_MEMORY_DELETE, AZ_CMD_CUE_COMMENT, AZ_CMD_CUE_COLOR, AZ_CMD_CUE_POINT_SET, AZ_CMD_CUE_LOOP_SET};
        for (unsigned range_index_2 = 0; range_index_2 < sizeof(range_values_2) / sizeof(range_values_2)[0]; ++range_index_2) {
            unsigned operation = range_values_2[range_index_2];
            mask |= UINT64_C(1) << operation;
        }
    }
    return mask;
}
static struct azcue_native_timestamps times(const azcs_native_property *p) {
    struct azcue_native_timestamps t = {0};
    memcpy(&t.sample_rate, p->bytes + 0x60, 4);
    memcpy(&t.in_sample, p->bytes + 0x40, 8);
    memcpy(&t.in_us, p->bytes + 0x68, 8);
    memcpy(&t.out_sample, p->bytes + 0x98, 8);
    memcpy(&t.out_us, p->bytes + 0xc0, 8);
    return t;
}

azcs_profile azcs_native_profile(int unused_build, uintptr_t unused_bias) {
    (void) unused_build;
    (void) unused_bias;
    return (azcs_profile) {};
}
void azcs_owned_cue_reset(azcs_owned_cue *self) {
    (void) (*self);
    if (self->destroy_) self->destroy_(&self->native);
    self->destroy_ = NULL;
    self->native = (__typeof__(self->native)) {};
}
void azcs_property_reset(azcs_property *self) {
    (void) (*self);
    if (self->copy_) self->copy_ = NULL;
    self->native = (__typeof__(self->native)) {};
    self->destroy_ = NULL;
}
static void dummy_destroy(void *unused_0) { (void) unused_0; }
bool azcs_property_copy_to(const azcs_property *self, azcs_property *out) {
    (void) (*self);
    if (!azcs_property_valid(self)) return false;
    out->native = self->native;
    out->destroy_ = dummy_destroy;
    return true;
}
void azcs_snapshot_reset(azcs_snapshot *self) {
    (void) (*self);
    self->player_ = self->source_ = self->where_from_ = self->memory_set_ = self->hot_set_ = self->content_ = self->hot_content_ = self->pack_ = 0;
    self->count_ = self->memory_count_ = 0;
    self->revision_ = self->fingerprint_ = 0;
}
bool azcs_snapshot_open(azcs_snapshot *self, const azcs_profile *unused_profile, void *unused_manager, uint64_t load, const unsigned char unused_source[12], azcs_revision_state *s) {
    (void) (*unused_profile);
    (void) unused_manager;
    (void) unused_source;
    (void) (*self);
    if (load != generation || fail_snapshot) return false;
    self->count_ = rows.count;
    self->memory_count_ = 0;
    while (self->memory_count_ < rows.count && rows.data[self->memory_count_].cue.bytes[0] == 0) ++self->memory_count_;
    if (stored_memory_count >= 0) self->memory_count_ = (size_t) (stored_memory_count);
    self->revision_ = revision;
    self->fingerprint_ = self->revision_ * 1931 + rows.count;
    s->revision = revision;
    return true;
}
bool azcs_snapshot_row(const azcs_snapshot *self, size_t i, azcs_owned_cue *out) {
    (void) (*self);
    if (i >= rows.count) return false;
    out->native = rows.data[i].cue;
    return true;
}
bool azcs_snapshot_property(const azcs_snapshot *self, uint32_t id, uint64_t rev, azcs_property *out) {
    (void) self;
    ++property_calls;
    if (!id || id > rows.count || rev != revision || (int) (id) == fail_property) return false;
    out->native = rows.data[id - 1].property;
    out->destroy_ = dummy_destroy;
    return true;
}
bool azcs_snapshot_content_witness(const azcs_snapshot *self, uint64_t expected, struct azcuepersist_witness *out, bool unused_3) {
    (void) unused_3;
    (void) (*self);
    ++witness_calls;
    if (!out || expected != self->revision_ || fail_witness) return false;
    *out = (__typeof__(*out)) {};
    out->source_info = 0x50000;
    out->memory_set = 0x60000;
    out->hot_set = 0x70000;
    out->memory_content = 0x80000;
    out->hot_content = 0x90000;
    out->fingerprint = self->fingerprint_;
    out->revision = self->revision_;
    out->source_id[0] = 1;
    save_witness(out->fingerprint, rows, self->memory_count_);
    return true;
}
bool azcs_snapshot_auto_only_changed(const azcs_snapshot *self, const struct azcuepersist_witness *before, uint32_t entry, bool desired, uint32_t source_rate) {
    (void) (*self);
    assert(source_rate);
    __auto_type found = find_witness(before->fingerprint);
    if (found == NULL || found->rows.count != rows.count || found->memory_count != self->memory_count_ ||
        !entry || entry > self->memory_count_) return false;
    for (size_t i = 0; i < rows.count; ++i) {
        const Row *a = &(found->rows.data[i]);
        const Row *b = &(rows.data[i]);
        if (a->cue.bytes[0] != b->cue.bytes[0] || memcmp(a->cue.bytes + 4, b->cue.bytes + 4, 0x24) ||
            memcmp(a->cue.bytes + 0x2a, b->cue.bytes + 0x2a, 4) || memcmp(a->cue.bytes + 0x30, b->cue.bytes + 0x30, 4) || memcmp(a->cue.bytes + 0x50, b->cue.bytes + 0x50, 4) || memcmp(a->cue.bytes + 0x48, b->cue.bytes + 0x48, 4) ||
            memcmp(a->cue.bytes + 0x68, b->cue.bytes + 0x68, 4) || memcmp(a->cue.bytes + 0x78, b->cue.bytes + 0x78, 5)) return false;
        if (i == entry - 1) {
            if (b->cue.bytes[0] != 0 || b->cue.bytes[0x28] != (unsigned) (desired)) return false;
        } else if (a->cue.bytes[0x28] != b->cue.bytes[0x28]) return false;
        uintptr_t text_a = 0, text_b = 0;
        memcpy(&text_a, a->cue.bytes + 0x70, 8);
        memcpy(&text_b, b->cue.bytes + 0x70, 8);
        if (strcmp(text_a ? ((const char *) (text_a)) : "", text_b ? ((const char *) (text_b)) : "")) return false;
        {
            unsigned range_values_3[] = {0x30u, 0x88u};
            for (unsigned range_index_3 = 0; range_index_3 < sizeof(range_values_3) / sizeof(range_values_3)[0]; ++range_index_3) {
                unsigned base = range_values_3[range_index_3];
                if (memcmp(a->property.bytes + base, b->property.bytes + base, 12) ||
                    memcmp(a->property.bytes + base + 0x10, b->property.bytes + base + 0x10, 8) ||
                    memcmp(a->property.bytes + base + 0x30, b->property.bytes + base + 0x30, 4) ||
                    memcmp(a->property.bytes + base + 0x38, b->property.bytes + base + 0x38, 8)) return false;
            }
        }
    }
    return true;
}
bool azcs_snapshot_metadata_only_changed(const azcs_snapshot *self, const struct azcuepersist_witness *before, unsigned kind, uint32_t entry, const char *comment, uint32_t rgb, uint32_t source_rate) {
    (void) (*self);
    __auto_type found = find_witness(before->fingerprint);
    if (!source_rate || found == NULL || found->rows.count != rows.count ||
        found->memory_count != self->memory_count_ || !entry || (kind ? kind != entry : entry > self->memory_count_)) return false;
    unsigned matches = 0;
    for (size_t i = 0; i < rows.count; ++i) {
        const Row *a = &(found->rows.data[i]);
        const Row *b = &(rows.data[i]);
        bool selected = kind ? i >= self->memory_count_ && a->cue.bytes[0] == kind : i + 1 == entry;
        if (selected) ++matches;
        if (a->cue.bytes[0] != b->cue.bytes[0] || a->cue.bytes[0x28] != b->cue.bytes[0x28]) return false;
        uintptr_t old_text = 0, new_text = 0;
        uint32_t old_color = 0, new_color = 0;
        memcpy(&old_text, a->cue.bytes + 0x70, 8);
        memcpy(&new_text, b->cue.bytes + 0x70, 8);
        memcpy(&old_color, a->cue.bytes + 0x78, 4);
        memcpy(&new_color, b->cue.bytes + 0x78, 4);
        const char *wanted = selected && comment ? comment : old_text ? ((const char *) (old_text))
                                                                      : "";
        if (strcmp(wanted, new_text ? ((const char *) (new_text)) : "") ||
            (selected && !comment ? new_color != (0xff000000u | rgb) && (kind || rgb || new_color) : new_color != old_color)) return false;

        if (!azcue_timestamps_equal(times(&(a->property)), times(&(b->property)), source_rate) ||
            memcmp(a->property.bytes + 0x30, b->property.bytes + 0x30, 12) ||
            memcmp(a->property.bytes + 0x88, b->property.bytes + 0x88, 12)) return false;
    }
    return matches == 1;
}
uint32_t azcs_snapshot_source_rate(const azcs_snapshot *self) {
    (void) (*self);
    return source_clock;
}
bool azcs_snapshot_timed_property(const azcs_snapshot *self, unsigned unused_1, double in, double out, bool loop, uint32_t unused_5, const char *comment, azcs_property *value) {
    (void) unused_5;
    (void) unused_1;
    (void) (*self);
    fixture_property(&(value->native), (uint64_t) (in * 48000), loop);
    put(uintptr_t, &value->native, 0xe8, ((uintptr_t) (comment)));
    if (loop) {
        put(uint64_t, &value->native, 0x98, (uint64_t) (out * 48000));
        put(uint64_t, &value->native, 0xc0, (uint64_t) (out * 1000000));
    }
    value->destroy_ = dummy_destroy;
    return true;
}
bool azcs_snapshot_creation_only_changed(const azcs_snapshot *self, const struct azcuepersist_witness *before, unsigned kind, const azcs_property *wanted, uint32_t rate, const char *comment, uint32_t rgb) {
    (void) (*self);
    __auto_type found = find_witness(before->fingerprint);
    if (!rate || found == NULL || rows.count != found->rows.count + 1) return false;
    unsigned matched = 0;
    for (size_t row_index_1 = 0; row_index_1 < found->rows.count; ++row_index_1) {
        const Row *old = &(found->rows.data[row_index_1]);

        bool present = false;
        for (size_t row_index_2 = 0; row_index_2 < rows.count; ++row_index_2) {
            const Row *fresh = &(rows.data[row_index_2]);
            if (!memcmp(old, fresh, sizeof(*old))) present = true;
        }
        if (!present) return false;
    }
    uint64_t sample;
    memcpy(&sample, wanted->native.bytes + 0x40, sizeof sample);
    for (size_t row_index_3 = 0; row_index_3 < rows.count; ++row_index_3) {
        const Row *fresh = &(rows.data[row_index_3]);

        uint64_t actual;
        memcpy(&actual, fresh->property.bytes + 0x40, sizeof actual);
        const char *text = ((const char *) (get(uintptr_t, &fresh->cue, 0x70)));
        uint32_t color = get(uint32_t, &fresh->cue, 0x78) & 0xffffff;
        if (fresh->cue.bytes[0] == kind && compare_timestamps(actual, sample, rate, rate, 1) == AZ_TIME_EQUAL &&
            !strcmp(text ? text : "", comment) && color == rgb) ++matched;
    }
    return matched == 1;
}
bool azcs_snapshot_canonical_property(const azcs_snapshot *self, unsigned unused_1, const azcs_property *original, azcs_property *out) {
    (void) unused_1;
    (void) (*self);
    ++canonical_calls;
    if (!azcs_property_copy_to(original, &((*out)))) return false;
    put(uintptr_t, &out->native, 0xe8, 0); // Stock Property-to-Cue roundtrip omits labels.
    {
        unsigned range_values_4[] = {0x30u, 0x88u};
        for (unsigned range_index_4 = 0; range_index_4 < sizeof(range_values_4) / sizeof(range_values_4)[0]; ++range_index_4) {
            unsigned offset = range_values_4[range_index_4];

            uint32_t rate;
            uint64_t usec;
            memcpy(&rate, original->native.bytes + offset + 0x30, 4);
            memcpy(&usec, original->native.bytes + offset + 0x38, 8);
            if (usec == UINT64_C(0x7fffffffffffffff)) continue;
        // Independent fixture for the native 2.00 integer-usec serialization.
            uint64_t sample = (uint64_t) ((double) (rate * usec) * 0.000001);
            put(uint64_t, &out->native, offset + 0x10, sample);
        }
    }
    return true;
}
bool azcs_snapshot_current_property(const azcs_snapshot *self, unsigned kind, azcs_property *out) {
    (void) (*self);
    if (kind && override_hot_view) {
        if (missing_hot_view) return false;
        out->native = hot_view;
        out->destroy_ = dummy_destroy;
        return true;
    }
    if (!kind) {
        if (!main_valid) return false;
        out->native = main_property;
        out->destroy_ = dummy_destroy;
        return true;
    }
    for (size_t row_index_4 = 0; row_index_4 < rows.count; ++row_index_4) {
        const Row *r = &(rows.data[row_index_4]);
        if (r->cue.bytes[0] == kind) {
            out->native = r->property;
            out->destroy_ = dummy_destroy;
            return true;
        }
    }
    return false;
}

int azsdk_load_read(int deck, struct azsdk_load_sample *out) {
    if (deck < 0 || deck >= 4 || !(loaded_decks & (1u << deck)) || !generation) return 0;
    *out = (__typeof__(*out)) {};
    out->load_generation = generation;
    return 1;
}
int azsdk_cue_main_profile(int unused_0, struct azcue_profile *p) {
    (void) unused_0;
    *p = (__typeof__(*p)) {};
    p->actor.memory_producer = 0x81db60;
    return 1;
}
int azsdk_cue_lifecycle_configure(int unused_0, azexit_install_fn unused_1) {
    (void) unused_0;
    (void) unused_1;
    return 1;
}
int azsdk_cue_actor_configure(const struct azcue_actor_profile *unused_0, azexit_install_fn unused_1, const struct azcue_actor_callbacks *c) {
    (void) unused_0;
    (void) unused_1;
    actor = *c;
    return AZ_OK;
}
int azsdk_cue_actor_enqueue(const struct azexit_cue_request *r) {
    if (!r->operation) ++background_attempts[r->identity.deck];
    if (enqueue_busy) {
        --enqueue_busy;
        return AZ_BUSY;
    }
    if (phase) return AZ_BUSY;
    mailbox = *r;
    phase = 1;
    return AZ_PENDING;
}
void azsdk_cue_actor_drive(unsigned deck) {
    if (deck != mailbox.identity.deck || phase != 1) return;
    if (hold_background_query && mailbox.operation == 0) return;
    phase = 2;
    struct azexit_result result = {0};
    int rc = actor.execute(&mailbox, manager, &result, NULL);
    if (phase == 1) return;
    if (rc == AZ_PENDING) phase = 3;
    else {
        terminal = result;
        if (rc != AZ_OK) terminal = (__typeof__(terminal)) {mailbox.identity.token, mailbox.identity.load_generation, 0, AZ_COMMAND_REJECTED, rc};
        phase = 4;
    }
}
int azsdk_cue_actor_resume(unsigned deck, uint64_t token) {
    if (resume_busy) {
        --resume_busy;
        return AZ_BUSY;
    }
    if (deck != mailbox.identity.deck || token != mailbox.identity.token) return AZ_STALE;
    phase = 1;
    return AZ_OK;
}
int azsdk_cue_actor_poll(unsigned deck, struct azexit_result *r) {
    if (deck != mailbox.identity.deck || phase != 4) return AZ_PENDING;
    *r = terminal;
    return AZ_OK;
}
int azsdk_cue_actor_release(unsigned deck, uint64_t token) {
    if (deck != mailbox.identity.deck || phase != 4 || token != terminal.token) return AZ_BUSY;
    if (mailbox.operation == 0 && background_release_busy) {
        --background_release_busy;
        return AZ_BUSY;
    }
    phase = 0;
    return AZ_OK;
}
int azsdk_cue_actor_cancel(unsigned deck, uint64_t token) {
    if (deck != mailbox.identity.deck || token != mailbox.identity.token) return AZ_STALE;
    if (mailbox.operation == 0 && background_cancel_busy) {
        --background_cancel_busy;
        return AZ_PENDING;
    }
    if (mailbox.operation == 0) cancelled_background_query = token;
    terminal = (__typeof__(terminal)) {token, mailbox.identity.load_generation, 0, AZ_COMMAND_CANCELLED, AZ_CANCELLED};
    phase = 4;
    return AZ_OK;
}
int azsdk_cue_actor_complete(unsigned deck, uint64_t token, const struct azexit_result *r) {
    if (deck != mailbox.identity.deck || token != mailbox.identity.token) return AZ_STALE;
    terminal = *r;
    phase = 4;
    return AZ_OK;
}
int azsdk_cue_persist_configure(const struct azcuepersist_profile *unused_0, azexit_install_fn unused_1, const struct azcuepersist_callbacks *c) {
    (void) unused_0;
    (void) unused_1;
    persist = *c;
    return 1;
}
uint64_t azsdk_cue_persist_capabilities() { return all_persist(); }
int azsdk_cue_persist_submit(const struct azexit_cue_request *r, void *m, void *unused_2, unsigned kind, const void *property_input, uint64_t source_cookie, const struct azcuepersist_witness *witness) {
    (void) unused_2;
    assert(persist.validate(r, m, source_cookie, NULL));
    if (persist_busy) {
        --persist_busy;
        return AZ_BUSY;
    } // No producer/request/reply was created.
    if (persist_stale) {
        --persist_stale;
        return AZ_STALE;
    } // Preflight witness changed; no producer/request/reply.
    bool metadata = r->operation == AZ_CMD_SAVED_AUTO || r->operation == AZ_CMD_CUE_COMMENT || r->operation == AZ_CMD_CUE_COLOR || r->operation == AZ_CMD_HOT_SET || r->operation == AZ_CMD_CUE_POINT_SET || r->operation == AZ_CMD_CUE_LOOP_SET;
    assert(metadata == (witness != NULL));
    if (witness) {
        assert(witness->revision == (r->operation == AZ_CMD_HOT_SET ? revision : r->expected_revision) && witness->fingerprint);
        assert(witness->source_info == 0x50000 && witness->memory_set == 0x60000 && witness->hot_set == 0x70000);
        assert(witness->memory_content == 0x80000 && witness->hot_content == 0x90000 && witness->source_id[0] == 1);
        assert((bool) (witness->selected_cue) == (r->operation != AZ_CMD_HOT_SET && r->operation != AZ_CMD_CUE_POINT_SET && r->operation != AZ_CMD_CUE_LOOP_SET));
        if (witness->selected_cue) {
            const void *native = ((const void *) (witness->selected_cue));
            bool found = false;
            for (size_t row_index_5 = 0; row_index_5 < rows.count; ++row_index_5) {
                const Row *row = &(rows.data[row_index_5]);
                if (!memcmp(native, &row->cue, sizeof(row->cue))) found = true;
            }
            assert(found);
        }
        submitted_witness = *witness;
    }
    ++persist_calls;
    assert(submission_count < 1024);
    submissions[submission_count++] = *r;
    assert(cookie_count < 1024);
    submitted_cookies[cookie_count++] = source_cookie;
    memcpy(&submitted_sample, ((const char *) (property_input)) + 0x40, 8);
    active_persist = *r;
    native_kind = kind;
    cookie = source_cookie;
    receipt = false;
    return AZ_PENDING;
}
int azsdk_cue_persist_poll(const struct azexit_cue_request *r, void *m, struct azexit_result *out) {
    if (!receipt) return AZ_PENDING;
    uint64_t rev = 0;
    bool applied = !force_native_failure && persist.confirm(r, m, cookie, &rev, NULL);
    *out = (__typeof__(*out)) {r->identity.token, r->identity.load_generation, rev, applied ? AZ_COMMAND_APPLIED : AZ_COMMAND_UNCERTAIN, applied ? AZ_OK : AZ_FAILED};
    if (applied && inject_foreign_after_confirm) {
        put(uint32_t, &rows.data[rows.count - 1].cue, 0x78, 0xff135790);
        ++revision;
        inject_foreign_after_confirm = false;
    }
    if (applied && expire_after_confirm) {
        azsdk_cue_commands_test_time(r->identity.deadline_ns);
        expire_after_confirm = false;
    }
    return AZ_OK;
}
int azsdk_cue_persist_confirm_publication(const struct azexit_cue_request *r, const struct azcuepersist_witness *fresh) {
    assert(r->identity.token == active_persist.identity.token && fresh->revision == revision);
    return publication_matches ? publication_mode : 0;
}
int azsdk_cue_recall_configure(int unused_0, uintptr_t unused_1) {
    (void) unused_0;
    (void) unused_1;
    return AZ_OK;
}
int azsdk_cue_recall_prepare(const struct azexit_cue_request *r, void *unused_1, const void *property) {
    (void) unused_1;
    if (r->operation == AZ_CMD_CUE_POINT_SET || r->operation == AZ_CMD_CUE_LOOP_SET) {
        const char *label = ((const char *) (get(uintptr_t, property, 0xe8)));
        assert(label && !strcmp(label, r->comment));
    }
    ++recalls;
    bridge_result = (__typeof__(bridge_result)) {r->identity.token, r->identity.load_generation, 1, AZ_COMMAND_APPLIED, AZ_OK};
    bridge_done = false;
    return AZ_PENDING;
}
void azsdk_cue_recall_release(unsigned unused_0, uint64_t unused_1) {
    (void) unused_0;
    (void) unused_1;
}
int azsdk_exit_cue_stage_submit(const struct azexit_cue_request *r) {
    ++stages;
    stage_request = *r;
    bridge_result = (__typeof__(bridge_result)) {r->identity.token, r->identity.load_generation, 0, AZ_COMMAND_APPLIED, AZ_OK};
    bridge_done = false;
    return AZ_PENDING;
}
int azsdk_exit_cue_stage_capture(unsigned deck, uint64_t token, struct azexit_cue_stage_sample *out) {
    assert(!deck && token == stage_request.identity.token && bridge_done && out);
    if (fail_stage_capture) return AZ_STALE;
    *out = (__typeof__(*out)) {};
    if (stage_request.operation == AZ_CMD_HOT_CLEAR) return AZ_OK;
    if (override_stage_sample) {
        *out = stage_sample;
        return AZ_OK;
    }
    for (size_t row_index_6 = 0; row_index_6 < rows.count; ++row_index_6) {
        const Row *r = &(rows.data[row_index_6]);
        if (r->cue.bytes[0] == stage_request.entry_id) {
            memcpy(out->source_id, r->property.bytes + 0x30, 12);
            memcpy(&out->in_sample, r->property.bytes + 0x40, 8);
            memcpy(&out->out_sample, r->property.bytes + 0x98, 8);
            memcpy(&out->sample_rate, r->property.bytes + 0x60, 4);
            out->out_valid = r->property.bytes[0x88] && compare_timestamps(out->out_sample, out->in_sample, out->sample_rate, out->sample_rate, 1) == AZ_TIME_AFTER;
            return AZ_OK;
        }
    }
    return AZ_STALE;
}
int azsdk_exit_recall_cancel(unsigned unused_0, uint64_t unused_1) {
    (void) unused_0;
    (void) unused_1;
    if (!bridge_cancel_success) return AZ_PENDING;
    bridge_result.outcome = AZ_COMMAND_CANCELLED;
    bridge_result.reason = AZ_CANCELLED;
    bridge_result.revision = 0;
    bridge_done = true;
    return AZ_OK;
}
int azsdk_exit_recall_poll(unsigned deck, struct azexit_result *r) {
    if (deck || !bridge_done) return AZ_PENDING;
    *r = bridge_result;
    return AZ_OK;
}
int azsdk_exit_recall_release(unsigned unused_0, uint64_t unused_1) {
    (void) unused_0;
    (void) unused_1;
    bridge_done = false;
    return AZ_OK;
}
static void *install(uintptr_t unused_0, void *unused_1, void **unused_2) {
    (void) unused_2;
    (void) unused_0;
    (void) unused_1;
    return NULL;
}
static struct azexit_cue_request request(uint64_t token, unsigned operation, unsigned classification, unsigned id, bool selected) {
    struct azexit_cue_request r = {0};
    r.size = sizeof r;
    r.operation = operation;
    r.identity.token = token;
    r.identity.load_generation = generation;
    r.identity.deadline_ns = UINT64_MAX;
    r.classification = classification;
    r.entry_id = id;
    r.flags = AZ_COMMAND_NATIVE_LOAD;
    if (operation != AZ_CMD_MEMORY_RECALL && operation != AZ_CMD_SAVED_RECALL) r.flags |= AZ_COMMAND_PERSIST;
    if (selected) {
        r.flags |= AZ_COMMAND_NATIVE_CUE_IDS;
        r.expected_revision = revision;
    }
    return r;
}
static void dispatch(const struct azexit_cue_request *r) {
    assert(azsdk_cue_commands_submit(r) == AZ_PENDING);
    azsdk_cue_actor_drive(0);
}
static struct azexit_result result(uint64_t token) {
    struct azexit_result r = {0};
    assert(azsdk_cue_commands_poll(0, &r) == AZ_OK);
    assert(r.token == token);
    return r;
}
static void release(uint64_t token) { assert(azsdk_cue_commands_release(0, token) == AZ_OK); }
static void reply() {
    receipt = true;
    persist.ready(0, active_persist.identity.token, NULL);
    azsdk_cue_commands_tick();
}
static void background_query_priority() {
    // A delayed read must not consume the public write's action budget. The
    // native actor retains the cancelled read's tombstone through destruction.
    ++generation;
    hold_background_query = true;
    azsdk_cue_commands_tick();
    assert(phase == 1 && mailbox.operation == 0);
    const uint64_t query = mailbox.identity.token;
    const unsigned before = persist_calls;
    __auto_type r = request(9100, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    const uint64_t deadline = r.identity.deadline_ns;
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(cancelled_background_query == query);
    assert(persist_calls == before + 1 && active_persist.identity.token == 9100);
    assert(compare_timestamps(active_persist.identity.deadline_ns, deadline, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    __auto_type second = request(9101, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
    reply();
    assert(result(9100).outcome == AZ_COMMAND_APPLIED);
    release(9100);
    hold_background_query = false;
    ++generation;
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick();
    // Failure to acquire or release the query's owner must retain both the
    // query identity and the copied public request until the existing drain.
    for (unsigned fault = 0; fault < 2; ++fault) {
        ++generation;
        hold_background_query = true;
        azsdk_cue_commands_tick();
        assert(phase == 1 && mailbox.operation == 0);
        background_cancel_busy = fault == 0;
        background_release_busy = fault == 1;
        const unsigned effects = persist_calls;
        r = request(9102 + fault, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
        assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
        assert(persist_calls == effects);
        second = request(9200 + fault, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
        assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
        hold_background_query = false;
        for (unsigned attempt = 0; attempt < 4 && persist_calls == effects; ++attempt) azsdk_cue_commands_tick();
        assert(persist_calls == effects + 1 && active_persist.identity.token == r.identity.token);
        assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
        reply();
        assert(result(r.identity.token).outcome == AZ_COMMAND_APPLIED);
        release(r.identity.token);
    }
    // Native actor frames share their owner lock/repository queue across decks.
    // Another deck's queued read must also yield to this public write.
    __auto_type saved_rows = rows;
    ++generation;
    loaded_decks = 2;
    hold_background_query = true;
    azsdk_cue_commands_tick();
    assert(phase == 1 && mailbox.operation == 0 && mailbox.identity.deck == 1);
    const uint64_t other_query = mailbox.identity.token;
    loaded_decks = 3;
    const unsigned effects = persist_calls;
    __auto_type cross = request(9300, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    assert(azsdk_cue_commands_submit(&cross) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(cancelled_background_query == other_query);
    assert(persist_calls == effects + 1 && active_persist.identity.token == cross.identity.token);
    const unsigned attempts = background_attempts[1];
    for (unsigned tick = 0; tick < 4; ++tick) azsdk_cue_commands_tick();
    assert(background_attempts[1] == attempts);
    assert(compare_timestamps(active_persist.identity.deadline_ns, cross.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    reply();
    assert(result(cross.identity.token).outcome == AZ_COMMAND_APPLIED);
    // A terminal public result held by a mod must not suppress other decks'
    // snapshot attempts after the actual native work has settled.
    azsdk_cue_commands_tick();
    assert(background_attempts[1] > attempts);
    release(cross.identity.token);
    hold_background_query = false;
    loaded_decks = 1;
    rows = saved_rows;
    ++generation;
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick();
    puts("PASS public cue writes retire queued reads across decks, retain their deadline and restore background reads after native completion");
}
static void management_admission() {
    __auto_type r = request(9000, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    const uint64_t deadline = r.identity.deadline_ns;
    const unsigned before = persist_calls;
    enqueue_busy = 3; // Shared owner lock busy, without this deck's refresh flag.
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    r.operation = AZ_CMD_HOT_CLEAR; // Admission owns a complete request copy.
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        azsdk_cue_commands_tick();
        struct azexit_result out = {0};
        assert(azsdk_cue_commands_poll(0, &out) == AZ_PENDING);
        assert(persist_calls == before);
    }
    for (unsigned attempt = 0; attempt < 8 && persist_calls == before; ++attempt) azsdk_cue_commands_tick();
    assert(persist_calls == before + 1);
    assert(active_persist.operation == AZ_CMD_MEMORY_SET);
    assert(compare_timestamps(active_persist.identity.deadline_ns, deadline, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    __auto_type second = request(9001, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
    reply();
    assert(result(9000).outcome == AZ_COMMAND_APPLIED);
    release(9000);
    assert(persist_calls == before + 1);
    // The admission slot protects both directly accepted and queued commands
    // until release; a second public request cannot hide the first result.
    r = request(9002, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    dispatch(&(r));
    assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
    reply();
    assert(result(9002).outcome == AZ_COMMAND_APPLIED);
    assert(azsdk_cue_commands_submit(&second) == AZ_BUSY);
    assert(azsdk_cue_commands_release(0, 9001) != AZ_OK);
    release(9002);
    const unsigned effects = persist_calls;
    enqueue_busy = 1;
    r = request(9003, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    assert(azsdk_cue_commands_cancel(0, 9004) == AZ_STALE);
    assert(azsdk_cue_commands_cancel(0, 9003) == AZ_OK);
    assert(result(9003).outcome == AZ_COMMAND_CANCELLED);
    assert(azsdk_cue_commands_release(0, 9004) == AZ_STALE);
    release(9003);
    assert(persist_calls == effects);
    // Waiting never renews the accepted deadline or follows a replacement load.
    enqueue_busy = 1;
    azsdk_cue_commands_test_time(100);
    r = request(9004, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    r.identity.deadline_ns = 200;
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    azsdk_cue_commands_test_time(200);
    azsdk_cue_commands_tick();
    assert(result(9004).outcome == AZ_COMMAND_REJECTED && result(9004).reason == AZ_STALE);
    release(9004);
    assert(persist_calls == effects);
    azsdk_cue_commands_test_time(0);
    enqueue_busy = 1;
    r = request(9005, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    ++generation;
    azsdk_cue_commands_tick();
    assert(result(9005).outcome == AZ_COMMAND_REJECTED && result(9005).reason == AZ_STALE);
    release(9005);
    assert(persist_calls == effects);
    puts("PASS public cue admission retains one copied request through contention, cancellation, deadline, load change and release");
}
static void unchanged_snapshot_refresh() {
    __auto_type query = request(UINT64_C(0x8000000000009500), 0, AZ_CUE_MEMORY, 0, false);
    struct azexit_result out = {0};
    const unsigned converted = property_calls;
    fail_property = 1;
    assert(actor.execute(&query, manager, &out, NULL) == AZ_OK);
    assert(property_calls == converted && out.revision == revision);
    fail_property = -1;
    ++revision;
    assert(actor.execute(&query, manager, &out, NULL) == AZ_OK);
    assert(property_calls == converted + rows.count);
    ++generation;
    query.identity.load_generation = generation;
    const unsigned before_load = property_calls;
    assert(actor.execute(&query, manager, &out, NULL) == AZ_OK);
    assert(property_calls == before_load + rows.count);
    fail_snapshot = true;
    assert(actor.execute(&query, manager, &out, NULL) == AZ_STALE);
    fail_snapshot = false;
    puts("PASS unchanged native cue publications renew observations without seek conversion; revision/load/source changes require fresh capture");
}
static void selected_property_conversion() {
    __auto_type saved = rows;
    rows.count = 0;
    for (unsigned entry = 1; entry <= 8; ++entry) rows_insert(&rows, rows.count, row(0, entry * 48000, false, "", 0));
    for (unsigned slot = 1; slot <= 8; ++slot) rows_insert(&rows, rows.count, row(slot, (slot + 8) * 48000, false, "", 0));
    ++revision;
    {
        unsigned range_values_5[] = {AZ_CUE_MEMORY, AZ_CUE_HOT};
        for (unsigned range_index_5 = 0; range_index_5 < sizeof(range_values_5) / sizeof(range_values_5)[0]; ++range_index_5) {
            unsigned classification = range_values_5[range_index_5];

            const size_t ordinal = classification == AZ_CUE_MEMORY ? 7 : 15;
            __auto_type r = request(9400 + classification, AZ_CMD_CUE_COMMENT, classification, 8, true);
            strcpy(r.comment, "selected note");
            const unsigned converted = property_calls, projected = canonical_calls;
            fail_property = 1; // An unrelated row must not enter native conversion.
            dispatch(&(r));
            assert(property_calls == converted + 1 && canonical_calls == projected + 1);
            assert(active_persist.identity.token == r.identity.token && active_persist.entry_id == 8);
            assert(native_kind == (classification == AZ_CUE_HOT ? 8u : 0u));
            assert(compare_timestamps(submitted_sample, (ordinal + 1) * 48000, 48000, 48000, 1) == AZ_TIME_EQUAL);
            assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
            fail_property = -1;
            put(uintptr_t, &rows.data[ordinal].cue, 0x70, (uintptr_t) ("selected note"));
            ++revision;
            const unsigned before_confirmation = property_calls;
            reply();
            assert(result(r.identity.token).outcome == AZ_COMMAND_APPLIED);
            assert(property_calls == before_confirmation);
            release(r.identity.token);
        }
    }
    // Even a correlated root-only reply cannot acknowledge an unrelated full
    // comment change beyond the public snapshot's 127-byte prefix.
    char old_tail[132] = {0}, new_tail[132] = {0};
    memset(old_tail, 'x', 130);
    memset(new_tail, 'x', 130);
    old_tail[130] = 'A';
    new_tail[130] = 'B';
    put(uintptr_t, &rows.data[8].cue, 0x70, (uintptr_t) (old_tail));
    ++revision;
    for (unsigned fault = 0; fault < 2; ++fault) {
        __auto_type r = request(9420 + fault, AZ_CMD_CUE_COMMENT, AZ_CUE_MEMORY, 8, true);
        strcpy(r.comment, "next selected note");
        dispatch(&(r));
        put(uintptr_t, &rows.data[7].cue, 0x70, (uintptr_t) ("next selected note"));
        publication_matches = fault != 0;
        publication_mode = 2;
        if (fault) put(uintptr_t, &rows.data[8].cue, 0x70, (uintptr_t) (new_tail));
        ++revision;
        reply();
        assert(result(r.identity.token).outcome == AZ_COMMAND_UNCERTAIN);
        release(r.identity.token);
        publication_matches = true;
        publication_mode = 1;
        put(uintptr_t, &rows.data[8].cue, 0x70, (uintptr_t) (old_tail));
        ++revision;
    }
    const unsigned converted = property_calls, effects = persist_calls;
    __auto_type absent = request(9410, AZ_CMD_CUE_COMMENT, AZ_CUE_MEMORY, 9, true);
    dispatch(&(absent));
    assert(result(absent.identity.token).reason == AZ_STALE && persist_calls == effects && property_calls == converted);
    release(absent.identity.token);
    rows = saved;
    ++generation;
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick();
    puts("PASS revision-qualified selectors convert only the selected native property and preserve identity/deadline");
}
static void hot_set_publication() {
    override_hot_view = override_stage_sample = true;
    {
        unsigned range_values_6[] = {44100u, 48000u, 96000u, 192000u};
        for (unsigned range_index_6 = 0; range_index_6 < sizeof(range_values_6) / sizeof(range_values_6)[0]; ++range_index_6) {
            unsigned rate = range_values_6[range_index_6];
            for (unsigned fault = 0; fault < 7; ++fault) {
                Row target = row(8, rate == 44100 ? 115274 : 3u * rate, false, "", 0);
                uint64_t sample;
                memcpy(&sample, target.property.bytes + 0x40, 8);
                put(uint32_t, &target.property, 0x60, rate);
                put(uint32_t, &target.property, 0xb8, rate);
                put(uint64_t, &target.property, 0x68, rate == 44100 ? 2613923 : 3000000);
                stage_sample = (__typeof__(stage_sample)) {};
                stage_sample.in_sample = sample;
                stage_sample.sample_rate = rate;
                memcpy(stage_sample.source_id, target.property.bytes + 0x30, 12);
                rows.data[rows.count - 1] = target;
                hot_view = target.property;
                missing_hot_view = fault == 0;
                if (fault == 1) put(uint64_t, &hot_view, 0x40, sample + 1); // Exact sample boundary is not equal.
                if (fault == 2) hot_view.bytes[0x30] ^= 2;
                if (fault == 3) put(uint32_t, &hot_view, 0x60, rate * 2);
                if (fault == 4) put(uint64_t, &hot_view, 0x98, sample + rate);
                __auto_type r = request(1000 + rate + fault, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8, false);
                unsigned before = persist_calls, before_stages = stages;
                dispatch(&(r));
                fail_snapshot = fault == 5;
                fail_witness = fault == 6;
                bridge_done = true;
                azsdk_cue_commands_tick();
                struct azexit_result pending = {0};
                assert(azsdk_cue_commands_poll(0, &pending) == AZ_PENDING && persist_calls == before && stages == before_stages + 1);
                missing_hot_view = false;
                fail_snapshot = false;
                fail_witness = false;
                hot_view = target.property;
                azsdk_cue_commands_tick();
                assert(persist_calls == before + 1 && stages == before_stages + 1 && compare_timestamps(submitted_sample, sample, rate, rate, 1) == AZ_TIME_EQUAL);
                assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
                reply();
                assert(result(r.identity.token).outcome == AZ_COMMAND_APPLIED);
                release(r.identity.token);
            }
        }
    }
    // The DSP setter can queue its own native save before the SDK's correlated
    // persistence step. Busy retries must not repeat that setter or its save.
    {
        int range_values_7[] = {AZ_BUSY, AZ_STALE};
        for (unsigned range_index_7 = 0; range_index_7 < sizeof(range_values_7) / sizeof(range_values_7)[0]; ++range_index_7) {
            int transient = range_values_7[range_index_7];

            __auto_type r = request(3000 + (unsigned) (-transient), AZ_CMD_HOT_SET, AZ_CUE_HOT, 8, false);
            unsigned before = persist_calls, before_stages = stages;
            dispatch(&(r));
            persist_busy = transient == AZ_BUSY ? 3 : 0;
            persist_stale = transient == AZ_STALE ? 3 : 0;
            bridge_done = true;
            for (unsigned retry = 0; retry < 3; ++retry) {
                azsdk_cue_commands_tick();
                struct azexit_result pending = {0};
                assert(azsdk_cue_commands_poll(0, &pending) == AZ_PENDING);
                assert(persist_calls == before && stages == before_stages + 1);
                ++revision;
            }
            azsdk_cue_commands_tick();
            assert(persist_calls == before + 1 && stages == before_stages + 1);
            assert(submitted_witness.revision == revision);
            assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
            reply();
            assert(result(r.identity.token).outcome == AZ_COMMAND_APPLIED);
            release(r.identity.token);
        }
    }
    for (unsigned failure = 0; failure < 8; ++failure) {
        azsdk_cue_commands_test_time(100);
        __auto_type r = request(2000 + failure, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8, false);
        r.identity.deadline_ns = 110;
        unsigned before = persist_calls;
        missing_hot_view = failure < 3;
        dispatch(&(r));
        fail_stage_capture = failure == 2;
        persist_busy = failure == 3 ? 3 : 0;
        persist_stale = failure == 4 || failure == 5 ? 3 : 0;
        fail_witness = failure >= 6;
        bridge_done = true;
        azsdk_cue_commands_tick();
        if (failure != 2) {
            struct azexit_result pending = {0};
            assert(azsdk_cue_commands_poll(0, &pending) == AZ_PENDING);
            if (failure == 0 || failure == 3 || failure == 4 || failure == 6) azsdk_cue_commands_test_time(r.identity.deadline_ns);
            else ++generation;
            azsdk_cue_commands_tick();
        }
        __auto_type out = result(r.identity.token);
        assert(out.outcome != AZ_COMMAND_APPLIED && out.reason == (failure == 2 ? AZ_FAILED : AZ_STALE));
        assert(persist_calls == before);
        release(r.identity.token);
        if (failure == 1 || failure == 5 || failure == 7) --generation;
        fail_stage_capture = false;
        persist_busy = 0;
        persist_stale = 0;
        fail_witness = false;
        azsdk_cue_commands_test_time(0);
        azsdk_cue_commands_tick();
        azsdk_cue_commands_tick();
    }
    missing_hot_view = false;
    override_hot_view = override_stage_sample = false;
    puts("PASS HOT_SET waits for its DSP-captured source point, rate and shape before persistence");
}
static void cue_snapshot_clock_tail() {
    struct az_native_cue_snapshot current = {0};
    current.size = sizeof current;
    assert(azsdk_cue_commands_native_cues(0, &current) == AZ_OK && current.source_rate_hz == 48000);
    typedef struct Legacy Legacy;
    struct Legacy {
        uint32_t size, version, deck, count;
        uint64_t native_load_generation, revision;
        struct az_cue_entry entries[AZ_NATIVE_CUE_LIMIT];
        uint64_t canary;
    } old = {0};
    _Static_assert(offsetof(Legacy, canary) == AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE, "original native snapshot prefix");
    old.size = AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE;
    old.canary = UINT64_C(0x0123456789abcdef);
    assert(azsdk_cue_commands_native_cues(0, ((struct az_native_cue_snapshot *) (&old))) == AZ_OK);
    assert(old.size == AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE && old.count == current.count && old.revision == current.revision && old.canary == UINT64_C(0x0123456789abcdef));
    old.size = AZ_NATIVE_CUE_SNAPSHOT_V1_SIZE - 1;
    Legacy before = old;
    assert(azsdk_cue_commands_native_cues(0, ((struct az_native_cue_snapshot *) (&old))) == AZ_INVALID && !memcmp(&old, &before, sizeof old));
    struct Extended {
        struct az_native_cue_snapshot value;
        uint64_t canary;
    } extended = {0};
    extended.value.size = sizeof extended;
    extended.canary = UINT64_C(0xfedcba9876543210);
    assert(azsdk_cue_commands_native_cues(0, &extended.value) == AZ_OK);
    assert(extended.value.size == sizeof extended && extended.value.source_rate_hz == 48000 && extended.canary == UINT64_C(0xfedcba9876543210));
    // Same cue revision can acquire its previously unavailable source clock.
    // The unchanged-publication fast path must not retain an old clock value.
    source_clock = 44100;
    azsdk_cue_commands_test_time(UINT64_C(1000000000));
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick();
    current.size = sizeof current;
    assert(azsdk_cue_commands_native_cues(0, &current) == AZ_OK && current.source_rate_hz == 44100);
    source_clock = 48000;
    rows.count = 0;
    ++revision;
    azsdk_cue_commands_test_time(UINT64_C(1200000000));
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick();
    assert(azsdk_cue_commands_native_cues(0, &current) == AZ_OK && !current.count && current.source_rate_hz == 48000);
    azsdk_cue_commands_test_time(0);
    puts("PASS native cue snapshot source clock: old-prefix and unknown-tail canaries, clock refresh and empty bank");
}
static void arbitrary_creation() {
    __auto_type main_before = main_property;
    unsigned writes = persist_calls, dsp = stages;
    __auto_type r = request(2000, AZ_CMD_CUE_POINT_SET, AZ_CUE_MEMORY, 0, true);
    r.position_s = 0;
    strcpy(r.comment, "Start");
    r.rgb = 0xff0000;
    dispatch(&(r));
    assert(persist_calls == writes + 1 && compare_timestamps(submitted_sample, 0, 48000, 48000, 1) == AZ_TIME_EQUAL && stages == dsp);
    rows_insert(&rows, 1, row(0, 0, false, "Start", r.rgb));
    ++revision;
    reply();
    assert(result(2000).outcome == AZ_COMMAND_APPLIED);
    release(2000);
    assert(!memcmp(&main_property, &main_before, sizeof main_before));
    r = request(2001, AZ_CMD_CUE_LOOP_SET, AZ_CUE_MEMORY, 0, true);
    r.position_s = 4;
    r.end_s = 6;
    dispatch(&(r));
    Row loop = row(0, 192000, true, "", 0);
    put(uint64_t, &loop.property, 0x98, 288000);
    put(uint64_t, &loop.property, 0xc0, 6000000);
    rows_insert(&rows, 2, loop);
    ++revision;
    reply();
    assert(result(2001).outcome == AZ_COMMAND_APPLIED);
    release(2001);
    r = request(2002, AZ_CMD_CUE_POINT_SET, AZ_CUE_HOT, 7, true);
    r.position_s = 8;
    strcpy(r.comment, "Hot \342\234\223");
    unsigned loads = recalls;
    dispatch(&(r));
    rows_insert(&rows, rows.count, row(7, 384000, false, "Hot \342\234\223", 0));
    ++revision;
    reply();
    struct azexit_result pending_result = {0};
    assert(azsdk_cue_commands_poll(0, &pending_result) == AZ_PENDING && recalls == loads + 1);
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(result(2002).outcome == AZ_COMMAND_APPLIED);
    release(2002);
    // Stored and currently unsaved Hot occupancy refuse before writing.
    r = request(2003, AZ_CMD_CUE_POINT_SET, AZ_CUE_HOT, 8, true);
    r.position_s = 10;
    writes = persist_calls;
    dispatch(&(r));
    assert(result(2003).reason == AZ_LIMIT && persist_calls == writes);
    release(2003);
    override_hot_view = true;
    missing_hot_view = false;
    fixture_property(&hot_view, 123456, false);
    r = request(2004, AZ_CMD_CUE_POINT_SET, AZ_CUE_HOT, 6, true);
    r.position_s = 10;
    dispatch(&(r));
    assert(result(2004).reason == AZ_LIMIT && persist_calls == writes);
    release(2004);
    override_hot_view = false;
    r = request(2005, AZ_CMD_CUE_POINT_SET, AZ_CUE_MEMORY, 0, true);
    r.position_s = 0;
    dispatch(&(r));
    assert(result(2005).reason == AZ_LIMIT && persist_calls == writes);
    release(2005);
    r = request(2006, AZ_CMD_CUE_LOOP_SET, AZ_CUE_MEMORY, 0, true);
    r.position_s = 10;
    r.end_s = 9;
    dispatch(&(r));
    assert(result(2006).reason == AZ_INVALID && persist_calls == writes);
    release(2006);
    r = request(2007, AZ_CMD_CUE_POINT_SET, AZ_CUE_MEMORY, 0, true);
    r.position_s = 10;
    --r.expected_revision;
    dispatch(&(r));
    assert(result(2007).reason == AZ_STALE && persist_calls == writes);
    release(2007);
    r = request(2008, AZ_CMD_HOT_CLEAR, AZ_CUE_HOT, 7, true);
    --r.expected_revision;
    unsigned stages_before = stages;
    dispatch(&(r));
    assert(result(2008).reason == AZ_STALE && stages == stages_before);
    release(2008);
    // Cancellation after the storage receipt is a partial application even
    // while the playable Hot update is still queued. Never claim no effect.
    r = request(2009, AZ_CMD_CUE_LOOP_SET, AZ_CUE_HOT, 5, true);
    r.position_s = 12;
    r.end_s = 14;
    dispatch(&(r));
    Row created = row(5, 576000, true, "", 0);
    put(uint64_t, &created.property, 0x98, 672000);
    put(uint64_t, &created.property, 0xc0, 14000000);
    rows_insert(&rows, rows.count, created);
    ++revision;
    reply();
    bridge_cancel_success = true;
    assert(azsdk_cue_commands_cancel(0, 2009) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(result(2009).outcome == AZ_COMMAND_UNCERTAIN);
    release(2009);
    r = request(2010, AZ_CMD_MEMORY_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(&(r));
    assert(azsdk_cue_commands_cancel(0, 2010) == AZ_OK);
    assert(result(2010).outcome == AZ_COMMAND_CANCELLED);
    release(2010);
    r = request(2011, AZ_CMD_CUE_POINT_SET, AZ_CUE_MEMORY, 0, true);
    r.position_s = 16;
    strcpy(r.comment, "Required label");
    dispatch(&(r));
    rows_insert(&rows, rows.count, row(0, 768000, false, "", 0));
    ++revision;
    reply();
    assert(result(2011).outcome == AZ_COMMAND_UNCERTAIN);
    release(2011); // Empty canonical label cannot prove requested metadata.
    azsdk_cue_commands_tick();
    bridge_cancel_success = false;
    puts("PASS arbitrary source-time cue creation: zero, loop bounds, revision, occupancy, preservation and Hot DSP settlement");
}
static void cache_lock_held(void *context) {
    _Atomic(unsigned) *signal = context;
    atomic_store_explicit(signal, 1, memory_order_release);
    while (atomic_load_explicit(signal, memory_order_acquire) != 2) sched_yield();
}
static void *hold_cache_lock(void *context) {
    azsdk_cue_commands_test_cache_lock(0, cache_lock_held, context);
    return NULL;
}
static unsigned settled_receipts;
static struct azexit_receipt last_settled;
int azsdk_exit_delivery_publish(const struct azexit_receipt *receipt) {
    assert(receipt && receipt->size == sizeof *receipt);
    last_settled = *receipt;
    ++settled_receipts;
    return AZ_OK;
}
static void retirement_settlement(void) {
    struct azexit_cue_request r = request(7000, AZ_CMD_SAVED_SAVE, AZ_CUE_MEMORY, 0, false);
    dispatch(&r);
    azsdk_cue_commands_actor_retired(0, r.identity.token, r.identity.load_generation);
    azsdk_cue_commands_tick();
    assert(!settled_receipts); // repository ownership still fences the actor's retirement
    azsdk_cue_commands_persist_retired(0, r.identity.token);
    azsdk_cue_commands_tick();
    assert(settled_receipts == 1 && last_settled.token == r.identity.token && last_settled.load_generation == r.identity.load_generation);
    assert(last_settled.kind == AZEXIT_RECEIPT_SETTLED);
    azsdk_cue_commands_tick();
    assert(settled_receipts == 1); // settlement is consumed exactly once
}
int main(int argc, char **argv) {
    fixture_property(&(main_property), 48000, true);
    char long_comment[130] = {0};
    memset(long_comment, 'x', 126);
    memcpy(long_comment + 126, "\xe2\x82\xac", 4);
    rows = (Rows) {.count = 2, .data = {row(0, 48000, true, "", 0), row(8, 96000, false, long_comment, 0x123456)}};
    assert(azsdk_cue_commands_configure(1, 1, install) == AZ_OK);
    assert(azsdk_cue_commands_commands() == (all_persist() | (UINT64_C(1) << AZ_CMD_SAVED_RECALL) | (UINT64_C(1) << AZ_CMD_MEMORY_RECALL)));
    azsdk_cue_commands_tick();
    azsdk_cue_commands_tick(); // refresh captures scalar IDs; second releases internal result
    if (argc == 2) {
        if (!strcmp(argv[1], "retirement")) {
            retirement_settlement();
            return 0;
        }
        if (!strcmp(argv[1], "clock")) {
            cue_snapshot_clock_tail();
            return 0;
        }
        if (!strcmp(argv[1], "create")) {
            arbitrary_creation();
            return 0;
        }
        if (!strcmp(argv[1], "priority")) {
            background_query_priority();
            return 0;
        }
        if (!strcmp(argv[1], "admission")) {
            management_admission();
            return 0;
        }
        if (!strcmp(argv[1], "selection")) {
            selected_property_conversion();
            return 0;
        }
        if (!strcmp(argv[1], "refresh")) {
            unchanged_snapshot_refresh();
            return 0;
        }
        assert(!strcmp(argv[1], "hot-set"));
        hot_set_publication();
        return 0;
    }
    assert(argc == 1);
    struct az_native_cue_snapshot snapshot = {0};
    snapshot.size = sizeof snapshot;
    assert(azsdk_cue_commands_native_cues(0, &snapshot) == AZ_OK && snapshot.count == 2);
    assert(snapshot.entries[0].entry_id == 1 && snapshot.entries[0].type == AZ_CUE_LOOP &&
           compare_timestamps(snapshot.entries[0].start_s, 1., 1, 48000, 1) == AZ_TIME_EQUAL &&
           compare_timestamps(snapshot.entries[0].end_s, 2., 1, 48000, 1) == AZ_TIME_EQUAL);
    assert(strlen(snapshot.entries[1].comment) == 126);
    assert(snapshot.entries[1].entry_id == 8 && snapshot.entries[1].slot == 8 && snapshot.entries[1].classification == AZ_CUE_HOT);
    struct azexit_cue_target target = {0};
    assert(azsdk_cue_commands_target_cue(0, &target) == AZ_OK);
    assert(!(target.persistent_commands & (UINT64_C(1) << AZ_CMD_MEMORY_RECALL)) && !(target.revision_commands & (UINT64_C(1) << AZ_CMD_HOT_SET)));
    _Atomic(unsigned) target_lock = 0;
    pthread_t lock_holder;
    assert(!pthread_create(&lock_holder, NULL, hold_cache_lock, &target_lock));
    while (!atomic_load_explicit(&(target_lock), memory_order_acquire)) sched_yield();
    int target_status = azsdk_cue_commands_target_cue(0, &target);
    atomic_store_explicit(&(target_lock), 2, memory_order_release);
    assert(!pthread_join(lock_holder, NULL));
    assert(target_status == AZ_BUSY);
    assert(azsdk_cue_commands_target_cue(0, &target) == AZ_OK && target.revision_valid && target.revision == revision);
    __auto_type fence_request = request(80, AZ_CMD_CUE_COMMENT, AZ_CUE_MEMORY, 1, true);
    strcpy(fence_request.comment, "fenced");
    fail_witness = true;
    dispatch(&(fence_request));
    assert(result(80).reason == AZ_STALE && persist_calls == 0);
    release(80);
    fail_witness = false;
    __auto_type r = request(1, AZ_CMD_CUE_COLOR, AZ_CUE_HOT, 8, true);
    r.rgb = 0xaabbcc;
    dispatch(&(r));
    assert(native_kind == 8 && persist_calls == 1);
    assert(submitted_witness.revision == r.expected_revision && witness_calls == 2);
    assert(azsdk_cue_commands_cancel(0, 1) == AZ_PENDING);
    put(uint32_t, &rows.data[1].cue, 0x78, 0xffaabbcc);
    ++revision;
    // A receipt must not be lost when the owner frame is still executing.
    receipt = true;
    resume_busy = 2;
    persist.ready(0, active_persist.identity.token, NULL);
    struct azexit_result deferred = {0};
    assert(azsdk_cue_commands_poll(0, &deferred) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(azsdk_cue_commands_poll(0, &deferred) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(azsdk_cue_commands_poll(0, &deferred) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(persist_calls == 1 && compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    assert(result(1).outcome == AZ_COMMAND_APPLIED);
    release(1);
    r = request(2, AZ_CMD_CUE_COMMENT, AZ_CUE_HOT, 8, true);
    strcpy(r.comment, "edited");
    dispatch(&(r));
    // A receipt alone cannot claim an effect absent from the refreshed store.
    reply();
    assert(result(2).outcome == AZ_COMMAND_UNCERTAIN);
    release(2);
    r = request(3, AZ_CMD_MEMORY_DELETE, AZ_CUE_MEMORY, 1, true);
    r.expected_revision = revision - 1;
    dispatch(&(r));
    assert(result(3).reason == AZ_STALE);
    release(3);
    // Hot SET must run the DSP stage even when no main cue exists.
    main_valid = false;
    r = request(4, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8, false);
    dispatch(&(r));
    assert(stages == 1 && persist_calls == 2);
    rows.data[1] = row(8, 144000, false, "", 0);
    ++revision;
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(persist_calls == 3 && native_kind == 8);
    reply();
    assert(result(4).outcome == AZ_COMMAND_APPLIED);
    release(4);
    // Hot CLEAR retains the old native property until after its live DSP clear.
    r = request(5, AZ_CMD_HOT_CLEAR, AZ_CUE_HOT, 8, false);
    dispatch(&(r));
    assert(stages == 2);
    (assert(rows.count), --rows.count);
    ++revision;
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(persist_calls == 4 && native_kind == 8);
    reply();
    assert(result(5).outcome == AZ_COMMAND_APPLIED);
    release(5);
    main_valid = true;
    r = request(6, AZ_CMD_MEMORY_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(&(r));
    assert(recalls == 1);
    struct azexit_result pending_result = {0};
    assert(azsdk_cue_commands_poll(0, &pending_result) == AZ_PENDING);
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(result(6).outcome == AZ_COMMAND_APPLIED && result(6).revision == r.expected_revision);
    release(6);
    r = request(7, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 1, true);
    r.position_s = 1;
    dispatch(&(r));
    rows.data[0].cue.bytes[0x28] = 1;
    ++revision;
    reply();
    assert(result(7).outcome == AZ_COMMAND_APPLIED);
    release(7);
    r = request(8, AZ_CMD_MEMORY_DELETE, AZ_CUE_MEMORY, 1, true);
    dispatch(&(r));
    rows.count = 0;
    ++revision;
    reply();
    assert(result(8).outcome == AZ_COMMAND_APPLIED);
    release(8);
    r = request(9, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    r.position_s = 1;
    assert(azsdk_cue_commands_submit(&r) == AZ_INVALID);
    r = request(10, AZ_CMD_HOT_SET, AZ_CUE_HOT, 9, false);
    assert(azsdk_cue_commands_submit(&r) == AZ_INVALID);
    r = request(11, AZ_CMD_CUE_COLOR, AZ_CUE_MEMORY, 1, true);
    r.rgb = 0x1000000;
    assert(azsdk_cue_commands_submit(&r) == AZ_INVALID);
    r.rgb = 0x2ba6d9;
    unsigned before_palette = persist_calls;
    assert(azsdk_cue_commands_submit(&r) == AZ_UNSUPPORTED);
    assert(persist_calls == before_palette);
    rows = (Rows) {.count = 1, .data = {row(0, 48000, true, "retained cue \xe2\x9c\x93", 0)}};
    ++revision;
    const uint32_t palette[] = {0, 0xff00c6, 0xff0000, 0xff7100, 0xffff00, 0x00e300, 0x00c3ff, 0x0051ff, 0x9c08ff};
    for (unsigned i = 0; i < 9; ++i) {
        r = request(1000 + i, AZ_CMD_CUE_COLOR, AZ_CUE_MEMORY, 1, true);
        r.rgb = palette[i];
        dispatch(&(r));
        assert(persist_calls == before_palette + i + 1);
        put(uint32_t, &rows.data[0].cue, 0x78, 0xff000000u | r.rgb);
        rows.data[0].cue.bytes[0x7c] = i;
        ++revision;
        reply();
        assert(result(1000 + i).outcome == AZ_COMMAND_APPLIED);
        release(1000 + i);
    }
    r = request(12, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 1, true);
    r.position_s = 0.5;
    assert(azsdk_cue_commands_submit(&r) == AZ_INVALID);
    r = request(13, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    dispatch(&(r));
    rows = (Rows) {.count = 1, .data = {row(0, 48000, true, "", 0)}};
    ++revision;
    reply();
    assert(result(13).outcome == AZ_COMMAND_APPLIED);
    release(13);
    r = request(14, AZ_CMD_SAVED_SAVE, AZ_CUE_MEMORY, 0, false);
    dispatch(&(r));
    reply();
    assert(result(14).outcome == AZ_COMMAND_APPLIED);
    release(14);
    revision = 50;
    unsigned before_recall_projection = canonical_calls;
    r = request(15, AZ_CMD_SAVED_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(&(r));
    assert(recalls == 2);
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(result(15).outcome == AZ_COMMAND_APPLIED && result(15).revision == 50);
    assert(canonical_calls == before_recall_projection);
    release(15);
    r = request(90, AZ_CMD_MEMORY_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(&(r));
    bridge_result.token = 91;
    bridge_done = true;
    azsdk_cue_commands_tick();
    struct azexit_result no_match = {0};
    assert(azsdk_cue_commands_poll(0, &no_match) == AZ_PENDING);
    bridge_result.token = 90;
    bridge_result.load_generation = generation + 1;
    azsdk_cue_commands_tick();
    assert(result(90).outcome == AZ_COMMAND_UNCERTAIN && result(90).revision == 0 && result(90).reason == AZ_STALE);
    release(90);
    r = request(91, AZ_CMD_MEMORY_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(&(r));
    bridge_result.outcome = AZ_COMMAND_UNCERTAIN; // Native DSP source/seek correlation failed.
    bridge_result.reason = AZ_STALE;
    bridge_result.revision = 0;
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(result(91).outcome == AZ_COMMAND_UNCERTAIN && result(91).revision == 0);
    release(91);
    r = request(16, AZ_CMD_CUE_COMMENT, AZ_CUE_MEMORY, 1, true);
    strcpy(r.comment, "saved note");
    dispatch(&(r));
    put(uintptr_t, &rows.data[0].cue, 0x70, (uintptr_t) ("saved note"));
    ++revision;
    reply();
    assert(result(16).outcome == AZ_COMMAND_APPLIED);
    release(16);
    r = request(17, AZ_CMD_SAVED_DELETE, AZ_CUE_MEMORY, 1, true);
    dispatch(&(r));
    rows.count = 0;
    ++revision;
    reply();
    assert(result(17).outcome == AZ_COMMAND_APPLIED);
    release(17);
    rows = (Rows) {.count = 1, .data = {row(0, 48000, false, "", 0)}};
    ++revision;
    r = request(18, AZ_CMD_SAVED_RECALL, AZ_CUE_MEMORY, 1, true);
    dispatch(&(r));
    assert(result(18).reason == AZ_INVALID);
    release(18);
    assert(recalls == 4);
    fixture_property(&(main_property), 48000, false);
    r = request(19, AZ_CMD_SAVED_SAVE, AZ_CUE_MEMORY, 0, false);
    dispatch(&(r));
    assert(result(19).reason == AZ_INVALID);
    release(19);
    // Actual 44.1kHz native persistence truncates the usec round trip by one
    // sample. The producer must still receive the untouched original property.
    fixture_property(&(main_property), 150025, false);
    put(uint32_t, &main_property, 0x60, 44100);
    put(uint64_t, &main_property, 0x68, 3401927);
    r = request(92, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    dispatch(&(r));
    assert(compare_timestamps(submitted_sample, 150025, 44100, 44100, 1) == AZ_TIME_EQUAL);
    Row projected = row(0, 150024, false, "", 0);
    put(uint32_t, &projected.property, 0x60, 44100);
    put(uint64_t, &projected.property, 0x68, 3401927);
    rows = (Rows) {.count = 1, .data = {projected}};
    ++revision;
    reply();
    assert(result(92).outcome == AZ_COMMAND_APPLIED);
    release(92);
    r = request(93, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    dispatch(&(r));
    put(uint64_t, &rows.data[0].property, 0x40, 150023);
    ++revision;
    reply();
    assert(result(93).outcome == AZ_COMMAND_UNCERTAIN);
    release(93);
    fixture_property(&(main_property), 48000, false);
    rows = (Rows) {.count = 1, .data = {row(0, 48000, false, "", 0)}};
    ++revision;
    r = request(20, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    dispatch(&(r));
    generation = 43;
    reply();
    assert(result(20).outcome == AZ_COMMAND_UNCERTAIN);
    release(20);
    snapshot.size = sizeof snapshot;
    assert(azsdk_cue_commands_native_cues(0, &snapshot) == AZ_STALE);
    // A background refresh never causes a random public BUSY rejection.
    azsdk_cue_commands_tick(); // New load: internal query now owns the actor mailbox.
    unsigned before_wait = persist_calls;
    r = request(21, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    r.operation = AZ_CMD_HOT_CLEAR; // Queued public request is an owned copy.
    azsdk_cue_commands_tick(); // Query drains; public command wins before another refresh.
    assert(persist_calls == before_wait + 1 && active_persist.operation == AZ_CMD_MEMORY_SET);
    reply();
    assert(result(21).outcome == AZ_COMMAND_APPLIED);
    release(21);
    // Cancelling the copied waiting request has no native effect and is pollable
    // even while its preceding query still occupies the native actor mailbox.
    ++generation;
    azsdk_cue_commands_tick();
    before_wait = persist_calls;
    r = request(22, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    assert(azsdk_cue_commands_cancel(0, 22) == AZ_OK);
    assert(result(22).outcome == AZ_COMMAND_CANCELLED);
    release(22);
    assert(persist_calls == before_wait);
    // Expiration while waiting rejects before any native public producer call.
    ++generation;
    azsdk_cue_commands_tick();
    r = request(23, AZ_CMD_MEMORY_SET, AZ_CUE_MEMORY, 0, false);
    r.identity.deadline_ns = 1;
    assert(azsdk_cue_commands_submit(&r) == AZ_PENDING);
    azsdk_cue_commands_tick();
    assert(result(23).outcome == AZ_COMMAND_REJECTED && result(23).reason == AZ_STALE);
    release(23);
    assert(persist_calls == before_wait);
    // The native HOT vector contains eight fixed slots, including explicit
    // absent rows. Only those markers are skipped, before native conversion.
    stored_memory_count = 2;
    rows = (Rows) {.count = 2, .data = {row(0, 48000, false, "", 0), row(0, 96000, true, "", 0)}};
    Row empty = row(0xff, 0, false, "", 0);
    put(uint64_t, &empty.cue, 8, UINT64_C(0x7fffffffffffffff));
    put(uint64_t, &empty.cue, 0x10, UINT64_C(0x7fffffffffffffff));
    for (unsigned slot = 1; slot <= 7; ++slot) rows_insert(&rows, rows.count, empty);
    rows_insert(&rows, rows.count, row(8, 192000, false, "", 0));
    // Native validity also treats either sentinel coordinate as absent.
    rows.data[3].cue.bytes[0] = 2;
    put(uint64_t, &rows.data[3].cue, 0x10, 0);
    rows.data[4].cue.bytes[0] = 3;
    put(uint64_t, &rows.data[4].cue, 8, 0);
    ++revision;
    struct azexit_cue_request query = request(UINT64_C(0x8000000000007777), 0, AZ_CUE_MEMORY, 0, false);
    struct azexit_result queried = {0};
    unsigned converted = property_calls;
    assert(actor.execute(&query, manager, &queried, NULL) == AZ_OK);
    assert(property_calls == converted + 3);
    snapshot.size = sizeof snapshot;
    assert(azsdk_cue_commands_native_cues(0, &snapshot) == AZ_OK && snapshot.count == 3);
    assert(snapshot.entries[0].entry_id == 1 && snapshot.entries[1].entry_id == 2);
    assert(snapshot.entries[2].classification == AZ_CUE_HOT && snapshot.entries[2].entry_id == 8);
    // A populated HOT row with an invalid converted position fails closed.
    rows.data[2] = row(1, 144000, false, "", 0);
    rows.data[2].property.bytes[0x30] = 0;
    ++revision;
    assert(actor.execute(&query, manager, &queried, NULL) == AZ_UNSUPPORTED);
    rows.data[2] = empty;
    // A cross-source/native conversion refusal is never interpreted as empty.
    fail_property = 10;
    assert(actor.execute(&query, manager, &queried, NULL) == AZ_UNSUPPORTED);
    fail_property = -1;
    rows.data[2] = row(9, 144000, false, "", 0);
    assert(actor.execute(&query, manager, &queried, NULL) == AZ_UNSUPPORTED);
    rows.data[2] = empty;
    rows.data[0].cue.bytes[0] = 0xff; // Absent markers are not permitted in memory rows.
    assert(actor.execute(&query, manager, &queried, NULL) == AZ_UNSUPPORTED);
    rows.data[0].cue.bytes[0] = 0;
    // A same-point native HOT duplicate with old RGB cannot be acknowledged.
    unsigned previous_stages = stages;
    r = request(81, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8, false);
    r.rgb = 0x123456;
    dispatch(&(r));
    assert(stages == previous_stages + 1);
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(submitted_witness.revision == revision && r.expected_revision == 0);
    assert(compare_timestamps(submitted_sample, 192000, 48000, 48000, 1) == AZ_TIME_EQUAL);
    reply();
    assert(result(81).outcome == AZ_COMMAND_UNCERTAIN);
    release(81);
    r = request(82, AZ_CMD_HOT_SET, AZ_CUE_HOT, 8, false);
    r.rgb = 0x123456;
    dispatch(&(r));
    bridge_done = true;
    azsdk_cue_commands_tick();
    assert(compare_timestamps(submitted_sample, 192000, 48000, 48000, 1) == AZ_TIME_EQUAL);
    put(uint32_t, &rows.data[rows.count - 1].cue, 0x78, 0xff123456);
    ++revision;
    reply();
    assert(result(82).outcome == AZ_COMMAND_APPLIED);
    release(82);
    // Enabling a saved loop durably disables each old loop through a separate
    // native request, then enables the selected loop under the same public ID.
    stored_memory_count = 3;
    rows = (Rows) {.count = 4, .data = {row(0, 48000, true, "old loop", 0x112233), row(0, 144000, true, "selected loop", 0x445566), row(0, 288000, false, "point", 0x778899), row(8, 384000, false, "hot", 0x123456)}};
    rows.data[0].cue.bytes[0x28] = 1;
    ++revision;
    unsigned before_auto = persist_calls;
    r = request(1000, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    r.identity.deadline_ns = UINT64_MAX - 100;
    dispatch(&(r));
    assert(persist_calls == before_auto + 1 && active_persist.entry_id == 1 && active_persist.position_s == 0);
    uint64_t first_cookie = cookie;
    assert(active_persist.identity.token == 1000 && compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    rows.data[0].cue.bytes[0x28] = 0;
    rows.data[0].cue.bytes[1] = 0xa5;
    rows.data[0].cue.bytes[2] = 0x5a; // Undefined native padding is not an identity/effect field.
    ++revision;
    reply();
    assert(azsdk_cue_commands_poll(0, &pending_result) == AZ_PENDING);
    assert(persist_calls == before_auto + 2 && active_persist.entry_id == 2 && active_persist.position_s == 1);
    assert(cookie != first_cookie && active_persist.expected_revision == revision);
    assert(active_persist.identity.token == 1000 && compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL);
    rows.data[1].cue.bytes[0x28] = 1;
    ++revision;
    reply();
    assert(result(1000).outcome == AZ_COMMAND_APPLIED && rows.data[0].cue.bytes[0x28] == 0 && rows.data[1].cue.bytes[0x28] == 1);
    assert(submissions[submission_count - 1].operation == AZ_CMD_SAVED_AUTO && submissions[submission_count - 2].operation == AZ_CMD_SAVED_AUTO);
    uintptr_t old_comment = 0, selected_comment = 0;
    uint32_t old_rgb = 0, selected_rgb = 0;
    memcpy(&old_comment, rows.data[0].cue.bytes + 0x70, 8);
    memcpy(&selected_comment, rows.data[1].cue.bytes + 0x70, 8);
    memcpy(&old_rgb, rows.data[0].cue.bytes + 0x78, 4);
    memcpy(&selected_rgb, rows.data[1].cue.bytes + 0x78, 4);
    assert(!strcmp(((const char *) (old_comment)), "old loop") && !strcmp(((const char *) (selected_comment)), "selected loop"));
    assert(old_rgb == 0xff112233 && selected_rgb == 0xff445566 && rows.count == 4);
    release(1000);
    // Disable changes only its selected loop, even when another loop is active.
    rows.data[0].cue.bytes[0x28] = 1;
    ++revision;
    before_auto = persist_calls;
    r = request(1001, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 0;
    dispatch(&(r));
    assert(active_persist.entry_id == 2 && active_persist.position_s == 0);
    rows.data[1].cue.bytes[0x28] = 0;
    ++revision;
    reply();
    assert(result(1001).outcome == AZ_COMMAND_APPLIED && rows.data[0].cue.bytes[0x28] == 1 && persist_calls == before_auto + 1);
    release(1001);
    // A foreign revision after our first receipt prevents the next native write.
    before_auto = persist_calls;
    r = request(1002, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(&(r));
    rows.data[0].cue.bytes[0x28] = 0;
    ++revision;
    inject_foreign_after_confirm = true;
    reply();
    assert(result(1002).outcome == AZ_COMMAND_UNCERTAIN && result(1002).reason == AZ_STALE && persist_calls == before_auto + 1);
    assert(rows.data[0].cue.bytes[0x28] == 0 && rows.data[1].cue.bytes[0x28] == 0);
    release(1002);
    // A failed first receipt must not issue a selected-enable follow-up.
    rows.data[0].cue.bytes[0x28] = 1;
    ++revision;
    before_auto = persist_calls;
    r = request(1003, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(&(r));
    force_native_failure = true;
    reply();
    force_native_failure = false;
    assert(result(1003).outcome == AZ_COMMAND_UNCERTAIN && persist_calls == before_auto + 1 && rows.data[0].cue.bytes[0x28] == 1);
    release(1003);
    // The public deadline is never renewed between its native steps.
    before_auto = persist_calls;
    r = request(1004, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    r.identity.deadline_ns = UINT64_MAX - 100;
    dispatch(&(r));
    rows.data[0].cue.bytes[0x28] = 0;
    ++revision;
    expire_after_confirm = true;
    reply();
    assert(result(1004).outcome == AZ_COMMAND_UNCERTAIN && persist_calls == before_auto + 1);
    assert(compare_timestamps(active_persist.identity.deadline_ns, r.identity.deadline_ns, 1000000000, 1000000000, 1) == AZ_TIME_EQUAL && rows.data[1].cue.bytes[0x28] == 0);
    release(1004);
    azsdk_cue_commands_test_time(0);
    // An unrelated publication before confirmation cannot qualify as our receipt.
    rows.data[0].cue.bytes[0x28] = 1;
    ++revision;
    before_auto = persist_calls;
    r = request(1005, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(&(r));
    rows.data[0].cue.bytes[0x28] = 0;
    ++revision;
    publication_matches = false;
    reply();
    publication_matches = true;
    assert(result(1005).outcome == AZ_COMMAND_UNCERTAIN && persist_calls == before_auto + 1);
    release(1005);
    // Even correlated root-only receipt proof requires full semantic equality,
    // including comment bytes beyond the public 127-byte snapshot prefix.
    char before_tail[132] = {0}, after_tail[132] = {0};
    memset(before_tail, 'x', 130);
    memset(after_tail, 'x', 130);
    before_tail[130] = 'A';
    after_tail[130] = 'B';
    put(uintptr_t, &rows.data[rows.count - 1].cue, 0x70, (uintptr_t) (before_tail));
    rows.data[0].cue.bytes[0x28] = 1;
    ++revision;
    before_auto = persist_calls;
    publication_mode = 2;
    r = request(1006, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(&(r));
    rows.data[0].cue.bytes[0x28] = 0;
    put(uintptr_t, &rows.data[rows.count - 1].cue, 0x70, (uintptr_t) (after_tail));
    ++revision;
    reply();
    assert(result(1006).outcome == AZ_COMMAND_UNCERTAIN && persist_calls == before_auto + 1);
    release(1006);
    // Rebuilt pointers and padding alone are not foreign semantic changes.
    rows.data[0].cue.bytes[0x28] = 1;
    ++revision;
    r = request(1007, AZ_CMD_SAVED_AUTO, AZ_CUE_MEMORY, 2, true);
    r.position_s = 1;
    dispatch(&(r));
    char same_old[] = "old loop";
    rows.data[0].cue.bytes[0x28] = 0;
    rows.data[0].cue.bytes[1] ^= 0xff;
    put(uintptr_t, &rows.data[0].cue, 0x70, (uintptr_t) (same_old));
    ++revision;
    reply();
    assert(azsdk_cue_commands_poll(0, &pending_result) == AZ_PENDING);
    rows.data[1].cue.bytes[0x28] = 1;
    ++revision;
    reply();
    assert(result(1007).outcome == AZ_COMMAND_APPLIED && rows.data[0].cue.bytes[0x28] == 0 && rows.data[1].cue.bytes[0x28] == 1);
    release(1007);
    publication_mode = 1;
}

bool azcs_property_valid(const azcs_property *self) {
    (void) (*self);
    return self->destroy_ != NULL;
}
size_t azcs_snapshot_count(const azcs_snapshot *self) {
    (void) (*self);
    return self->count_;
}
size_t azcs_snapshot_memory_count(const azcs_snapshot *self) {
    (void) (*self);
    return self->memory_count_;
}
uint64_t azcs_snapshot_revision(const azcs_snapshot *self) {
    (void) (*self);
    return self->revision_;
}
const unsigned char *azcs_snapshot_source_id(const azcs_snapshot *self) {
    (void) (*self);
    return self->source_id_;
}
const unsigned char *azcs_snapshot_track_id(const azcs_snapshot *self) {
    (void) (*self);
    return self->track_id_;
}
