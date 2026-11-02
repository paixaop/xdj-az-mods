/* SPDX-License-Identifier: GPL-3.0-only */
/* Optional local-file validation. Supply your own DAT, EXT and optional 2EX files.
 * No track-derived bytes are embedded or written. Real fixtures stay outside the source tree.
 * The independent reader checks every grid/phrase/cue field and the overview payload, then
 * removes PCO2 sections in memory to exercise split legacy hot-cue lists. */
#include "sdk_analysis.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct input {
    unsigned char *bytes;
    size_t size;
};
struct raw_cue {
    unsigned classification, slot, type, start, end;
};
static unsigned u16(const unsigned char *p) { return (unsigned) p[0] * 256u + p[1]; }
static unsigned u32(const unsigned char *p) { return (unsigned) p[0] * 16777216u + (unsigned) p[1] * 65536u + (unsigned) p[2] * 256u + p[3]; }
static void write32(unsigned char *p, unsigned v) {
    for (int i = 3; i >= 0; i--) {
        p[i] = (unsigned char) v;
        v >>= 8;
    }
}
static struct input read_input(const char *path) {
    struct input in = {0};
    FILE *f = fopen(path, "rb");
    if (!f) return in;
    if (fseek(f, 0, SEEK_END)) {
        fclose(f);
        return in;
    }
    long n = ftell(f);
    if (n < 12 || (unsigned long) n > ST_FILE_MAX || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return in;
    }
    in.size = (size_t) n;
    in.bytes = malloc(in.size);
    if (!in.bytes || fread(in.bytes, 1, in.size, f) != in.size) {
        free(in.bytes);
        in.bytes = NULL;
        in.size = 0;
    }
    fclose(f);
    return in;
}
/* Validate the framing independently of the production parser. */
static int framed(struct input in) {
    if (!in.bytes || in.size < 12 || memcmp(in.bytes, "PMAI", 4) || u32(in.bytes + 8) != in.size) return 0;
    size_t at = u32(in.bytes + 4);
    if (at < 12 || at > in.size) return 0;
    while (at < in.size) {
        if (in.size - at < 12) return 0;
        size_t h = u32(in.bytes + at + 4), n = u32(in.bytes + at + 8);
        if (h < 12 || h > n || n > in.size - at) return 0;
        at += n;
    }
    return 1;
}
static const unsigned char *tag(struct input in, const char *name, int kind) {
    for (size_t at = u32(in.bytes + 4); at < in.size; at += u32(in.bytes + at + 8))
        if (!memcmp(in.bytes + at, name, 4) && (kind < 0 || (u32(in.bytes + at + 4) >= 16 && u32(in.bytes + at + 12) == (unsigned) kind)))
            return in.bytes + at;
    return NULL;
}
static unsigned cue_count(const unsigned char *t, int co2) { return t ? u16(t + (co2 ? 16 : 18)) : 0; }
/* Decode the production UTF-8 output in the opposite direction to the source UTF-16BE. */
static int same_comment(const unsigned char *source, size_t bytes, const char *utf8) {
    const unsigned char *text = (const unsigned char *) utf8;
    for (size_t off = 0; off + 1 < bytes; off += 2) {
        unsigned want = u16(source + off);
        if (!want) return !*text;
        if (want >= 0xd800 && want <= 0xdbff && off + 3 < bytes) {
            unsigned low = u16(source + off + 2);
            if (low < 0xdc00 || low > 0xdfff) return 0;
            want = 65536u + (want - 0xd800) * 1024u + low - 0xdc00;
            off += 2;
        }
        unsigned got = *text++;
        if (got >= 0xc0) {
            unsigned count = got < 0xe0 ? 1 : got < 0xf0 ? 2
                                                         : 3;
            got &= count == 1 ? 31u : count == 2 ? 15u
                                                 : 7u;
            for (unsigned j = 0; j < count; j++) {
                if ((*text & 0xc0) != 0x80) return 0;
                got = got * 64u + (*text++ & 63u);
            }
        }
        if (got != want) return 0;
    }
    return !*text;
}
static unsigned raw_cues(const unsigned char *t, int co2, struct raw_cue *out, unsigned cap) {
    if (!t) return 0;
    size_t at = u32(t + 4);
    unsigned count = cue_count(t, co2), kind = u32(t + 12);
    assert(count <= cap);
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = t + at;
        size_t len = u32(e + 8);
        assert(len >= 40u);
        out[i] = (struct raw_cue) {kind ? AZ_CUE_HOT : AZ_CUE_MEMORY, u32(e + 12), e[co2 ? 16 : 28], u32(e + (co2 ? 20 : 32)), u32(e + (co2 ? 24 : 36))};
        at += len;
    }
    return count;
}
static void compare_cues(struct input pair[2], const struct st_data *d) {
    unsigned expected = 0;
    for (int kind = 1; kind >= 0; kind--) {
        const unsigned char *t = tag(pair[1], "PCO2", kind);
        if (!t) t = tag(pair[0], "PCO2", kind);
        if (!t) continue; /* The supplied sample's full path uses extended lists. */
        size_t at = u32(t + 4);
        unsigned count = cue_count(t, 1);
        expected += count;
        for (unsigned i = 0; i < count; i++) {
            const unsigned char *e = t + at;
            unsigned slot = u32(e + 12), start = u32(e + 20), end = u32(e + 24);
            const az_cue_entry *found = NULL;
            for (unsigned j = 0; j < d->view.cue_count; j++) {
                const az_cue_entry *c = &d->cues[j];
                if (c->classification == (unsigned) (kind ? AZ_CUE_HOT : AZ_CUE_MEMORY) && c->slot == slot && c->start_s == start / 1000.0) {
                    found = c;
                    break;
                }
            }
            assert(found && found->type == e[16] && (found->valid & AZ_CUE_START));
            if (found->type == AZ_CUE_LOOP) assert((found->valid & AZ_CUE_END) && found->end_s == end / 1000.0);
            size_t len = u32(e + 8);
            assert(len >= 40);
            size_t nc = len >= 44 ? u32(e + 40) : 0;
            assert(nc <= (len >= 44 ? len - 44 : 0));
            assert(same_comment(e + (len >= 44 ? 44 : 40), nc, found->comment));
            if (!kind) {
                assert((found->valid & AZ_CUE_COLOR_ID) && found->color_id == e[28]);
                assert(!(found->valid & AZ_CUE_COLOR));
            } else {
                assert(!(found->valid & AZ_CUE_COLOR_ID));
                if (len >= 44 && len - 44 - nc >= 4) {
                    const unsigned char *rgb = e + 44 + nc;
                    assert((found->valid & AZ_CUE_COLOR) && found->rgb == ((unsigned) rgb[1] * 65536u + (unsigned) rgb[2] * 256u + rgb[3]));
                }
            }
            at += u32(e + 8);
        }
    }
    assert(expected == d->view.cue_count);
}
static void compare_grid(struct input dat, const struct st_data *d) {
    const unsigned char *t = tag(dat, "PQTZ", -1);
    assert(t);
    unsigned count = u32(t + 20);
    assert(count == d->view.beat_count);
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = t + u32(t + 4) + i * 8;
        assert(d->beats[i].time_s == u32(e + 4) / 1000.0 && d->beats[i].bpm == u16(e + 2) / 100.0 && d->beats[i].beat_in_bar == u16(e));
    }
}
static void compare_phrases(struct input ext, const struct st_data *d) {
    const unsigned char *t = tag(ext, "PSSI", -1);
    assert(t);
    size_t n = u32(t + 8), header = u32(t + 4);
    unsigned count = u16(t + 16), stride = u32(t + 12);
    unsigned char *clear = malloc(n);
    assert(clear);
    memcpy(clear, t, n);
    if (u16(clear + 18) > 20) {
        static const unsigned char key[] = {0xcb, 0xe1, 0xee, 0xfa, 0xe5, 0xee, 0xad, 0xee, 0xe9, 0xd2, 0xe9, 0xeb, 0xe1, 0xe9, 0xf3, 0xe8, 0xe9, 0xf4, 0xe1};
        for (size_t i = 18; i < n; i++) clear[i] ^= (unsigned char) (key[(i - 18) % sizeof key] + count);
    }
    assert(d->view.phrase_count == count);
    for (unsigned i = 0; i < count; i++) {
        const unsigned char *e = clear + header + i * stride;
        unsigned end = i + 1 < count ? u16(e + stride + 2) : u16(clear + 26);
        assert(d->phrases[i].start_beat + 1 == u16(e + 2) && d->phrases[i].end_beat + 1 == end);
        assert(d->phrases[i].raw_kind == u16(e + 4) && d->phrases[i].raw_mood == u16(clear + 18));
    }
    free(clear);
}
static struct input without_extended_cues(struct input in) {
    struct input out = {malloc(in.size), 0};
    assert(out.bytes);
    size_t header = u32(in.bytes + 4);
    memcpy(out.bytes, in.bytes, header);
    out.size = header;
    for (size_t at = header; at < in.size; at += u32(in.bytes + at + 8)) {
        size_t n = u32(in.bytes + at + 8);
        if (memcmp(in.bytes + at, "PCO2", 4)) {
            memcpy(out.bytes + out.size, in.bytes + at, n);
            out.size += n;
        }
    }
    write32(out.bytes + 8, (unsigned) out.size);
    return out;
}
static void compare_legacy(struct input pair[2]) {
    struct input stripped[2] = {without_extended_cues(pair[0]), without_extended_cues(pair[1])};
    struct raw_cue expected[ST_CUES], selected_hot[8];
    unsigned n = 0, hot_mask = 0;
    for (int f = 0; f < 2; f++) {
        const unsigned char *t = tag(stripped[f], "PCOB", 1);
        struct raw_cue hot[ST_CUES];
        unsigned count = raw_cues(t, 0, hot, ST_CUES);
        for (unsigned i = 0; i < count; i++) {
            assert(hot[i].slot >= 1 && hot[i].slot <= 8);
            selected_hot[hot[i].slot - 1] = hot[i]; /* EXT overrides a duplicate hot slot. */
            hot_mask |= 1u << (hot[i].slot - 1);
        }
    }
    const unsigned char *memory = tag(stripped[0], "PCOB", 0);
    if (!memory) memory = tag(stripped[1], "PCOB", 0);
    n = raw_cues(memory, 0, expected, ST_CUES);
    for (unsigned i = 0; i < 8; i++)
        if (hot_mask & (1u << i)) {
            assert(n < ST_CUES);
            expected[n++] = selected_hot[i];
        }
    struct st_data *d = malloc(sizeof *d);
    assert(d);
    st_data_init(d);
    const uint8_t *buffers[] = {stripped[0].bytes, stripped[1].bytes};
    size_t sizes[] = {stripped[0].size, stripped[1].size};
    int rc = st_parse_pair(d, buffers, sizes, AZ_DATA_CUES);
    printf("legacy fallback: result %d, expected %u cues, actual %u cues\n", rc, n, d->view.cue_count);
    assert(rc == AZ_OK && d->view.cue_count == n);
    unsigned actual_mask = 0;
    for (unsigned i = 0; i < d->view.cue_count; i++)
        if (d->cues[i].classification == AZ_CUE_HOT) actual_mask |= 1u << (d->cues[i].slot - 1);
    assert(actual_mask == hot_mask);
    unsigned char used[ST_CUES] = {0};
    for (unsigned i = 0; i < n; i++) {
        const struct raw_cue *want = &expected[i];
        unsigned j;
        for (j = 0; j < d->view.cue_count; j++) {
            const az_cue_entry *got = &d->cues[j];
            if (!used[j] && got->classification == want->classification && got->slot == want->slot &&
                got->type == want->type && got->start_s == want->start / 1000.0) {
                assert(got->valid & AZ_CUE_START);
                if (want->type == AZ_CUE_LOOP)
                    assert((got->valid & AZ_CUE_END) && got->end_s == want->end / 1000.0);
                used[j] = 1;
                break;
            }
        }
        assert(j < d->view.cue_count);
    }
    free(d);
    free(stripped[0].bytes);
    free(stripped[1].bytes);
}
int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: %s DAT EXT [2EX]\n", argv[0]);
        return 2;
    }
    struct input pair[] = {read_input(argv[1]), read_input(argv[2])};
    assert(framed(pair[0]) && framed(pair[1]));
    struct st_data *d = malloc(sizeof *d);
    assert(d);
    st_data_init(d);
    const uint8_t *buffers[] = {pair[0].bytes, pair[1].bytes};
    size_t sizes[] = {pair[0].size, pair[1].size};
    assert(st_parse_pair(d, buffers, sizes, 31) == AZ_OK);
    compare_grid(pair[0], d);
    compare_cues(pair, d);
    compare_phrases(pair[1], d);
    const unsigned char *wave = tag(pair[0], "PWAV", -1);
    assert(wave);
    assert(d->view.waveform.sample_count == u32(wave + 12) && !memcmp(d->wave, wave + u32(wave + 4), u32(wave + 12)));
    printf("full pair: %u beats, %u cues, %u phrases, %u overview bytes; every selected raw field matched\n", d->view.beat_count, d->view.cue_count, d->view.phrase_count, d->view.waveform.sample_count);
    unsigned loops = 0;
    for (unsigned i = 0; i < d->view.cue_count; i++) loops += d->cues[i].type == AZ_CUE_LOOP;
    printf("saved cue types: %u points, %u loops\n", d->view.cue_count - loops, loops);
    compare_legacy(pair);
    if (argc == 4) {
        struct input extra = read_input(argv[3]);
        assert(framed(extra));
        st_data_init(d);
        const uint8_t *eb[] = {extra.bytes, NULL};
        size_t en[] = {extra.size, 0};
        assert(st_parse_pair(d, eb, en, 31) == AZ_OK);
        assert(!d->view.beat_count && !d->view.cue_count && !d->view.phrase_count && !d->view.waveform.sample_count);
        puts("2EX: framing valid; legacy overview parser excludes it (separate range provider supports PWV7)");
        free(extra.bytes);
    }
    free(d);
    free(pair[0].bytes);
    free(pair[1].bytes);
    return 0;
}
