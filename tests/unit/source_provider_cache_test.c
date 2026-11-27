/* SPDX-License-Identifier: GPL-3.0-only */
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 700
#include "source_provider_cache.h"

#include <assert.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void join_path(char out[AZ_SOURCE_PATH_BYTES], const char *root, const char *name) {
    int length = snprintf(out, AZ_SOURCE_PATH_BYTES, "%s/%s", root, name);
    assert(length >= 0 && (unsigned) length < AZ_SOURCE_PATH_BYTES);
}

static void put(const char *path) {
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file >= 0);
    assert(write(file, "audio", 5) == 5 && close(file) == 0);
}

static int clean(const char *path, const struct stat *st, int kind, struct FTW *walk) {
    (void) st;
    (void) kind;
    (void) walk;
    return remove(path);
}

static void hold_audio(struct azsp_cache *cache, char audio[AZ_SOURCE_PATH_BYTES]) {
    struct azsp_stage *stage = NULL;
    assert(azsp_stage_create(azsp_cache_root(cache), AZ_SOURCE_STREAMING, &stage) == AZ_OK);
    join_path(audio, azsp_stage_directory(stage), "track.m4a");
    put(audio);
    assert(azsp_cache_hold(cache, stage) == AZ_OK);
    assert(azsp_cache_hold(cache, stage) == AZ_OK);
    azsp_stage_release(stage);
    assert(access(audio, R_OK) == 0);
}

static void serve_audio(const char *parent, int ready, int stop) {
    struct azsp_cache *cache = NULL;
    assert(azsp_cache_open(parent, &cache) == AZ_OK);
    char audio[AZ_SOURCE_PATH_BYTES] = {0};
    hold_audio(cache, audio);
    assert(write(ready, audio, sizeof audio) == (ssize_t) sizeof audio);
    char value;
    assert(read(stop, &value, 1) == 1 && value == 'q');
    assert(access(audio, R_OK) == 0);
}

static void verify_owner_lifetime(const char *parent) {
    int ready[2], stop[2];
    assert(pipe(ready) == 0 && pipe(stop) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(ready[0]);
        close(stop[1]);
        serve_audio(parent, ready[1], stop[0]);
        _exit(0);
    }
    close(ready[1]);
    close(stop[0]);
    char audio[AZ_SOURCE_PATH_BYTES];
    assert(read(ready[0], audio, sizeof audio) == (ssize_t) sizeof audio);
    close(ready[0]);
    struct azsp_cache *first, *next;
    assert(azsp_cache_open(parent, &first) == AZ_OK);
    assert(access(audio, R_OK) == 0);
    char active[AZ_SOURCE_PATH_BYTES] = {0};
    hold_audio(first, active);
    assert(write(stop[1], "q", 1) == 1 && close(stop[1]) == 0);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(azsp_cache_open(parent, &next) == AZ_OK);
    assert(strcmp(azsp_cache_root(first), azsp_cache_root(next)) != 0);
    assert(access(audio, F_OK) != 0 && access(active, R_OK) == 0);
    azsp_cache_discard(next);
    azsp_cache_discard(first);
    assert(access(active, F_OK) != 0);
}

static void reject_unowned(const char *parent) {
    struct azsp_cache *cache = NULL;
    assert(azsp_cache_open(NULL, &cache) == AZ_INVALID);
    assert(azsp_cache_open(parent, NULL) == AZ_INVALID);
    assert(azsp_cache_open("relative", &cache) == AZ_INVALID);
    char path[AZ_SOURCE_PATH_BYTES];
    join_path(path, parent, "world");
    assert(mkdir(path, 0700) == 0 && chmod(path, 0755) == 0);
    assert(azsp_cache_open(path, &cache) == AZ_INVALID);
    join_path(path, parent, "symlink");
    assert(symlink(parent, path) == 0);
    assert(azsp_cache_open(path, &cache) == AZ_FAILED);
    assert(azsp_cache_open(parent, &cache) == AZ_OK);
    struct azsp_stage *outside, *import;
    assert(azsp_stage_create(parent, AZ_SOURCE_STREAMING, &outside) == AZ_OK);
    assert(azsp_stage_create(azsp_cache_root(cache), AZ_SOURCE_IMPORT, &import) == AZ_OK);
    assert(azsp_cache_hold(cache, outside) == AZ_INVALID);
    assert(azsp_cache_hold(cache, import) == AZ_INVALID);
    assert(azsp_cache_hold(cache, NULL) == AZ_INVALID);
    azsp_stage_release(outside);
    azsp_stage_release(import);
    azsp_cache_discard(cache);
}

static void verify_unrelated_paths(const char *parent) {
    char other[AZ_SOURCE_PATH_BYTES], lookalike[AZ_SOURCE_PATH_BYTES], audio[AZ_SOURCE_PATH_BYTES];
    join_path(other, parent, "unrelated");
    join_path(lookalike, parent, "session.aaaaaa");
    assert(mkdir(other, 0700) == 0 && symlink(other, lookalike) == 0);
    join_path(audio, other, "audio");
    put(audio);
    struct azsp_cache *cache;
    assert(azsp_cache_open(parent, &cache) == AZ_OK);
    assert(access(audio, R_OK) == 0 && access(lookalike, F_OK) == 0);
    azsp_cache_discard(cache);
}

int main(void) {
    char parent[] = "/tmp/az-provider-cache-test.XXXXXX";
    assert(mkdtemp(parent));
    verify_owner_lifetime(parent);
    reject_unowned(parent);
    verify_unrelated_paths(parent);
    assert(nftw(parent, clean, 8, FTW_DEPTH | FTW_PHYS | FTW_MOUNT) == 0);
    puts("source provider cache ok");
    return 0;
}
