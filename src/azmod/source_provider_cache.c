/* SPDX-License-Identifier: GPL-3.0-only */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include "source_provider_cache.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define CACHE_SESSION "session."
#define CACHE_LOCK ".owner"

struct cache_stage {
    struct azsp_stage *stage;
    struct cache_stage *next;
};

struct azsp_cache {
    char root[AZ_SOURCE_PATH_BYTES];
    int lock;
    pthread_mutex_t mutex;
    struct cache_stage *stages;
};

static bool private_directory(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_uid == geteuid() && !(st.st_mode & 077);
}

static bool private_lock(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 1 &&
           st.st_uid == geteuid() && !(st.st_mode & 077);
}

static int lock_directory(int fd, const char *name, int create) {
    int lock = openat(fd, name, O_RDWR | O_CLOEXEC | O_NOFOLLOW | create, 0600);
    if (lock < 0)
        return -1;
    if (private_lock(lock) && flock(lock, LOCK_EX | LOCK_NB) == 0)
        return lock;
    close(lock);
    return -1;
}

static int open_directory(const char *path) {
    return open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static int open_parent(const char *path) {
    if (mkdir(path, 0700) && errno != EEXIST)
        return -1;
    return open_directory(path);
}

static int remove_entry(const char *path, const struct stat *st, int kind,
                        struct FTW *context) {
    (void) st;
    (void) kind;
    (void) context;
    return remove(path);
}

static int remove_cache_entry(const char *path, const struct stat *st, int kind,
                              struct FTW *context) {
    if (!context->level || (context->level == 1 && !strcmp(path + context->base, CACHE_LOCK)))
        return 0;
    return remove_entry(path, st, kind, context);
}

static void remove_cache_root(const char *path, int fd) {
    if (nftw(path, remove_cache_entry, 8, FTW_DEPTH | FTW_PHYS | FTW_MOUNT) == 0) {
        (void) unlinkat(fd, CACHE_LOCK, 0);
        (void) rmdir(path);
    }
}

static bool session_name(const char *name) {
    const size_t offset = sizeof(CACHE_SESSION) - 1;
    if (strncmp(name, CACHE_SESSION, offset) != 0)
        return false;
    const char *tail = name + offset;
    return strlen(tail) == 6 && strspn(tail, "abcdefghijklmnopqrstuvwxyz"
                                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == 6;
}

static bool cache_path(char out[AZ_SOURCE_PATH_BYTES], const char *parent, const char *name) {
    int count = snprintf(out, AZ_SOURCE_PATH_BYTES, "%s/%s", parent, name);
    return (unsigned) count < AZ_SOURCE_PATH_BYTES;
}

static void reclaim_session(const char *path) {
    int fd = open_directory(path);
    if (fd < 0)
        return;
    int owner = private_directory(fd) ? lock_directory(fd, CACHE_LOCK, 0) : -1;
    if (owner >= 0) {
        remove_cache_root(path, fd);
        close(owner);
    }
    close(fd);
}

static void reclaim_sessions(int fd, const char *parent) {
    int stream = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (stream < 0)
        return;
    DIR *dir = fdopendir(stream);
    if (!dir) {
        close(stream);
        return;
    }
    for (struct dirent *entry; (entry = readdir(dir)); ) {
        char path[AZ_SOURCE_PATH_BYTES];
        if (session_name(entry->d_name) && cache_path(path, parent, entry->d_name))
            reclaim_session(path);
    }
    closedir(dir);
}

static int make_session(struct azsp_cache *cache, const char *parent) {
    if (!cache_path(cache->root, parent, CACHE_SESSION "XXXXXX"))
        return AZ_LIMIT;
    if (!mkdtemp(cache->root))
        return AZ_FAILED;
    int fd = open_directory(cache->root);
    if (fd < 0) {
        (void) rmdir(cache->root);
        return AZ_FAILED;
    }
    cache->lock = lock_directory(fd, CACHE_LOCK, O_CREAT | O_EXCL);
    close(fd);
    if (cache->lock >= 0)
        return AZ_OK;
    (void) nftw(cache->root, remove_entry, 8, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
    return AZ_FAILED;
}

static struct azsp_cache *alloc_cache(void) {
    struct azsp_cache *cache = calloc(1, sizeof(*cache));
    if (cache && pthread_mutex_init(&cache->mutex, NULL)) {
        free(cache);
        return NULL;
    }
    return cache;
}

static int create_cache(int parent_fd, const char *parent, struct azsp_cache **out) {
    int lock = lock_directory(parent_fd, CACHE_LOCK, O_CREAT);
    if (lock < 0)
        return AZ_BUSY;
    reclaim_sessions(parent_fd, parent);
    struct azsp_cache *cache = alloc_cache();
    int status = cache ? make_session(cache, parent) : AZ_FAILED;
    close(lock);
    if (status == AZ_OK)
        *out = cache;
    else if (cache) {
        pthread_mutex_destroy(&cache->mutex);
        free(cache);
    }
    return status;
}

int azsp_cache_open(const char *parent, struct azsp_cache **out) {
    if (!out)
        return AZ_INVALID;
    *out = NULL;
    if (!parent || parent[0] != '/' || strnlen(parent, AZ_SOURCE_PATH_BYTES) == AZ_SOURCE_PATH_BYTES)
        return AZ_INVALID;
    int fd = open_parent(parent);
    if (fd < 0)
        return AZ_FAILED;
    int status = private_directory(fd) ? create_cache(fd, parent, out) : AZ_INVALID;
    close(fd);
    return status;
}

const char *azsp_cache_root(const struct azsp_cache *cache) {
    return cache->root;
}

void azsp_cache_discard(struct azsp_cache *cache) {
    while (cache->stages) {
        struct cache_stage *node = cache->stages;
        cache->stages = node->next;
        azsp_stage_release(node->stage);
        free(node);
    }
    int fd = open_directory(cache->root);
    if (fd >= 0) {
        remove_cache_root(cache->root, fd);
        close(fd);
    }
    close(cache->lock);
    pthread_mutex_destroy(&cache->mutex);
    free(cache);
}

static bool can_hold(const struct azsp_cache *cache, const struct azsp_stage *stage) {
    if (!cache || !stage || azsp_stage_mode(stage) != AZ_SOURCE_STREAMING)
        return false;
    const char *directory = azsp_stage_directory(stage);
    size_t root_length = strlen(cache->root);
    return strncmp(directory, cache->root, root_length) == 0 &&
           directory[root_length] == '/' && !strchr(directory + root_length + 1, '/');
}

static const struct cache_stage *held_stage(const struct azsp_cache *cache,
                                           const struct azsp_stage *stage) {
    for (const struct cache_stage *node = cache->stages; node; node = node->next) {
        if (node->stage == stage)
            return node;
    }
    return NULL;
}

static int retain_stage(struct azsp_cache *cache, struct azsp_stage *stage) {
    if (held_stage(cache, stage))
        return AZ_OK;
    struct cache_stage *node = malloc(sizeof(*node));
    if (!node)
        return AZ_FAILED;
    azsp_stage_retain(stage);
    *node = (struct cache_stage) {.stage = stage, .next = cache->stages};
    cache->stages = node;
    return AZ_OK;
}

int azsp_cache_hold(struct azsp_cache *cache, struct azsp_stage *stage) {
    if (!can_hold(cache, stage))
        return AZ_INVALID;
    pthread_mutex_lock(&cache->mutex);
    int status = retain_stage(cache, stage);
    pthread_mutex_unlock(&cache->mutex);
    return status;
}
