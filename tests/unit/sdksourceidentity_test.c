/* SPDX-License-Identifier: GPL-3.0-only */
/* Exact production source publication, with deterministic mount/stat fixtures. */
#define _DEFAULT_SOURCE 1
#define _DARWIN_C_SOURCE 1
#include "azmod-mod.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static uint64_t mounts[2] = {100, 200}, devices[2] = {1, 2}, inodes[2] = {19, 29};
static int sdk_stick_mount(int stick, char *out, size_t capacity) {
    return snprintf(out, capacity, "/%d", stick) < (int) capacity;
}
static uint64_t st_mount_id(const char *mount) { return mounts[mount[1] - '0']; }
static int st_source_path(const char *mount, const char *relative, char out[1024]) {
    return snprintf(out, 1024, "%s%s", mount, relative) < 1024;
}
static int source_stat(const char *path, struct stat *out) {
    unsigned port = (unsigned) (path[1] - '0');
    assert(port < 2);
    memset(out, 0, sizeof *out);
    out->st_mode = S_IFREG | 0600;
    out->st_dev = (dev_t) devices[port];
    out->st_ino = (ino_t) inodes[port];
    return 0;
}
#define stat(path, out) source_stat(path, out)
#include SOURCE_IDENTITY_INC
#undef stat

int main(void) {
    struct st_source a, b, out;
    assert(st_source_read(0, &a) == AZ_OK && st_source_read(1, &b) == AZ_OK);
    assert(a.ref.source_id != b.ref.source_id && a.ref.source_generation != b.ref.source_generation);
    assert(st_source_read(0, &out) == AZ_OK && out.ref.source_generation == a.ref.source_generation);
    /* A library_id fingerprint is not an exact dev/inode equality test. Two
     * distinct physical DB tuples can collide without changing mount/path. */
    const uint64_t multiplier = UINT64_C(0x9e3779b185ebca87);
    devices[0] = 2;
    inodes[0] = (multiplier ^ UINT64_C(19)) ^ (UINT64_C(2) * multiplier);
    assert(st_source_read(0, &out) == AZ_OK);
    assert(out.ref.library_id == a.ref.library_id);
    assert(out.ref.source_generation != a.ref.source_generation);
    assert(st_source_read(1, &out) == AZ_OK && out.ref.source_generation == b.ref.source_generation);
    for (int port = 0; port < 2; ++port) {
        struct st_source old;
        assert(st_source_read(port, &old) == AZ_OK);
        mounts[port]++;
        assert(st_source_read(port, &out) == AZ_OK && out.ref.source_generation != old.ref.source_generation);
    }
    g_st_generation = UINT64_MAX - 1;
    mounts[0]++;
    assert(st_source_read(0, &a) == AZ_OK && a.ref.source_generation == UINT64_MAX);
    mounts[1]++;
    struct st_source before;
    memset(&before, 0x5a, sizeof before);
    out = before;
    assert(st_source_read(1, &out) == AZ_LIMIT && !memcmp(&before, &out, sizeof out));
    assert(g_st_generation == UINT64_MAX);
    assert(st_source_read(1, &out) == AZ_LIMIT && !memcmp(&before, &out, sizeof out));
    assert(st_source_read(0, &out) == AZ_OK && out.ref.source_generation == a.ref.source_generation);
    mounts[0]++;
    out = before;
    assert(st_source_read(0, &out) == AZ_LIMIT && !memcmp(&before, &out, sizeof out));
    puts("source identity: exact physical DB tuples, two-port reconnect and nonwrapping publication passed");
    return 0;
}
