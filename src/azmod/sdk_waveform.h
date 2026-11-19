/* SPDX-License-Identifier: GPL-3.0-only */
/* Pure stored-waveform helpers. No allocation, I/O, native pointers or public ABI.
 * Streaming callers validate every container envelope before publishing an index,
 * and check file freshness around reading the planned byte span. */
#ifndef AZMOD_SDK_WAVEFORM_H
#define AZMOD_SDK_WAVEFORM_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define SDKW_FILE_MAX (16u * 1024u * 1024u)
#define SDKW_TAGS_MAX 4096u
#define SDKW_WINDOW_MAX 4096u
#define SDKW_PWAV 0x50574156u
#define SDKW_PWV2 0x50575632u
#define SDKW_PWV3 0x50575633u
#define SDKW_PWV4 0x50575634u
#define SDKW_PWV5 0x50575635u
#define SDKW_PWV6 0x50575636u
#define SDKW_PWV7 0x50575637u
#define SDKW_PMAI 0x504d4149u
#define SDKW_PWV3_WORD 0x00960000u
#define SDKW_PWV5_WORD 0x00960305u
#define SDKW_PWV7_WORD 0x00960000u

enum {
    SDKW_OK = 0,
    SDKW_EMPTY,
    SDKW_NOT_FOUND,
    SDKW_UNSUPPORTED,
    SDKW_MALFORMED,
    SDKW_OUT_OF_RANGE,
    SDKW_LIMIT,
    SDKW_INVALID,
    SDKW_AMBIGUOUS
};
enum {
    SDKW_ENCODING_PWV3 = 1,
    SDKW_ENCODING_PWV5 = 2,
    SDKW_ENCODING_PWV7 = 3,
    SDKW_ENCODING_PWV4 = 4,
    SDKW_ENCODING_PWV6 = 5,
    SDKW_ENCODING_PWV2 = 6,
    SDKW_ENCODING_PWAV = 7
};

struct sww_file {
    uint32_t size, header_size;
};
struct sww_tag {
    struct sww_file file;
    uint32_t fourcc, offset, header_size, size;
};
struct sww_index {
    struct sww_tag tag;
    uint32_t encoding, entry_size, sample_count;
};
struct sww_window {
    uint32_t encoding, total_samples, first_sample, sample_count;
    uint64_t byte_offset;
    size_t byte_count;
    double start_s, seconds_per_sample;
};
struct sww_sample {
    uint8_t raw, height, intensity, reserved;
};

/* Numeric raw preserves all 16 bits; RGB are unscaled 3-bit components.
 * low_bits has no inferred fine-height meaning. */
struct sww_rgb_sample {
    uint16_t raw;
    uint8_t red, green, blue, height, low_bits, reserved;
};
/* Original unsigned band bytes, without inferred gains or pixel calibration. */
struct sww_band_sample {
    uint8_t low, mid, high, reserved;
};
struct sww_format {
    uint32_t fourcc, entry_size, header_word;
};
static inline int sww_format_for(uint32_t encoding, struct sww_format *out) {
    if (!out) return SDKW_INVALID;
    struct sww_format f;
    switch (encoding) {
    case SDKW_ENCODING_PWV3: f = (struct sww_format) {SDKW_PWV3, 1, SDKW_PWV3_WORD}; break;
    case SDKW_ENCODING_PWV5: f = (struct sww_format) {SDKW_PWV5, 2, SDKW_PWV5_WORD}; break;
    case SDKW_ENCODING_PWV7: f = (struct sww_format) {SDKW_PWV7, 3, SDKW_PWV7_WORD}; break;
    case SDKW_ENCODING_PWV4: f = (struct sww_format) {SDKW_PWV4, 6, 0}; break;
    case SDKW_ENCODING_PWV6: f = (struct sww_format) {SDKW_PWV6, 3, 0}; break;
    case SDKW_ENCODING_PWV2: f = (struct sww_format) {SDKW_PWV2, 1, 0x00010000u}; break;
    case SDKW_ENCODING_PWAV: f = (struct sww_format) {SDKW_PWAV, 1, 0x00010000u}; break;
    default: return SDKW_UNSUPPORTED;
    }
    *out = f;
    return SDKW_OK;
}
static inline int sww_is_preview(uint32_t encoding) {
    return encoding == SDKW_ENCODING_PWV4 || encoding == SDKW_ENCODING_PWV6 ||
           encoding == SDKW_ENCODING_PWV2 || encoding == SDKW_ENCODING_PWAV;
}
static inline int sww_is_mono_preview(uint32_t encoding) {
    return encoding == SDKW_ENCODING_PWV2 || encoding == SDKW_ENCODING_PWAV;
}
static inline uint32_t sww_header_bytes(uint32_t encoding) {
    return encoding == SDKW_ENCODING_PWV6 || sww_is_mono_preview(encoding) ? 20u : 24u;
}

static inline uint32_t sww_be32(const uint8_t *p) {
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}
static inline int sww_file_valid(const struct sww_file *f) {
    return f && f->size >= 12 && f->size <= SDKW_FILE_MAX &&
           f->header_size >= 12 && f->header_size <= f->size;
}
/* Only the first 12 bytes are needed. file_size is the actual bounded file extent,
 * not merely the length declared by untrusted input. Outputs stay unchanged on error. */
static inline int sww_file_header(const uint8_t *p, size_t available, size_t file_size,
                                  struct sww_file *out) {
    if (!p || !out) return SDKW_INVALID;
    if (file_size > SDKW_FILE_MAX) return SDKW_LIMIT;
    if (file_size < 12 || available < 12 || sww_be32(p) != SDKW_PMAI) return SDKW_MALFORMED;
    struct sww_file f = {(uint32_t) file_size, sww_be32(p + 4)};
    if (!sww_file_valid(&f) || sww_be32(p + 8) != file_size) return SDKW_MALFORMED;
    *out = f;
    return SDKW_OK;
}
static inline int sww_tag_valid(const struct sww_tag *t) {
    return t && sww_file_valid(&t->file) && t->offset >= t->file.header_size &&
           t->offset <= t->file.size && t->size >= 12 &&
           t->size <= t->file.size - t->offset &&
           t->header_size >= 12 && t->header_size <= t->size;
}
/* The generic envelope needs 12 bytes, even for unknown tags. The caller must
 * advance by tag.size and bound the number of tags by SDKW_TAGS_MAX. */
static inline int sww_tag_header(const uint8_t *p, size_t available,
                                 const struct sww_file *file, uint64_t offset,
                                 struct sww_tag *out) {
    if (!p || !out || !sww_file_valid(file)) return SDKW_INVALID;
    if (offset < file->header_size || offset > file->size ||
        file->size - offset < 12 || available < 12) return SDKW_MALFORMED;
    struct sww_tag t = {*file, sww_be32(p), (uint32_t) offset,
                        sww_be32(p + 4), sww_be32(p + 8)};
    if (!sww_tag_valid(&t)) return SDKW_MALFORMED;
    *out = t;
    return SDKW_OK;
}
/* Canonical stored layouts only. A zero stride or inconsistent payload is
 * malformed; unfamiliar nonzero strides, headers or header words are unsupported.
 * Header words are recognized, not generalized into a rate formula. */
static inline int sww_index_header(const uint8_t *p, size_t available, const struct sww_tag *tag, uint32_t header) {
    if (tag->header_size < header || available < header) return SDKW_MALFORMED;
    if (sww_be32(p) != tag->fourcc || sww_be32(p + 4) != tag->header_size || sww_be32(p + 8) != tag->size) return SDKW_MALFORMED;
    return tag->header_size == header ? SDKW_OK : SDKW_UNSUPPORTED;
}
static inline int sww_index_mono_preview(const uint8_t *p, const struct sww_tag *tag, uint32_t encoding,
                                         struct sww_format format, struct sww_index *out) {
    /* Mono preview count precedes its opaque header word. */
    uint32_t count = sww_be32(p + 12);
    if (count != tag->size - tag->header_size) return SDKW_MALFORMED;
    if (sww_be32(p + 16) != format.header_word) return SDKW_UNSUPPORTED;
    *out = (struct sww_index) {*tag, encoding, 1, count};
    return count ? SDKW_OK : SDKW_EMPTY;
}
static inline int sww_index_strided(const uint8_t *p, const struct sww_tag *tag, uint32_t encoding,
                                    struct sww_format format, struct sww_index *out) {
    uint32_t stride = sww_be32(p + 12), count = sww_be32(p + 16);
    uint32_t payload = tag->size - tag->header_size;
    if (!stride || payload % stride || count != payload / stride) return SDKW_MALFORMED;
    if (stride != format.entry_size || (tag->header_size == 24 && sww_be32(p + 20) != format.header_word)) return SDKW_UNSUPPORTED;
    *out = (struct sww_index) {*tag, encoding, stride, count};
    return count ? SDKW_OK : SDKW_EMPTY;
}
static inline int sww_index_detail(const uint8_t *p, size_t available,
                                   const struct sww_tag *tag, uint32_t encoding, struct sww_index *out) {
    if (!p || !out || !sww_tag_valid(tag)) return SDKW_INVALID;
    struct sww_format format = {0};
    int rc = sww_format_for(encoding, &format);
    if (rc != SDKW_OK) return rc;
    if (tag->fourcc != format.fourcc) return SDKW_UNSUPPORTED;
    rc = sww_index_header(p, available, tag, sww_header_bytes(encoding));
    if (rc != SDKW_OK) return rc;
    return sww_is_mono_preview(encoding) ? sww_index_mono_preview(p, tag, encoding, format, out)
                                         : sww_index_strided(p, tag, encoding, format, out);
}
/* Original mono entry point remains source-compatible. */
static inline int sww_index_pwv3(const uint8_t *p, size_t available,
                                 const struct sww_tag *tag, struct sww_index *out) {
    return sww_index_detail(p, available, tag, SDKW_ENCODING_PWV3, out);
}
static inline int sww_index_valid(const struct sww_index *index) {
    if (!index || !sww_tag_valid(&index->tag)) return 0;
    struct sww_format format = {0};
    if (sww_format_for(index->encoding, &format) != SDKW_OK) return 0;
    uint32_t payload = index->tag.size - index->tag.header_size;
    return index->tag.fourcc == format.fourcc && index->tag.header_size == sww_header_bytes(index->encoding) &&
           index->entry_size == format.entry_size && payload % format.entry_size == 0 &&
           index->sample_count == payload / format.entry_size;
}
/* Full-memory convenience. Validate the entire envelope chain, including tags
 * after the requested encoding. Duplicate targets are ambiguous. */
static inline int sww_find_tag(const uint8_t *p, size_t size, const struct sww_file *file,
                               uint32_t fourcc, struct sww_tag *selected) {
    uint32_t found = 0, tags = 0, offset = file->header_size;
    while (offset < file->size) {
        if (++tags > SDKW_TAGS_MAX) return SDKW_LIMIT;
        struct sww_tag tag = {0};
        int rc = sww_tag_header(p + offset, size - offset, file, offset, &tag);
        if (rc != SDKW_OK) return rc;
        if (tag.fourcc == fourcc) {
            *selected = tag;
            found++;
        }
        offset += tag.size;
    }
    if (!found) return SDKW_NOT_FOUND;
    return found == 1 ? SDKW_OK : SDKW_AMBIGUOUS;
}
static inline int sww_find_detail(const uint8_t *p, size_t size, uint32_t encoding, struct sww_index *out) {
    if (!p || !out) return SDKW_INVALID;
    struct sww_format format = {0};
    int rc = sww_format_for(encoding, &format);
    if (rc != SDKW_OK) return rc;
    struct sww_file file = {0};
    rc = sww_file_header(p, size, size, &file);
    if (rc != SDKW_OK) return rc;
    struct sww_tag selected = {0};
    rc = sww_find_tag(p, size, &file, format.fourcc, &selected);
    if (rc != SDKW_OK) return rc;
    return sww_index_detail(p + selected.offset, size - selected.offset, &selected, encoding, out);
}
static inline int sww_find_pwv3(const uint8_t *p, size_t size, struct sww_index *out) {
    return sww_find_detail(p, size, SDKW_ENCODING_PWV3, out);
}
/* Requests name source columns, not seconds or output pixels. A tail window is
 * clipped to EOF; nonempty first>=total is out of range. Empty source accepts
 * only first=0. Time is stored-analysis source time, not player/DAC alignment. */
static inline int sww_axis_valid(const struct sww_index *index, double seconds_per_sample) {
    return isfinite(seconds_per_sample) && seconds_per_sample >= 0 &&
           (!index->sample_count || seconds_per_sample != 0);
}
static inline int sww_first_valid(uint32_t total, uint32_t first) {
    return total ? first < total : first == 0;
}
static inline int sww_plan_window_axis(const struct sww_index *index, uint32_t first,
                                       uint32_t requested, double seconds_per_sample, struct sww_window *out) {
    if (!out || !sww_index_valid(index) || !requested) return SDKW_INVALID;
    if (!sww_axis_valid(index, seconds_per_sample)) return SDKW_INVALID;
    if (requested > SDKW_WINDOW_MAX) return SDKW_LIMIT;
    uint32_t total = index->sample_count;
    if (!sww_first_valid(total, first)) return SDKW_OUT_OF_RANGE;
    uint32_t count = total - first;
    if (count > requested) count = requested;
    struct sww_window w = {index->encoding, total, first, count,
                           (uint64_t) index->tag.offset + index->tag.header_size + (uint64_t) first * index->entry_size,
                           (size_t) count * index->entry_size, first * seconds_per_sample, seconds_per_sample};
    *out = w;
    return count ? SDKW_OK : SDKW_EMPTY;
}
/* Detailed analysis is sampled at 150 Hz. A preview covers the whole track and
 * must never inherit that clock merely because its packed column width matches. */
static inline int sww_plan_window(const struct sww_index *index, uint32_t first,
                                  uint32_t requested, struct sww_window *out) {
    if (!out || !sww_index_valid(index)) return SDKW_INVALID;
    if (sww_is_preview(index->encoding)) return SDKW_UNSUPPORTED;
    int rc = sww_plan_window_axis(index, first, requested, 1.0 / 150.0, out);
    if (rc == SDKW_OK || rc == SDKW_EMPTY) out->start_s = first / 150.0;
    return rc;
}
/* Duration normally comes from the integer-second library metadata. Its resulting
 * source-time axis is approximate; packed samples are not gain/gamma calibrated. */
static inline int sww_plan_preview_window(const struct sww_index *index, uint32_t first,
                                          uint32_t requested, double duration_s, struct sww_window *out) {
    if (!out || !sww_index_valid(index) || !sww_is_preview(index->encoding)) return SDKW_INVALID;
    if (!isfinite(duration_s) || duration_s <= 0) return SDKW_UNSUPPORTED;
    double step = index->sample_count ? duration_s / index->sample_count : 0;
    return sww_plan_window_axis(index, first, requested, step, out);
}
/* raw points to the beginning of the planned span, not the beginning of the file.
 * memmove permits an in-place/overlapping packed-byte copy. All checks precede writes. */
static inline int sww_window_extent(const struct sww_window *window, uint32_t entry_size) {
    return window->first_sample <= window->total_samples &&
           window->sample_count <= window->total_samples - window->first_sample &&
           window->byte_count == (size_t) window->sample_count * entry_size;
}
static inline int sww_copy_capacity(const uint8_t *raw, size_t available, uint8_t *out, size_t capacity, size_t bytes) {
    if (!raw || !out) return SDKW_INVALID;
    if (available < bytes) return SDKW_MALFORMED;
    return capacity < bytes ? SDKW_LIMIT : SDKW_OK;
}
static inline int sww_copy_window(const struct sww_window *window, const uint8_t *raw,
                                  size_t available, uint8_t *out, size_t capacity) {
    if (!window) return SDKW_INVALID;
    struct sww_format format = {0};
    int rc = sww_format_for(window->encoding, &format);
    if (rc != SDKW_OK) return rc;
    if (window->sample_count > SDKW_WINDOW_MAX) return SDKW_LIMIT;
    if (!sww_window_extent(window, format.entry_size)) return SDKW_INVALID;
    if (!window->sample_count) return SDKW_EMPTY;
    rc = sww_copy_capacity(raw, available, out, capacity, window->byte_count);
    if (rc != SDKW_OK) return rc;
    memmove(out, raw, window->byte_count);
    return SDKW_OK;
}
/* Decode a small read span. Input/output arrays must not overlap. These are
 * independent packed fields; raw 0xff means height31/intensity7, not height255. */
static inline int sww_decode_pwv3(const uint8_t *raw, size_t available, uint32_t count,
                                  struct sww_sample *out, size_t capacity) {
    if (count > SDKW_WINDOW_MAX) return SDKW_LIMIT;
    if (!count) return SDKW_EMPTY;
    if (!raw || !out) return SDKW_INVALID;
    if (available < count) return SDKW_MALFORMED;
    if (capacity < count) return SDKW_LIMIT;
    for (uint32_t i = 0; i < count; i++) {
        struct sww_sample s = {raw[i], (uint8_t) (raw[i] & 31), (uint8_t) (raw[i] >> 5), 0};
        out[i] = s;
    }
    return SDKW_OK;
}
/* Decode BE16 bit fields without color scaling or low-bit interpretation.
 * Input/output arrays must not overlap. capacity counts decoded columns. */
static inline int sww_decode_pwv5(const uint8_t *raw, size_t available, uint32_t count,
                                  struct sww_rgb_sample *out, size_t capacity) {
    if (count > SDKW_WINDOW_MAX) return SDKW_LIMIT;
    if (!count) return SDKW_EMPTY;
    if (!raw || !out) return SDKW_INVALID;
    if (available / 2 < count) return SDKW_MALFORMED;
    if (capacity < count) return SDKW_LIMIT;
    for (uint32_t i = 0; i < count; i++) {
        uint16_t word = (uint16_t) ((uint16_t) raw[i * 2] << 8 | raw[i * 2 + 1]);
        struct sww_rgb_sample sample = {word, (uint8_t) (word >> 13),
                                        (uint8_t) ((word >> 10) & 7), (uint8_t) ((word >> 7) & 7),
                                        (uint8_t) ((word >> 2) & 31), (uint8_t) (word & 3), 0};
        out[i] = sample;
    }
    return SDKW_OK;
}
/* PWV7 byte order is low/mid/high for the supported export family. These are
 * uncalibrated analysis bytes, not physical frequency bins or player pixels. */
static inline int sww_decode_pwv7(const uint8_t *raw, size_t available, uint32_t count,
                                  struct sww_band_sample *out, size_t capacity) {
    if (count > SDKW_WINDOW_MAX) return SDKW_LIMIT;
    if (!count) return SDKW_EMPTY;
    if (!raw || !out) return SDKW_INVALID;
    if (available / 3 < count) return SDKW_MALFORMED;
    if (capacity < count) return SDKW_LIMIT;
    for (uint32_t i = 0; i < count; i++) {
        struct sww_band_sample sample = {raw[i * 3], raw[i * 3 + 1], raw[i * 3 + 2], 0};
        out[i] = sample;
    }
    return SDKW_OK;
}
/* Private raw layered schema, not a calibrated waveform/pixel capability.
 * A/B/C mean input bytes0/1/2 only. Subset IDs are native symbolic palette IDs,
 * never RGB values or frequency cutoffs. Magnitudes stay absolute raw bytes.
 * Output stride is6, with ONLY bytes0..4 defined/written. Byte5 is native malloc
 * padding and remains caller-owned/unchanged. Input/output must not overlap.
 * This helper does not acquire provider/content lifetime or invoke native code.
 */
#define SDKW_LAYERED3_VERSION 1
enum {
    SDKW_LAYER_CHANNELS = 3,
    SDKW_LAYER_RECORD_BYTES = 6
};
static inline void sww_layer_sort(uint8_t levels[SDKW_LAYER_CHANNELS]) {
    for (unsigned a = 0; a + 1 < SDKW_LAYER_CHANNELS; ++a)
        for (unsigned b = a + 1; b < SDKW_LAYER_CHANNELS; ++b)
            if (levels[a] < levels[b]) {
                uint8_t temporary = levels[a];
                levels[a] = levels[b];
                levels[b] = temporary;
            }
}
static inline unsigned sww_layer_subset(const uint8_t *input, uint8_t level) {
    return (input[0] >= level ? 1u : 0u) | (input[1] >= level ? 2u : 0u) | (input[2] >= level ? 4u : 0u);
}
static inline void sww_layer_record(const uint8_t *input, uint8_t *record) {
    static const uint8_t subset_id[8] = {0, 3, 2, 6, 1, 5, 4, 8};
    uint8_t levels[SDKW_LAYER_CHANNELS] = {input[0], input[1], input[2]};
    sww_layer_sort(levels);
    uint8_t magnitude[SDKW_LAYER_CHANNELS] = {0, 0, 0};
    uint16_t header = 0;
    unsigned layers = 0;
    for (unsigned j = 0; j < SDKW_LAYER_CHANNELS; ++j) {
        uint8_t level = levels[j];
        if (!level || (j && level == levels[j - 1])) continue;
        header |= (uint16_t) (subset_id[sww_layer_subset(input, level)] << (4 * layers));
        magnitude[layers++] = level;
    }
    if (!layers) { /* Native allzero record still has one layer. */
        layers = 1;
        header = 8;
    }
    header |= (uint16_t) (layers << 12);
    record[0] = (uint8_t) header;
    record[1] = (uint8_t) (header >> 8);
    record[2] = magnitude[0];
    record[3] = magnitude[1];
    record[4] = magnitude[2];
}
static inline int sww_encode_pwv7_layers(const uint8_t *raw, size_t available, uint32_t count,
                                         uint8_t *out, size_t out_bytes) {
    if (count > SDKW_WINDOW_MAX) return SDKW_LIMIT;
    if (!count) return SDKW_EMPTY;
    if (!raw || !out) return SDKW_INVALID;
    if (available / SDKW_LAYER_CHANNELS < count) return SDKW_MALFORMED;
    if (out_bytes / SDKW_LAYER_RECORD_BYTES < count) return SDKW_LIMIT;
    for (uint32_t i = 0; i < count; ++i)
        sww_layer_record(raw + i * SDKW_LAYER_CHANNELS, out + i * SDKW_LAYER_RECORD_BYTES);
    return SDKW_OK;
}
/* Test/convenience wrapper; production streaming can use the stages above. */
static inline int sww_readable(int status) {
    return status == SDKW_OK || status == SDKW_EMPTY;
}
static inline int sww_read_memory(const uint8_t *p, size_t size, uint32_t first,
                                  uint32_t requested, struct sww_sample *samples,
                                  size_t capacity, struct sww_window *out) {
    if (!out) return SDKW_INVALID;
    struct sww_index index = {0};
    int rc = sww_find_pwv3(p, size, &index);
    if (!sww_readable(rc)) return rc;
    struct sww_window w = {0};
    rc = sww_plan_window(&index, first, requested, &w);
    if (!sww_readable(rc)) return rc;
    if (w.byte_offset > size || w.byte_count > size - (size_t) w.byte_offset) return SDKW_MALFORMED;
    rc = sww_decode_pwv3(p + (size_t) w.byte_offset, w.byte_count, w.sample_count,
                         samples, capacity);
    if (sww_readable(rc)) *out = w;
    return rc;
}
#endif
