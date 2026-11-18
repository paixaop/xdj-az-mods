/* SPDX-License-Identifier: GPL-3.0-only */
#include "../../src/common/azcuestore.h"
#include "../../src/common/azcuepersist.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "native_abi_fixture.h"


_Alignas(8) unsigned char manager[0x188], player_interface[8], service[8], player[0x200], source[0x70], pack[0xf0], publication[0x30], content[0x38];
azcs_native_cue rows[2];
uintptr_t player_table[3], service_table[7], snapshot_table[2];
azcs_native_property retained[9];
bool retained_valid = true;
_Alignas(8) unsigned char hot_publication[0x30], hot_content[0x38];
azcs_native_cue hot_rows[2];
_Alignas(8) unsigned char delta_content[0x38], delta_hot_content[0x38];
azcs_native_cue delta_rows[2], delta_hot_rows[2];
_Alignas(8) unsigned char where_from[0x20];
int where_refs, root_refs;
unsigned char identity[12] = {1, 2, 3};
int sources, contents, players, cues, properties;
bool bad_conversion;
int strings;
char last_comment[256];
uint32_t last_color;
void put(void *p, size_t off, uintptr_t value) { memcpy(((unsigned char *) (p)) + off, &value, sizeof value); }
uint64_t get(const void *p, size_t off) {
    uint64_t value;
    memcpy(&value, ((const unsigned char *) (p)) + off, 8);
    return value;
}
void capture_body(azcs_native_ref *out, void *p) {
    (void) p;
    out->value = (uintptr_t) player;
    ++players;
}
extern void capture(void);
void destroy_player(void *unused) {
    (void) unused;
    --players;
}
const void *position(const void *p) { return ((const unsigned char *) (p)) + 0xd0; }
void copy_source(uintptr_t *out, const uintptr_t *in) {
    *out = *in;
    if (*out) ++sources;
}
void copy_content(uintptr_t *out, const uintptr_t *in) {
    *out = *in;
    if (*out == ((uintptr_t) (publication)) || *out == ((uintptr_t) (hot_publication))) ++root_refs;
    else if (*out) ++contents;
}
void release(uintptr_t *p) {
    if (*p == ((uintptr_t) (where_from))) --where_refs;
    else if (*p == ((uintptr_t) (source))) --sources;
    else if (*p == ((uintptr_t) (publication)) || *p == ((uintptr_t) (hot_publication))) --root_refs;
    else {
        assert(*p == ((uintptr_t) (content)) || *p == ((uintptr_t) (hot_content)) || *p == ((uintptr_t) (delta_content)) || *p == ((uintptr_t) (delta_hot_content)));
        --contents;
    }
    *p = 0;
}
void capture_where_body(azcs_native_ref *out, const void *p) {
    assert(p == player);
    out->value = (uintptr_t) where_from;
    ++where_refs;
}
extern void capture_where(void);
void *lookup(void *self, const void *key) {
    assert(self == service && get(key, 0) == ((uintptr_t) (where_from)));
    return pack;
}
azcs_native_cue get_cue(const uintptr_t *ref, size_t ordinal) {
    assert(ordinal < 2);
    ++cues;
    if (*ref == ((uintptr_t) (hot_content))) return hot_rows[ordinal];
    assert(*ref == ((uintptr_t) (content)));
    return rows[ordinal];
}
void destroy_cue(void *unused) {
    (void) unused;
    --cues;
}
azcs_native_property convert(const uintptr_t *ref, const void *p, const void *path, const void *row) {
    assert(*ref == ((uintptr_t) (where_from)) && p == pack && path == source + 0x28);
    assert(get(((const void *) (*ref)), 8) == 0x574845524546524fULL);
    assert(((const azcs_native_cue *) (row))->bytes[0] <= 8 || ((const azcs_native_cue *) (row))->bytes[0] == 42 || ((const azcs_native_cue *) (row))->bytes[0] == 43);
    snprintf(last_comment, sizeof last_comment, "%s", get(row, 0x70) ? ((const char *) (get(row, 0x70))) : "");
    last_color = (uint32_t) (get(row, 0x78));
    azcs_native_property value = {0};
    memcpy(value.bytes + 0x30, identity, 12);
    put(&value, 0x58, ((uintptr_t) (source)));
    put(&value, 0x40, get(row, 0x10));
    put(&value, 0x60, get(row, 4) & 0xffffffffu);
    put(&value, 0x68, get(row, 8));
    if (get(row, 0x18) != UINT64_C(0x7fffffffffffffff)) memcpy(value.bytes + 0x88, identity, 12);
    put(&value, 0x98, get(row, 0x20));
    put(&value, 0xb8, get(row, 4) & 0xffffffffu);
    put(&value, 0xc0, get(row, 0x18));
    if (bad_conversion) {
        ++value.bytes[0x30];
    }
    ++properties;
    return value;
}
azcs_native_cue serialize(unsigned kind, const void *property, bool hot) {
    assert(kind <= 8 && hot == (kind != 0));
    azcs_native_cue row = {0};
    row.bytes[0] = ((unsigned char) (kind));
    const uint64_t rate = get(property, 0x60) & 0xffffffffu, in_usec = get(property, 0x68), out_usec = get(property, 0xc0);
    put(&row, 4, rate);
    put(&row, 8, in_usec);
    put(&row, 0x10, (uint64_t) ((double) (rate * in_usec) * 1e-6));
    put(&row, 0x18, out_usec);
    put(&row, 0x20, (uint64_t) ((double) (rate * out_usec) * 1e-6));
    ++cues;
    return row;
}
void destroy_property(void *unused) {
    (void) unused;
    --properties;
}
bool has_cue(const void *p, int kind) {
    assert(p == player && kind >= 0 && kind <= 8);
    return retained_valid;
}
const void *get_current_cue(const void *p, int kind) {
    assert(p == player && kind >= 0 && kind <= 8);
    return &retained[kind];
}
void copy_property(void *out, const void *in) {
    memcpy(out, in, sizeof(azcs_native_property));
    ++properties;
}
void allocate_body(azcs_native_string *out, uint64_t count) {
    out->data = malloc(count);
    if (out->data) ++strings;
}
extern void allocate(void);
void destroy_string(void *p) {
    free(((azcs_native_string *) (p))->data);
    --strings;
}
void color(void *p, unsigned r, unsigned g, unsigned b) {
    uint32_t rgb = 0xff000000u | (r << 16) | (g << 8) | b;
    memcpy(p, &rgb, 4);
}
azcs_profile profile() {
    azcs_profile p = {0};
    p.copy_source = copy_source;
    p.copy_content = copy_content;
    p.release_ref = release;
    p.position = position;
    p.get_cue = get_cue;
    p.destroy_cue = destroy_cue;
    p.convert = convert;
    p.destroy_property = destroy_property;
    p.has_cue = has_cue;
    p.get_current_cue = get_current_cue;
    p.copy_property = copy_property;
    p.serialize_property = serialize;
    p.where_from = (void *) capture_where;
    p.string_allocate = (void *) allocate;
    p.string_destroy = destroy_string;
    p.color_construct = color;
    return p;
}
void setup() {
    player_table[2] = ((uintptr_t) (capture));
    service_table[6] = ((uintptr_t) (lookup));
    snapshot_table[1] = ((uintptr_t) (destroy_player));
    put(player_interface, 0, ((uintptr_t) (player_table)));
    put(service, 0, ((uintptr_t) (service_table)));
    put(player, 0, ((uintptr_t) (snapshot_table)));
    put(player, 0x18, (uint32_t) (44100));
    put(manager, 0x158, ((uintptr_t) (player_interface)));
    put(manager, 0x160, ((uintptr_t) (service)));
    memcpy(player + 0xd8, identity, 12);
    put(player, 0x100, ((uintptr_t) (source)));
    put(source, 0x58, ((uintptr_t) (where_from)));
    put(where_from, 8, 0x574845524546524fULL);
    memcpy(pack + 0x80, player + 0xd8, 16);
    put(pack, 0xe8, ((uintptr_t) (publication)));
    put(publication, 0x28, ((uintptr_t) (content)));
    put(content, 0x28, ((uintptr_t) (rows)));
    put(content, 0x30, ((uintptr_t) (rows + 2)));
    rows[0].bytes[0] = 42;
    rows[1].bytes[0] = 43;
    {
        for (unsigned range_index_1 = 0; range_index_1 < sizeof(retained) / sizeof(retained)[0]; ++range_index_1) {
            __auto_type cue = &(retained[range_index_1]);
            memcpy(cue->bytes + 0x30, identity, 12);
        }
    }
}
void clean() {
    assert(!strings);
    assert(!sources && !where_refs && !root_refs && !contents && !players && !cues && !properties);
}
static void check_metadata(azcs_snapshot *snapshot, const azcs_profile *p, azcs_revision_state *delta_state, const struct azcuepersist_witness *before_delta, unsigned kind, unsigned entry, const char *text, unsigned rgb, bool expected) {
    assert(azcs_snapshot_open(snapshot, &((*p)), manager, 2, NULL, delta_state));
    assert(azcs_snapshot_metadata_only_changed(snapshot, &((*before_delta)), kind, entry, text, rgb, 44100) == expected);
    azcs_snapshot_reset(snapshot);
}
static void check_delta(azcs_snapshot *snapshot, const azcs_profile *p, azcs_revision_state *state, const struct azcuepersist_witness *before, bool expected) {
    assert(azcs_snapshot_open(snapshot, p, manager, 2, NULL, state));
    assert(azcs_snapshot_auto_only_changed(snapshot, before, 1, true, 44100) == expected);
    azcs_snapshot_reset(snapshot);
}
int main() {
    struct azcue_native_timestamps a = {0}, b = {0};
    a.sample_rate = 96000;
    b.sample_rate = 48000;
    a.in_us = b.in_us = 1000000;
    a.in_sample = 96001;
    b.in_sample = 48000;
    a.out_us = b.out_us = a.out_sample = b.out_sample = UINT64_C(0x7fffffffffffffff);
    assert(azcue_timestamps_equal(a, b, 48000));
    assert(azcue_timestamps_equal(b, a, 48000));
    a.out_sample = UINT64_C(0xfffffffffffffff0);
    b.out_sample = 0;
    assert(azcue_timestamps_equal(a, b, 48000));
    assert(azcue_timestamps_equal(b, a, 48000));
    b.out_us = 0;
    assert(!azcue_timestamps_equal(a, b, 48000));
    a.out_us = b.out_us = 1000000;
    a.out_sample = 96000;
    b.out_sample = 48002;
    assert(!azcue_timestamps_equal(a, b, 48000));
    a.out_us = b.out_us = a.out_sample = b.out_sample = UINT64_C(0x7fffffffffffffff);
    a.in_sample = 96002;
    assert(!azcue_timestamps_equal(a, b, 48000));
    assert(!azcue_timestamps_equal(b, a, 48000));
    a.in_sample = 96000;
    b.sample_rate = 192000;
    b.in_sample = 192001;
    assert(azcue_timestamps_equal(a, b, 48000));
    assert(azcue_timestamps_equal(b, a, 48000));
    b.in_sample = 192004;
    assert(!azcue_timestamps_equal(a, b, 48000));
    b.in_sample = 192000;
    b.in_us += 5;
    assert(azcue_timestamps_equal(a, b, 192000));
    b.in_us += 1;
    assert(!azcue_timestamps_equal(a, b, 192000));
    assert(!azcue_timestamps_equal(a, b, 0));
    b.in_us = a.in_us;
    b.sample_rate = 0;
    assert(!azcue_timestamps_equal(a, b, 48000));
    a.in_sample = b.in_sample = UINT64_C(0x7fffffffffffffff);
    assert(azcue_timestamps_equal(a, b, 48000)); // Known cached clock, absent sample index.
    setup();
    azcs_profile p = profile();
    azcs_revision_state state = {0};
    azcs_snapshot snapshot __attribute__((cleanup(azcs_snapshot_reset))) = {0};
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 1, identity, &(state)));
    assert(azcs_snapshot_count(&(snapshot)) == 2 && azcs_snapshot_revision(&(snapshot)) == 1 && azcs_snapshot_track_id(&(snapshot))[0] == 1);
    azcs_property property __attribute__((cleanup(azcs_property_reset))) = {0};
    assert(!azcs_snapshot_property(&(snapshot), 0, 1, &(property)));
    assert(!azcs_snapshot_property(&(snapshot), 1, 2, &(property)));
    assert(azcs_snapshot_property(&(snapshot), 2, 1, &(property)));
    assert(azcs_property_valid(&(property)));
    assert(get(&property.native, 0x58) == ((uintptr_t) (source)));
    azcs_property copied __attribute__((cleanup(azcs_property_reset))) = {0};
    assert(azcs_property_copy_to(&(property), &(copied)));
    azcs_property_reset(&(copied));
    put(&retained[0], 0x40, 150025);
    put(&retained[0], 0x60, 44100);
    put(&retained[0], 0x68, 3401927);
    put(&retained[0], 0x98, 450025);
    put(&retained[0], 0xb8, 44100);
    put(&retained[0], 0xc0, 10204648);
    azcs_property original __attribute__((cleanup(azcs_property_reset))) = {0};
    azcs_property canonical __attribute__((cleanup(azcs_property_reset))) = {0};
    azcs_property again __attribute__((cleanup(azcs_property_reset))) = {0};
    assert(azcs_snapshot_current_property(&(snapshot), 0, &(original)));
    assert(azcs_snapshot_canonical_property(&(snapshot), 0, &(original), &(canonical)));
    assert(get(&canonical.native, 0x58) == ((uintptr_t) (source)));
    assert(get(&original.native, 0x40) == 150025 && get(&canonical.native, 0x40) == 150024);
    assert(get(&original.native, 0x98) == 450025 && get(&canonical.native, 0x98) == 450024);
    assert(azcs_snapshot_canonical_property(&(snapshot), 8, &(canonical), &(again)));
    assert(get(&again.native, 0x40) == 150024 && get(&again.native, 0x98) == 450024);
    assert(!azcs_snapshot_canonical_property(&(snapshot), 9, &(original), &(again)));
    azcs_property_reset(&(original));
    azcs_property_reset(&(canonical));
    azcs_property_reset(&(again));
    assert(azcs_snapshot_current_property(&(snapshot), 8, &(copied)));
    assert(azcs_property_valid(&(copied)));
    azcs_property_reset(&(copied));
    assert(!azcs_snapshot_current_property(&(snapshot), 9, &(copied)));
    retained_valid = false;
    assert(!azcs_snapshot_current_property(&(snapshot), 0, &(copied)));
    retained_valid = true;
    ++retained[0].bytes[0x30];
    assert(!azcs_snapshot_current_property(&(snapshot), 0, &(copied)));
    --retained[0].bytes[0x30];
    azcs_snapshot_reset(&(snapshot));
    assert(azcs_property_valid(&(property)));
    azcs_property_reset(&(property));
    clean();
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 1, identity, &(state)) && azcs_snapshot_revision(&(snapshot)) == 1);
    azcs_snapshot_reset(&(snapshot));
    rows[1].bytes[3] = 1;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 1, identity, &(state)) && azcs_snapshot_revision(&(snapshot)) == 2);
    bad_conversion = true;
    assert(!azcs_snapshot_property(&(snapshot), 1, 2, &(property)));
    bad_conversion = false;
    pack[0x80] = 10;
    azcs_owned_cue row __attribute__((cleanup(azcs_owned_cue_reset))) = {0};
    assert(!azcs_snapshot_row(&(snapshot), 0, &(row)));
    azcs_snapshot_reset(&(snapshot));
    clean();
    pack[0x80] = 1;
    pack[0x8c] = 1;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 1, identity, &(state)));
    assert(azcs_snapshot_revision(&(snapshot)) == 2);
    azcs_snapshot_reset(&(snapshot));
    clean();
    pack[0x8c] = 0;
    player[0xe4] = 0xa5;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 1, identity, &(state)));
    assert(azcs_snapshot_revision(&(snapshot)) == 2);
    pack[0x8d] = 0x5a;
    assert(azcs_snapshot_row(&(snapshot), 0, &(row)));
    azcs_owned_cue_reset(&(row));
    assert(azcs_snapshot_current_property(&(snapshot), 0, &(copied)));
    azcs_property_reset(&(copied));
    azcs_snapshot_reset(&(snapshot));
    clean();
    player[0xe4] = 0;
    pack[0x8d] = 0;
    pack[0x88] = 1;
    assert(!azcs_snapshot_open(&(snapshot), &(p), manager, 1, identity, &(state)));
    clean();
    pack[0x88] = 0;
    unsigned char wrong[12] = {9};
    assert(!azcs_snapshot_open(&(snapshot), &(p), manager, 1, wrong, &(state)));
    clean();
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 1, NULL, &(state)));
    assert(!memcmp(azcs_snapshot_source_id(&(snapshot)), identity, 12));
    azcs_snapshot_reset(&(snapshot));
    clean();
    put(content, 0x30, ((uintptr_t) (rows)) + 1);
    assert(!azcs_snapshot_open(&(snapshot), &(p), manager, 1, identity, &(state)));
    clean();
    put(content, 0x30, ((uintptr_t) (rows + 2)));
    put(source, 0x58, 0);
    assert(!azcs_snapshot_open(&(snapshot), &(p), manager, 1, identity, &(state)));
    clean();
    put(source, 0x58, ((uintptr_t) (where_from)));
    put(where_from, 8, 0x574845524546524fULL);
    put(source, 0x58, 123);
    assert(!azcs_snapshot_open(&(snapshot), &(p), manager, 1, identity, &(state)));
    clean();
    put(source, 0x58, ((uintptr_t) (where_from)));
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, identity, &(state)) && azcs_snapshot_revision(&(snapshot)) == 3);
    azcs_snapshot_reset(&(snapshot));
    clean();
    azcs_revision_state combined = {0};
    put(pack, 0xe0, ((uintptr_t) (hot_publication)));
    put(hot_publication, 0x28, ((uintptr_t) (hot_content)));
    put(hot_content, 0x28, ((uintptr_t) (hot_rows)));
    put(hot_content, 0x30, ((uintptr_t) (hot_rows + 2)));
    hot_rows[0].bytes[0] = 43;
    hot_rows[1].bytes[0] = 42;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(combined)));
    assert(azcs_snapshot_count(&(snapshot)) == 4 && azcs_snapshot_memory_count(&(snapshot)) == 2 && azcs_snapshot_revision(&(snapshot)) == 1);
    assert(azcs_snapshot_row(&(snapshot), 2, &(row)) && row.native.bytes[0] == 43);
    azcs_owned_cue_reset(&(row));
    assert(azcs_snapshot_property(&(snapshot), 3, 1, &(property)));
    azcs_property_reset(&(property));
    azcs_snapshot_reset(&(snapshot));
    clean();
    hot_rows[1].bytes[4] = 1;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(combined)) && azcs_snapshot_revision(&(snapshot)) == 2);
    azcs_snapshot_reset(&(snapshot));
    clean();
    rows[0].bytes[4] = 1;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(combined)) && azcs_snapshot_revision(&(snapshot)) == 3);
    azcs_snapshot_reset(&(snapshot));
    clean();
    put(pack, 0xe8, 0);
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(combined)));
    assert(azcs_snapshot_count(&(snapshot)) == 2 && azcs_snapshot_memory_count(&(snapshot)) == 0);
    azcs_snapshot_reset(&(snapshot));
    clean();
    put(pack, 0xe0, 0);
    put(pack, 0xe8, ((uintptr_t) (publication)));
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(combined)));
    assert(azcs_snapshot_count(&(snapshot)) == 2 && azcs_snapshot_memory_count(&(snapshot)) == 2);
    azcs_snapshot_reset(&(snapshot));
    clean();
    put(pack, 0xe8, 0);
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(combined)));
    assert(azcs_snapshot_count(&(snapshot)) == 0);
    azcs_snapshot_reset(&(snapshot));
    clean();
    put(pack, 0xe0, ((uintptr_t) (hot_publication)));
    pack[0x8c] = 1;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(combined)));
    azcs_snapshot_reset(&(snapshot));
    clean();
    pack[0x8c] = 0;
    pack[0x88] = 1;
    assert(!azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(combined)));
    azcs_snapshot_reset(&(snapshot));
    clean();
    pack[0x88] = 0;
    put(pack, 0xe8, ((uintptr_t) (publication)));
    put(pack, 0xe0, 0);
    azcs_revision_state partial_state = {0};
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(partial_state)));
    struct azcuepersist_witness absent_root = {0};
    assert(!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &absent_root, false));
    azcs_snapshot_reset(&(snapshot));
    clean();
    put(pack, 0xe0, ((uintptr_t) (hot_publication)));
    azcs_revision_state witness_state = {0};
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(witness_state)));
    struct azcuepersist_witness witness = {0};
    assert(!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)) + 1, &witness, false));
    assert(!witness.memory_content && !witness.fingerprint);
    assert(azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &witness, false));
    assert(witness.memory_content == ((uintptr_t) (content)) && witness.hot_content == ((uintptr_t) (hot_content)) && witness.fingerprint);
    assert(witness.memory_set == ((uintptr_t) (publication)) && witness.hot_set == ((uintptr_t) (hot_publication)));
    assert(witness.source_info == ((uintptr_t) (source)) && !memcmp(witness.source_id, identity, 12));
    rows[0].bytes[7] ^= 1;
    assert(!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &witness, false));
    rows[0].bytes[7] ^= 1;
    hot_rows[0].bytes[7] ^= 1;
    assert(!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &witness, false));
    hot_rows[0].bytes[7] ^= 1;
    put(publication, 0x28, ((uintptr_t) (hot_content)));
    assert(!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &witness, false));
    put(publication, 0x28, ((uintptr_t) (content)));
    put(pack, 0xe0, 0);
    assert(!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &witness, false));
    put(pack, 0xe0, ((uintptr_t) (hot_publication)));
    assert(azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &witness, false));
    uintptr_t retained_refs[5] = {0};
    p.copy_source(&retained_refs[0], &witness.source_info);
    const uintptr_t borrowed_refs[4] = {witness.memory_set, witness.hot_set, witness.memory_content, witness.hot_content};
    for (unsigned i = 0; i < 4; ++i) p.copy_content(&retained_refs[i + 1], &borrowed_refs[i]);
    azcs_snapshot_reset(&(snapshot));
    assert(contents == 2 && root_refs == 2 && sources == 1);
    {
        for (unsigned range_index_2 = 0; range_index_2 < sizeof(retained_refs) / sizeof(retained_refs)[0]; ++range_index_2) {
            __auto_type ref = &(retained_refs[range_index_2]);
            p.release_ref(ref);
        }
    }
    clean();
    static unsigned char mpeg_before[8] = {1, 2, 3, 4, 5, 6, 7, 8}, mpeg_after[8];
    static unsigned char flac_before[24], flac_after[24];
    for (unsigned i = 0; i < 24; ++i) {
        flac_before[i] = ((unsigned char) (i + 1));
        flac_after[i] = ((unsigned char) (i + 1));
    }
    memcpy(mpeg_after, mpeg_before, 8);
    flac_after[23] = 99;
    static char long_before[201], long_after[201];
    memset(long_before, 'a', 200);
    memset(long_after, 'a', 200);
    rows[0].bytes[0] = rows[1].bytes[0] = 0;
    hot_rows[0].bytes[0] = 1;
    hot_rows[1].bytes[0] = 2;
    put(&hot_rows[0], 0x78, 0xff000000u);
    put(&hot_rows[1], 0x78, 0xff000000u);
    {
        for (unsigned range_index_3 = 0; range_index_3 < sizeof(rows) / sizeof(rows)[0]; ++range_index_3) {
            __auto_type row = &(rows[range_index_3]);
            put(row, 4, 44100);
        }
    }
    {
        for (unsigned range_index_4 = 0; range_index_4 < sizeof(hot_rows) / sizeof(hot_rows)[0]; ++range_index_4) {
            __auto_type row = &(hot_rows[range_index_4]);
            put(row, 4, 44100);
        }
    }
    rows[0].bytes[0x28] = 0;
    put(&rows[0], 0x70, ((uintptr_t) (long_before)));
    put(&rows[0], 0x30, 2);
    put(&rows[0], 0x38, ((uintptr_t) (mpeg_before)));
    put(&rows[0], 0x40, 123);
    put(&rows[0], 0x48, 1);
    put(&rows[0], 0x50, 4);
    put(&rows[0], 0x58, ((uintptr_t) (flac_before)));
    put(&rows[0], 0x60, 456);
    put(&rows[0], 0x68, 1);
    put(&hot_rows[0], 0x30, 2);
    put(&hot_rows[0], 0x38, ((uintptr_t) (mpeg_before)));
    put(&hot_rows[0], 0x40, 123);
    put(&hot_rows[0], 0x48, 1);

    put(publication, 0x28, ((uintptr_t) (content)));
    put(hot_publication, 0x28, ((uintptr_t) (hot_content)));
    azcs_revision_state delta_state = {0};
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(delta_state)));
    struct azcuepersist_witness before_delta = {0};
    assert(azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &before_delta, false));
    uintptr_t delta_refs[5] = {0};
    p.copy_source(&delta_refs[0], &before_delta.source_info);
    const uintptr_t delta_borrowed[4] = {before_delta.memory_set, before_delta.hot_set, before_delta.memory_content, before_delta.hot_content};
    for (unsigned i = 0; i < 4; ++i) { p.copy_content(&delta_refs[i + 1], &delta_borrowed[i]); }
    azcs_snapshot_reset(&(snapshot));
    memcpy(delta_rows, rows, sizeof rows);
    memcpy(delta_hot_rows, hot_rows, sizeof hot_rows);
    delta_rows[0].bytes[0x28] = 1;
    put(&delta_rows[0], 0x70, ((uintptr_t) (long_after)));
    put(&delta_rows[0], 0x38, ((uintptr_t) (mpeg_after)));
    put(&delta_rows[0], 0x40, 987);
    put(&delta_rows[0], 0x58, ((uintptr_t) (flac_after)));
    put(&delta_rows[0], 0x60, 654);
    put(&delta_hot_rows[0], 0x38, ((uintptr_t) (mpeg_after)));
    put(&delta_hot_rows[0], 0x40, 987);

    {
        for (unsigned range_index_5 = 0; range_index_5 < sizeof(delta_rows) / sizeof(delta_rows)[0]; ++range_index_5) {
            __auto_type r = &(delta_rows[range_index_5]);
            r->bytes[1] = 31;
            r->bytes[0x29] = 12;
            r->bytes[0x34] = 99;
            r->bytes[0x7f] = 17;
        }
    }
    put(delta_content, 0x28, ((uintptr_t) (delta_rows)));
    put(delta_content, 0x30, ((uintptr_t) (delta_rows + 2)));
    put(delta_hot_content, 0x28, ((uintptr_t) (delta_hot_rows)));
    put(delta_hot_content, 0x30, ((uintptr_t) (delta_hot_rows + 2)));
    put(publication, 0x28, ((uintptr_t) (delta_content)));
    put(hot_publication, 0x28, ((uintptr_t) (delta_hot_content)));

    check_delta(&snapshot, &p, &delta_state, &before_delta, true);
    long_after[180] = 'b';
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    long_after[180] = 'a';
    uint64_t cached_before = get(&delta_rows[0], 8);
    put(&delta_rows[0], 8, cached_before + 22);
    check_delta(&snapshot, &p, &delta_state, &before_delta, true);
    put(&delta_rows[0], 8, cached_before + 23);
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    put(&delta_rows[0], 8, cached_before);
    check_delta(&snapshot, &p, &delta_state, &before_delta, true);
    delta_rows[1].bytes[0x28] ^= 1;
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    delta_rows[1].bytes[0x28] ^= 1;
    delta_hot_rows[0].bytes[0x78] ^= 1;
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    delta_hot_rows[0].bytes[0x78] ^= 1;
    delta_rows[0].bytes[0x10] ^= 1;
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    delta_rows[0].bytes[0x10] ^= 1;
    delta_rows[0].bytes[0x30] = 3;
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    delta_rows[0].bytes[0x30] = 2;
    delta_rows[0].bytes[0x48] = 2;
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    delta_rows[0].bytes[0x48] = 1;
    mpeg_after[7] ^= 1;
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    mpeg_after[7] ^= 1;
    flac_after[19] ^= 1;
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    flac_after[19] ^= 1;
    flac_after[23] ^= 1;
    check_delta(&snapshot, &p, &delta_state, &before_delta, true);
    flac_after[23] ^= 1;
    delta_rows[0].bytes[0x7c] ^= 1;
    check_delta(&snapshot, &p, &delta_state, &before_delta, false);
    delta_rows[0].bytes[0x7c] ^= 1;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 2, NULL, &(delta_state)));
    assert(!azcs_snapshot_auto_only_changed(&(snapshot), &(before_delta), 1, false, 44100));
    assert(!azcs_snapshot_auto_only_changed(&(snapshot), &(before_delta), 3, true, 44100));
    assert(!azcs_snapshot_auto_only_changed(&(snapshot), &(before_delta), 1, true, 0));
    azcs_snapshot_reset(&(snapshot));
    // Metadata confirmation checks every typed stored value, including full
    // comments and seek variants, without converting unrelated properties.
    delta_rows[0].bytes[0x28] = 0;
    const char *edited = "metadata caf\xc3\xa9";

    put(&delta_rows[1], 0x70, ((uintptr_t) (edited)));
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, edited, 0, true);
    long_after[180] = 'b';
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, edited, 0, false);
    long_after[180] = 'a';
    delta_rows[0].bytes[0x28] ^= 1;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, edited, 0, false);
    delta_rows[0].bytes[0x28] ^= 1;
    delta_hot_rows[0].bytes[0x78] ^= 1;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, edited, 0, false);
    delta_hot_rows[0].bytes[0x78] ^= 1;
    delta_rows[1].bytes[0x2a] ^= 1;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, edited, 0, false);
    delta_rows[1].bytes[0x2a] ^= 1;
    put(&delta_rows[1], 8, 22);
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, edited, 0, true);
    put(&delta_rows[1], 8, 23);
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, edited, 0, false);
    put(&delta_rows[1], 8, 0);
    mpeg_after[7] ^= 1;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, edited, 0, false);
    mpeg_after[7] ^= 1;
    delta_rows[0].bytes[0x30] = 3;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, edited, 0, false);
    delta_rows[0].bytes[0x30] = 2;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 3, edited, 0, false);
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 8, 8, edited, 0, false);
    put(&delta_rows[1], 0x70, 0);
    put(&delta_hot_rows[0], 0x70, ((uintptr_t) (edited)));
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 1, 1, edited, 0, true);
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 1, 2, edited, 0, false);
    put(&delta_hot_rows[0], 0x70, 0);
    put(&delta_hot_rows[0], 0x78, 0xff123456u);
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 1, 1, NULL, 0x123456, true);
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 1, 1, NULL, 0x123457, false);
    delta_hot_rows[0].bytes[0x7c] ^= 1;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 1, 1, NULL, 0x123456, false);
    delta_hot_rows[0].bytes[0x7c] ^= 1;
    put(&delta_hot_rows[0], 0x78, 0xff000000u);
    // Actual Memory readback publishes the requested opaque RGB and its native
    // palette index together. Uncolored black can use the zero color word.
    put(&delta_rows[1], 0x78, 0xff00c3ffu);
    delta_rows[1].bytes[0x7c] = 6;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, NULL, 0x00c3ff, true);
    delta_rows[0].bytes[0x7c] ^= 1;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, NULL, 0x00c3ff, false);
    delta_rows[0].bytes[0x7c] ^= 1;
    delta_rows[1].bytes[0x7c] = 5;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, NULL, 0x00c3ff, false);
    delta_rows[1].bytes[0x7c] = 6;
    put(&delta_rows[1], 0x78, 0x00c3ffu);
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, NULL, 0x00c3ff, false);
    put(&delta_rows[1], 0x78, 0x7f00c3ffu);
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, NULL, 0x00c3ff, false);
    put(&delta_rows[1], 0x78, 0u);
    delta_rows[1].bytes[0x7c] = 0;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, NULL, 0, true);
    put(&delta_rows[1], 0x78, 0xff000000u);
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, NULL, 0, true);
    delta_rows[1].bytes[0x7c] = 6;
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, NULL, 0, false);
    put(&delta_rows[1], 0x78, 0xff00c3ffu);
    long_after[180] = 'b';
    check_metadata(&(snapshot), &(p), &(delta_state), &(before_delta), 0, 2, NULL, 0x00c3ff, false);
    long_after[180] = 'a';
    {
        for (unsigned range_index_6 = 0; range_index_6 < sizeof(delta_refs) / sizeof(delta_refs)[0]; ++range_index_6) {
            __auto_type ref = &(delta_refs[range_index_6]);
            p.release_ref(ref);
        }
    }
    clean();
    // Production construction: zero is present, source rate comes from the
    // owned PlayerState, seek variants start empty, and native conversion owns refs.
    put(pack, 0xe8, ((uintptr_t) (publication)));
    put(pack, 0xe0, ((uintptr_t) (hot_publication)));
    put(publication, 0x28, ((uintptr_t) (content)));
    put(hot_publication, 0x28, ((uintptr_t) (hot_content)));
    put(content, 0x28, ((uintptr_t) (rows)));
    put(content, 0x30, ((uintptr_t) (rows)));
    put(hot_content, 0x28, ((uintptr_t) (hot_rows)));
    put(hot_content, 0x30, ((uintptr_t) (hot_rows)));
    azcs_revision_state timed_state = {0};
    const uint32_t rates[] = {44100, 48000, 96000, 192000};
    put(pack, 0xbe, (uint32_t) (16777216)); // Native musical key data must never supply Hertz.
    {
        for (unsigned range_index_7 = 0; range_index_7 < sizeof(rates) / sizeof(rates)[0]; ++range_index_7) {
            uint32_t rate = rates[range_index_7];

            memcpy(player + 0x18, &rate, 4);
            assert(azcs_snapshot_open(&(snapshot), &(p), manager, 4, NULL, &(timed_state)));
            assert(azcs_snapshot_source_rate(&(snapshot)) == rate);
            azcs_property timed __attribute__((cleanup(azcs_property_reset))) = {0};
            assert(azcs_snapshot_timed_property(&(snapshot), 0, 0., 0., false, 0xff0000, "Zero caf\xc3\xa9", &(timed)));
            assert(compare_timestamps(get(&timed.native, 0x40), 0, rate, rate, 1) == AZ_TIME_EQUAL);
            assert(!strcmp(last_comment, "Zero caf\xc3\xa9") && last_color == 0xffff0000 && !strings);
            assert(azcs_snapshot_timed_property(&(snapshot), 8, 1.234567, 9.876543, true, 0x123456, "Loop", &(timed)));
            assert(compare_timestamps((double) (get(&timed.native, 0x40)) / rate, 1.234567, 1, rate, 1) == AZ_TIME_EQUAL);
            assert(compare_timestamps((double) (get(&timed.native, 0x98)) / rate, 9.876543, 1, rate, 1) == AZ_TIME_EQUAL);
            assert(last_color == 0xff123456 && !strcmp(last_comment, "Loop"));
            const double frames[] = {0.005, 0.49, 0.5, 0.995, 100.005, 100.49, 100.5, 100.995, 12345678.005, 12345678.49, 12345678.51, 12345678.995};
            const uint64_t nearest[] = {0, 0, 1, 1, 100, 100, 101, 101, 12345678, 12345678, 12345679, 12345679};
            for (unsigned i = 0; i < sizeof frames / sizeof *frames; ++i) {
                const double start_s = frames[i] / rate, end_s = (frames[i] + 300) / rate;
                {
                    bool range_values_8[] = {false, true};
                    for (unsigned range_index_8 = 0; range_index_8 < sizeof(range_values_8) / sizeof(range_values_8)[0]; ++range_index_8) {
                        bool loop = range_values_8[range_index_8];

                        assert(azcs_snapshot_timed_property(&(snapshot), 8, start_s, end_s, loop, 0x123456, "Fractional frame", &(timed)));
                        const int selected = compare_timestamps(get(&timed.native, 0x40), nearest[i], rate, rate, 1);
                        assert(selected != AZ_TIME_INVALID && selected == AZ_TIME_EQUAL);
                        azcs_property canonical __attribute__((cleanup(azcs_property_reset))) = {0};
                        assert(azcs_snapshot_canonical_property(&(snapshot), 8, &(timed), &(canonical)));
                        const int roundtrip = compare_timestamps(get(&canonical.native, 0x40), nearest[i], rate, rate, 1);
                        assert(roundtrip != AZ_TIME_INVALID && roundtrip == AZ_TIME_EQUAL);
                        if (loop) {
                            const int selected_end = compare_timestamps(get(&timed.native, 0x98), nearest[i] + 300, rate, rate, 1);
                            const int roundtrip_end = compare_timestamps(get(&canonical.native, 0x98), nearest[i] + 300, rate, rate, 1);
                            assert(selected_end != AZ_TIME_INVALID && selected_end == AZ_TIME_EQUAL);
                            assert(roundtrip_end != AZ_TIME_INVALID && roundtrip_end == AZ_TIME_EQUAL);
                        }
                    }
                }
            }
            assert(!azcs_snapshot_timed_property(&(snapshot), 8, 9007199254740991. / rate, 0., false, 0, "", &(timed)));
            assert(!azcs_snapshot_timed_property(&(snapshot), 8, 9007199254740991. / 1000000, 0., false, 0, "", &(timed)));
            assert(!azcs_snapshot_timed_property(&(snapshot), 0, 1., 2., true, 0x123456, "Invalid memory RGB", &(timed)));
            assert(!azcs_snapshot_timed_property(&(snapshot), 0, -1., 0., false, 0, "", &(timed)));
            assert(!azcs_snapshot_timed_property(&(snapshot), 0, 1., 1., true, 0, "", &(timed)));
            bad_conversion = true;
            assert(!azcs_snapshot_timed_property(&(snapshot), 0, 1., 0., false, 0, "", &(timed)));
            bad_conversion = false;
            azcs_snapshot_reset(&(snapshot));
            azcs_property_reset(&(timed));
            clean();
        }
    }
    uint32_t unknown = 0;
    memcpy(player + 0x18, &unknown, 4);
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 4, NULL, &(timed_state)));
    azcs_property missing __attribute__((cleanup(azcs_property_reset))) = {0};
    assert(!azcs_snapshot_timed_property(&(snapshot), 1, 0., 0., false, 0, "", &(missing)));
    azcs_snapshot_reset(&(snapshot));
    clean();
    // Production baseline comparison admits exactly one addition and no
    // changes to the other cue collection, independent of row ordering.
    uint32_t rate = 48000;
    memcpy(player + 0x18, &rate, 4);
    put(pack, 0xe8, ((uintptr_t) (publication)));
    put(pack, 0xe0, ((uintptr_t) (hot_publication)));
    put(content, 0x28, ((uintptr_t) (rows)));
    put(content, 0x30, ((uintptr_t) (rows + 1)));
    put(hot_content, 0x28, ((uintptr_t) (hot_rows)));
    put(hot_content, 0x30, ((uintptr_t) (hot_rows)));
    rows[0] = (azcs_native_cue) {0};
    rows[0].bytes[0] = 0;
    put(&rows[0], 4, rate);
    put(&rows[0], 8, 1000000);
    put(&rows[0], 16, 48000);
    put(&rows[0], 24, UINT64_C(0x7fffffffffffffff));
    put(&rows[0], 32, UINT64_C(0x7fffffffffffffff));
    azcs_revision_state creation_state = {0};
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 6, NULL, &(creation_state)));
    struct azcuepersist_witness before_create = {0};
    assert(azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &before_create, false));
    azcs_property wanted __attribute__((cleanup(azcs_property_reset))) = {0};
    assert(azcs_snapshot_timed_property(&(snapshot), 0, 2., 3., true, 0, "", &(wanted)));
    delta_rows[0] = rows[0];
    delta_rows[1] = serialize(0, &wanted.native, false);
    destroy_cue(NULL);
    azcs_snapshot_reset(&(snapshot));
    put(delta_content, 0x28, ((uintptr_t) (delta_rows)));
    put(delta_content, 0x30, ((uintptr_t) (delta_rows + 2)));
    put(publication, 0x28, ((uintptr_t) (delta_content)));
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 6, NULL, &(creation_state)));
    assert(azcs_snapshot_creation_only_changed(&(snapshot), &(before_create), 0, &(wanted), rate, "", 0));
    azcs_snapshot_reset(&(snapshot));
    delta_rows[0].bytes[0x78] ^= 1;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 6, NULL, &(creation_state)));
    assert(!azcs_snapshot_creation_only_changed(&(snapshot), &(before_create), 0, &(wanted), rate, "", 0));
    azcs_snapshot_reset(&(snapshot));
    delta_rows[0].bytes[0x78] ^= 1;
    azcs_native_cue swap = delta_rows[0];
    delta_rows[0] = delta_rows[1];
    delta_rows[1] = swap;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 6, NULL, &(creation_state)));
    assert(azcs_snapshot_creation_only_changed(&(snapshot), &(before_create), 0, &(wanted), rate, "", 0));
    azcs_snapshot_reset(&(snapshot));
    azcs_property_reset(&(wanted));
    clean();
    // A track can have no publication roots yet. Creation records explicit
    // absence and later proves the first native root contains only its cue.
    put(pack, 0xe8, 0);
    put(pack, 0xe0, 0);
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 7, NULL, &(creation_state)));
    assert(!azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &before_create, false));
    assert(azcs_snapshot_content_witness(&(snapshot), azcs_snapshot_revision(&(snapshot)), &before_create, true));
    assert(!before_create.memory_set && !before_create.hot_set);
    const char label[] = "Native label \342\234\223";
    const uint32_t rgb = 0x00c3ff;
    assert(azcs_snapshot_timed_property(&(snapshot), 0, 0., 0., false, rgb, label, &(wanted)));
    rows[0] = serialize(0, &wanted.native, false);
    destroy_cue(NULL);
    azcs_snapshot_reset(&(snapshot));
    put(&rows[0], 32, UINT64_C(0xfffffffffffffff0)); // Native point's unused OUT sample cache.
    put(publication, 0x28, ((uintptr_t) (content)));
    put(content, 0x30, ((uintptr_t) (rows + 1)));
    put(pack, 0xe8, ((uintptr_t) (publication)));
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 7, NULL, &(creation_state)));
    assert(!azcs_snapshot_creation_only_changed(&(snapshot), &(before_create), 0, &(wanted), rate, label, rgb));
    azcs_snapshot_reset(&(snapshot)); // Serializer's empty label is insufficient.
    put(&rows[0], 0x70, ((uintptr_t) (label)));
    put(&rows[0], 0x78, (uint32_t) (0xff000000u | rgb));
    rows[0].bytes[0x7c] = 6;
    put(&rows[0], 0x48, (uint32_t) (0x301)); // Native tag0 union copy leaves its inactive length undefined.
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 7, NULL, &(creation_state)));
    assert(azcs_snapshot_creation_only_changed(&(snapshot), &(before_create), 0, &(wanted), rate, label, rgb));
    azcs_snapshot_reset(&(snapshot));
    rows[0].bytes[0x7c] = 0;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 7, NULL, &(creation_state)));
    assert(!azcs_snapshot_creation_only_changed(&(snapshot), &(before_create), 0, &(wanted), rate, label, rgb));
    azcs_snapshot_reset(&(snapshot));
    rows[0].bytes[0x7c] = 6;
    rows[0].bytes[0x30] = 1;
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 7, NULL, &(creation_state)));
    assert(!azcs_snapshot_creation_only_changed(&(snapshot), &(before_create), 0, &(wanted), rate, label, rgb));
    azcs_snapshot_reset(&(snapshot));
    rows[0].bytes[0x30] = 0;
    put(&rows[0], 24, 1000000);
    assert(azcs_snapshot_open(&(snapshot), &(p), manager, 7, NULL, &(creation_state)));
    assert(!azcs_snapshot_creation_only_changed(&(snapshot), &(before_create), 0, &(wanted), rate, label, rgb));
    azcs_snapshot_reset(&(snapshot));
    azcs_property_reset(&(wanted));
    clean();
    puts("SDK_CUE_STORE_OK");
}
