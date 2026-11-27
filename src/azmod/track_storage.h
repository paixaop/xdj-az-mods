/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef AZ_TRACK_STORAGE_H
#define AZ_TRACK_STORAGE_H
#include "azmod-mod.h"
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#pragma GCC visibility push(hidden)
struct st_source {
    az_track_ref ref;
    uint64_t mount_id, device, inode;
    char mount[1024];
    int hfsplus;
};
/* Worker-only, one positive locator per physical source. This holds copied
 * values, never a SQLite handle, statement, native pointer or waveform window. */
struct st_resolution {
    struct st_source source;
    uint64_t signature;
    az_track_metadata metadata;
    int64_t duration_ticks; /* original OneLibrary integral seconds */
    char database[1024], content[2048], analysis[1024];
    int valid, analysis_limited, analysis_generated;
};

uint64_t st_stat_sig(const struct stat *status);
uint64_t st_disk_sig(const char *path);
int st_hash_file(const char *path, uint64_t signature, char digest[65]);
int st_source_read(int stick, struct st_source *out);
int st_source_assign(struct st_source *source, uint64_t *generation, int stick, const char *mount,
                     uint64_t mount_id, int hfsplus, struct stat status);
int st_source_path(const char *mount, const char *relative, char out[1024]);
const char *st_source_export(const struct st_source *source);
int st_source_file(const struct st_source *source, const char *relative, char out[1024]);
int st_source_database(const struct st_source *source, char out[1024]);
int st_database_quiet(const char *database);
uint8_t *st_read_file_checked(const char *path, size_t *size, uint64_t device, uint64_t inode, uint64_t signature);
uint8_t *st_read_file(const char *path, size_t *size);
int st_resolution_source_equal(const struct st_source *left, const struct st_source *right);
int st_read_row_checked(void *database, uint32_t track, struct st_resolution *row, int *ambiguous);
int st_read_row(void *database, uint32_t track, struct st_resolution *row);
/* Native host adapters supply mount topology and the process SQLite binding.
 * They expose no native object or mutable service state. */
int az_storage_stick_mount(int stick, char *mount, size_t capacity);
const char *az_storage_mountinfo(void);
#pragma GCC visibility pop
#endif
