/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "source_import.h"
#include "sdk_analysis.h"

#include <stdio.h>
#include <stdlib.h>

enum {
    IMPORT_PATH = 1u,
    IMPORT_WAVEFORM = 2u,
    IMPORT_GRID = 4u
};

struct import_anlz {
    const char *audio, *waveform;
    unsigned seen;
    uint32_t bpm_x100;
};

static int import_asset_path(const struct st_source *source, const char *relative, char path[1024],
                             struct stat *st) {
    return st_source_file(source, relative, path) && !stat(path, st) &&
           (uint64_t) st->st_dev == source->device && (uint64_t) st->st_ino != source->inode && st->st_size > 0;
}

static int import_asset_read(const struct st_source *source, struct azspi_asset *asset,
                             const char *relative, uint64_t *size) {
    char path[1024];
    struct stat st;
    if (!import_asset_path(source, relative, path, &st))
        return AZ_STALE;
    uint64_t sig = st_stat_sig(&st);
    if (!sig)
        return AZ_INVALID;
    if (snprintf(asset->relative, sizeof(asset->relative), "%s", relative) >= (int) sizeof(asset->relative))
        return AZ_LIMIT;
    asset->signature = sig;
    if (size)
        *size = (uint64_t) st.st_size;
    return AZ_OK;
}

static int import_utf16_path(const uint8_t *tag, size_t size, const char *audio) {
    if (size < 18 || hc_be32(tag + 4) != 16)
        return 0;
    size_t bytes = hc_be32(tag + 12);
    if (bytes != size - 16 || (bytes & 1u) || hc_be16(tag + size - 2))
        return 0;
    for (size_t at = 16; at < size - 2; at += 2) {
        if (!hc_be16(tag + at))
            return 0;
    }
    char decoded[AZ_SOURCE_PATH_BYTES];
    st_comment(tag + 16, bytes, decoded, sizeof(decoded));
    return !strcmp(decoded, audio);
}

static int import_grid(struct import_anlz *scan, const uint8_t *tag, size_t size) {
    size_t header = hc_be32(tag + 4);
    if (header < 24 || (scan->seen & IMPORT_GRID))
        return 0;
    uint32_t count = hc_be32(tag + 20);
    if (count > ST_BEATS || count > (size - header) / 8)
        return 0;
    if (count)
        scan->bpm_x100 = hc_be16(tag + header + 2);
    scan->seen |= IMPORT_GRID;
    return 1;
}

static int import_path_tag(struct import_anlz *scan, const uint8_t *tag, size_t size) {
    if ((scan->seen & IMPORT_PATH) || !import_utf16_path(tag, size, scan->audio))
        return 0;
    scan->seen |= IMPORT_PATH;
    return 1;
}

static int import_visit(struct import_anlz *scan, const uint8_t *tag, size_t size) {
    if (!memcmp(tag, "PPTH", 4))
        return import_path_tag(scan, tag, size);
    if (!memcmp(tag, "PQTZ", 4))
        return import_grid(scan, tag, size);
    if (!memcmp(tag, scan->waveform, 4) && size > hc_be32(tag + 4))
        scan->seen |= IMPORT_WAVEFORM;
    return 1;
}

static int import_scan(struct import_anlz *scan, const uint8_t *data, size_t size) {
    if (!st_container_header(data, size))
        return 0;
    for (size_t at = hc_be32(data + 4); at < size;) {
        const uint8_t *tag = data + at;
        size_t length = st_chunk_length(tag, size - at);
        if (!length || !import_visit(scan, tag, length))
            return 0;
        at += length;
    }
    return 1;
}

static int import_file_anlz(struct azspi_record *record, int file, const char *relative,
                            const char *waveform, unsigned required) {
    struct azspi_asset *asset = &record->analysis[file];
    int status = import_asset_read(&record->source, asset, relative, NULL);
    if (status != AZ_OK)
        return status;
    char path[1024];
    struct stat st;
    if (!import_asset_path(&record->source, relative, path, &st))
        return AZ_STALE;
    size_t size = 0;
    uint8_t *data = st_read_file_checked(path, &size, record->source.device, st.st_ino, asset->signature);
    struct import_anlz scan = {.audio = record->audio.relative, .waveform = waveform};
    int ok = import_scan(&scan, data, size) && (scan.seen & required) == required;
    free(data);
    if (!ok)
        return AZ_FAILED;
    if (file == 0)
        record->bpm_x100 = scan.bpm_x100;
    return AZ_OK;
}

static int import_dat_path(const char *name) {
    size_t length = strlen(name);
    return length > 4 && !strncmp(name, "/PIONEER/USBANLZ/", 17) &&
           !strcmp(name + length - 4, ".DAT");
}

static int import_fileset(struct azspi_record *record, const char *dat) {
    static const char *const extensions[] = {"DAT", "EXT", "2EX"};
    static const char *const waveforms[] = {"PWAV", "PWV3", "PWV6"};
    size_t stem = strlen(dat) - 3;
    if (stem + 4 > AZ_SOURCE_PATH_BYTES)
        return AZ_LIMIT;
    char path[AZ_SOURCE_PATH_BYTES];
    snprintf(path, sizeof path, "%s", dat);
    for (int file = 0; file < 3; ++file) {
        memcpy(path + stem, extensions[file], 4);
        unsigned required = IMPORT_PATH | IMPORT_WAVEFORM | (file == 0 ? IMPORT_GRID : 0);
        int status = import_file_anlz(record, file, path, waveforms[file], required);
        if (status != AZ_OK)
            return status;
    }
    return AZ_OK;
}

static int import_asset_current(const struct st_source *source, const struct azspi_asset *asset) {
    struct stat st;
    char path[1024];
    return asset->signature && import_asset_path(source, asset->relative, path, &st) &&
           asset->signature == st_stat_sig(&st);
}

int azspi_current(const struct azspi_record *record) {
    if (!import_asset_current(&record->source, &record->audio))
        return AZ_STALE;
    for (int file = 0; file < 3; ++file) {
        if (!import_asset_current(&record->source, &record->analysis[file]))
            return AZ_STALE;
    }
    return AZ_OK;
}

static int import_metadata_valid(const az_source_track_v1 *track, const struct azspi_audio *format) {
    return track->title[0] && memchr(track->title, 0, sizeof(track->title)) &&
           memchr(track->artist, 0, sizeof(track->artist)) && memchr(track->album, 0, sizeof(track->album)) &&
           format->file_type && format->sample_rate;
}

int azspi_inspect(const struct st_source *source, const az_source_track_v1 *track,
                  const struct azspi_audio *format, const char *audio, const char *dat,
                  struct azspi_record *out) {
    if (!import_dat_path(dat) || !import_metadata_valid(track, format))
        return AZ_INVALID;
    struct azspi_record record = {.source = *source, .track = *track, .format = *format};
    int status = import_asset_read(source, &record.audio, audio, &record.file_size);
    if (status == AZ_OK)
        status = import_fileset(&record, dat);
    if (status == AZ_OK)
        status = azspi_current(&record);
    if (status == AZ_OK)
        *out = record;
    return status;
}
