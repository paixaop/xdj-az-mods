/* SPDX-License-Identifier: GPL-3.0-only */
/* Worker-only streaming reader. Retains at most one bounded detail window. */
#ifndef AZMOD_SDK_WAVE_PROVIDER_H
#define AZMOD_SDK_WAVE_PROVIDER_H
#include "sdk_waveform.h"
#include <errno.h>
_Static_assert(SDKW_WINDOW_MAX == AZ_WAVE_WINDOW_MAX, "wave window limits must match");
static int st_wave_status(int rc) {
    switch (rc) {
    case SDKW_OK:
    case SDKW_EMPTY: return AZ_OK;
    case SDKW_NOT_FOUND:
    case SDKW_UNSUPPORTED: return AZ_UNSUPPORTED;
    case SDKW_OUT_OF_RANGE:
    case SDKW_INVALID: return AZ_INVALID;
    case SDKW_LIMIT: return AZ_LIMIT;
    default: return AZ_FAILED;
    }
}
static int st_wave_pread(int fd, void *out, size_t bytes, uint64_t offset) {
    uint8_t *p = out;
    while (bytes) {
        ssize_t n = pread(fd, p, bytes, (off_t) offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return AZ_FAILED;
        bytes -= (size_t) n;
        p += n;
        offset += (uint64_t) n;
    }
    return AZ_OK;
}
/* Derived representations keep the canonical source tag/path selection in one
 * place. A raw PWV7 request continues to return its original three bytes. */
static uint32_t st_wave_file_encoding(uint32_t encoding) {
    return encoding == AZ_WAVE_PWV7_LAYERED_RAW ? (uint32_t) AZ_WAVE_PWV7_BANDS : encoding;
}
static int st_wave_read(const char *path, struct st_data *data) {
    struct sww_format format = {0};
    const int layered = data->wave_query.encoding == AZ_WAVE_PWV7_LAYERED_RAW;
    uint32_t file_encoding = st_wave_file_encoding(data->wave_query.encoding);
    int rc = st_wave_status(sww_format_for(file_encoding, &format));
    if (rc) return rc;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT || errno == ENOTDIR ? AZ_UNSUPPORTED : AZ_FAILED;
    struct stat sb;
    uint8_t header[24];
    struct sww_file file = {0};
    struct sww_tag selected = {0};
    rc = AZ_FAILED;
    if (fstat(fd, &sb) || !S_ISREG(sb.st_mode) || sb.st_size < 12 ||
        (uint64_t) sb.st_size > SDKW_FILE_MAX || st_wave_pread(fd, header, 12, 0)) goto done;
    rc = st_wave_status(sww_file_header(header, 12, (size_t) sb.st_size, &file));
    if (rc) goto done;
    uint32_t target = format.fourcc;
    uint32_t offset = file.header_size, tags = 0, found = 0;
    while (offset < file.size) {
        if (++tags > SDKW_TAGS_MAX) {
            rc = AZ_LIMIT;
            goto done;
        }
        if (file.size - offset < 12 || st_wave_pread(fd, header, 12, offset)) {
            rc = AZ_FAILED;
            goto done;
        }
        struct sww_tag tag = {0};
        rc = st_wave_status(sww_tag_header(header, 12, &file, offset, &tag));
        if (rc) goto done;
        if (tag.fourcc == target) {
            selected = tag;
            found++;
        }
        offset += tag.size;
    }
    if (!found) {
        rc = AZ_UNSUPPORTED;
        goto done;
    }
    if (found != 1) {
        rc = AZ_FAILED;
        goto done;
    }
    uint32_t required = sww_header_bytes(file_encoding);
    if (selected.size < required || st_wave_pread(fd, header, required, selected.offset)) {
        rc = AZ_FAILED;
        goto done;
    }
    struct sww_index index = {0};
    rc = st_wave_status(sww_index_detail(header, required, &selected, file_encoding, &index));
    if (rc) goto done;
    struct sww_window window = {0};
    if (sww_is_preview(index.encoding)) {
        if (!(data->view.metadata.valid & AZ_META_DURATION)) {
            rc = AZ_UNSUPPORTED;
            goto done;
        }
        rc = st_wave_status(sww_plan_preview_window(&index, data->wave_query.first_column,
                                                    data->wave_query.column_count, data->view.metadata.duration_s, &window));
    } else rc = st_wave_status(sww_plan_window(&index, data->wave_query.first_column,
                                               data->wave_query.column_count, &window));
    if (rc) goto done;
    if (window.byte_count > sizeof data->detail ||
        (layered && window.sample_count > sizeof data->detail / SDKW_LAYER_RECORD_BYTES)) {
        rc = AZ_LIMIT;
        goto done;
    }
    rc = st_wave_pread(fd, data->detail, window.byte_count, window.byte_offset);
    if (rc) goto done;
    if (layered) {
        /* Expand backward inside the private, unpublished window. Each input
         * triple is copied first, so the encoder's arrays never overlap and
         * no expanded record overwrites a lower, unprocessed input triple. */
        for (uint32_t remaining = window.sample_count; remaining; --remaining) {
            uint32_t column = remaining - 1;
            uint8_t raw[SDKW_LAYER_CHANNELS];
            memcpy(raw, data->detail + column * SDKW_LAYER_CHANNELS, sizeof raw);
            uint8_t *record = data->detail + column * SDKW_LAYER_RECORD_BYTES;
            rc = st_wave_status(sww_encode_pwv7_layers(raw, sizeof raw, 1, record, SDKW_LAYER_RECORD_BYTES));
            if (rc) goto done;
            record[SDKW_LAYER_RECORD_BYTES - 1] = 0;
        }
    }
    az_wave_snapshot *v = &data->wave_view;
    v->encoding = data->wave_query.encoding;
    v->first_column = window.first_sample;
    v->column_count = window.sample_count;
    v->total_columns = window.total_samples;
    v->bytes_per_column = layered ? (uint32_t) SDKW_LAYER_RECORD_BYTES : index.entry_size;
    v->data_bytes = window.sample_count * v->bytes_per_column;
    v->start_s = window.start_s;
    v->seconds_per_column = window.seconds_per_sample;
    v->info.availability = window.sample_count ? AZ_AVAIL_READY : AZ_AVAIL_EMPTY;
done:
    close(fd);
    return rc;
}
#endif
