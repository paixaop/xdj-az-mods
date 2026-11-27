/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "source_import.h"
#include "source_import_format.h"
#include "../common/azsha256.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define IMPORT_DIRECTORY "AZ-MODS-IMPORTS"
static _Atomic unsigned int import_directory_number;

struct import_copy {
    int root, scratch;
    char temp[64], name[20], published_name[20], digest[65];
};

static const char *import_extension(const char *path) {
    const char *name = strrchr(path, '/');
    const char *suffix = name ? strrchr(name, '.') : NULL;
    if (!suffix || !suffix[1] || strlen(suffix) > 8)
        return NULL;
    const char *valid = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    return strspn(suffix + 1, valid) == strlen(suffix + 1) ? suffix : NULL;
}

static int import_mount_matches(int directory, const struct st_source *source) {
    struct stat st;
    if (fstat(directory, &st) || (uint64_t) st.st_dev != source->device)
        return 0;
#ifdef __linux__
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/self/fdinfo/%d", directory);
    FILE *info = fopen(path, "re");
    if (!info)
        return 0;
    unsigned long long mount_id = 0;
    while (fgets(line, sizeof line, info)) {
        if (sscanf(line, "mnt_id: %llu", &mount_id) == 1)
            break;
    }
    fclose(info);
    return mount_id && mount_id == source->mount_id;
#else
    return 1;
#endif
}

static int import_database_matches(int mount, const struct st_source *source) {
    char path[64];
    snprintf(path, sizeof path, "%s/rekordbox/exportLibrary.db", st_source_export(source));
    struct stat st;
    return !fstatat(mount, path, &st, 0) && S_ISREG(st.st_mode) &&
           (uint64_t) st.st_dev == source->device && (uint64_t) st.st_ino == source->inode;
}

static int import_open_directory(int mount, const struct st_source *source) {
    if (mkdirat(mount, IMPORT_DIRECTORY, 0777) && errno != EEXIST)
        return -1;
    int root = openat(mount, IMPORT_DIRECTORY, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root >= 0 && !import_mount_matches(root, source)) {
        close(root);
        return -1;
    }
    return root;
}

static int import_open_root(const struct st_source *source) {
    int mount = open(source->mount, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (mount < 0)
        return -1;
    int root = -1;
    if (import_mount_matches(mount, source) && import_database_matches(mount, source))
        root = import_open_directory(mount, source);
    close(mount);
    return root;
}

static int import_create_scratch(struct import_copy *copy) {
    for (unsigned i = 0; i < 16; ++i) {
        unsigned serial = atomic_fetch_add_explicit(&import_directory_number, 1, memory_order_relaxed);
        snprintf(copy->temp, sizeof(copy->temp), ".partial-%ld-%u", (long) getpid(), serial);
        if (!mkdirat(copy->root, copy->temp, 0700)) {
            copy->scratch = openat(copy->root, copy->temp, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            return copy->scratch >= 0;
        }
        if (errno != EEXIST)
            break;
    }
    copy->temp[0] = 0;
    return 0;
}

static void import_copy_end(struct import_copy *copy) {
    if (copy->scratch >= 0) {
        (void) unlinkat(copy->scratch, copy->name, 0);
        close(copy->scratch);
    }
    if (copy->temp[0])
        (void) unlinkat(copy->root, copy->temp, AT_REMOVEDIR);
    close(copy->root);
}

static int import_write(int fd, const uint8_t *bytes, size_t size) {
    if (fd < 0)
        return 1;
    while (size) {
        ssize_t n = write(fd, bytes, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return 0;
        bytes += n;
        size -= (size_t) n;
    }
    return 1;
}

static int import_hash_copy(int in, int out, azsha256_ctx *hash) {
    uint8_t buffer[32768];
    for (;;) {
        ssize_t n = read(in, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n == 0;
        if (!import_write(out, buffer, (size_t) n))
            return 0;
        azsha256_update(hash, buffer, (size_t) n);
    }
}

static uint64_t import_signature(int fd) {
    struct stat st;
    return !fstat(fd, &st) && st.st_size > 0 ? st_stat_sig(&st) : 0;
}

static int import_digest(int in, int out, const char *provider, const char *id, char digest[65]) {
    uint64_t sig = import_signature(in);
    if (!sig)
        return 0;
    azsha256_ctx hash;
    azsha256_init(&hash);
    azsha256_update(&hash, provider, strlen(provider) + 1);
    azsha256_update(&hash, id, strlen(id) + 1);
    if (!import_hash_copy(in, out, &hash) || sig != import_signature(in))
        return 0;
    azsha256_final_hex(&hash, digest);
    return 1;
}

static int import_copy_audio(struct import_copy *copy, const char *path, const char *provider, const char *id) {
    int in = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (in < 0)
        return 0;
    int out = openat(copy->scratch, copy->name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
    int ok = 0;
    if (out >= 0) {
        ok = import_digest(in, out, provider, id, copy->digest) && !fsync(out);
        if (close(out) != 0) ok = 0;
    }
    close(in);
    return ok && !fsync(copy->scratch);
}

static int import_find_name(DIR *directory, struct import_copy *copy) {
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(directory))) {
        if (strcasecmp(entry->d_name, copy->name))
            continue;
        if (copy->published_name[0])
            return 0;
        /* Use the stored spelling so the native analyzer sees the same path again. */
        memcpy(copy->published_name, entry->d_name, strlen(copy->name) + 1);
    }
    return !errno && copy->published_name[0];
}

static int import_existing_file(struct import_copy *copy) {
    int directory = openat(copy->root, copy->digest, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0)
        return -1;
    DIR *entries = fdopendir(directory);
    if (!entries) {
        close(directory);
        return -1;
    }
    int fd = -1;
    if (import_find_name(entries, copy))
        fd = openat(directory, copy->published_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    closedir(entries);
    return fd;
}

static int import_identical(struct import_copy *copy, const char *provider, const char *id) {
    int fd = import_existing_file(copy);
    if (fd < 0)
        return 0;
    char digest[65];
    int ok = import_digest(fd, -1, provider, id, digest) && !strcmp(digest, copy->digest);
    close(fd);
    return ok;
}

static int import_publish_directory(struct import_copy *copy, const char *provider, const char *id) {
    /* Publishing a directory protects an existing nonempty destination on FAT
     * and exFAT, which lack hard links and may lack RENAME_NOREPLACE. */
    if (renameat(copy->root, copy->temp, copy->root, copy->digest) == 0) {
        memcpy(copy->published_name, copy->name, sizeof copy->name);
        copy->temp[0] = 0;
        close(copy->scratch);
        copy->scratch = -1;
        return !fsync(copy->root);
    }
    return import_identical(copy, provider, id) && !fsync(copy->root);
}

static int import_validate_copy(const struct azspi_request *request, const struct import_copy *copy,
                                struct azspi_audio *format) {
    char scratch[sizeof IMPORT_DIRECTORY + sizeof copy->temp + sizeof copy->name + 3];
    snprintf(scratch, sizeof scratch, "/" IMPORT_DIRECTORY "/%s/%s", copy->temp, copy->name);
    int status = azspi_probe_format(request->firmware, request->source, scratch, format);
    if (status == AZ_OK && request->cancelled && request->cancelled(request->context))
        return AZ_CANCELLED;
    return status;
}

static int import_publish_copy(const struct azspi_request *request, struct import_copy *copy,
                               const char *path, struct azspi_audio *format) {
    const char *id = request->track->id;
    if (!import_create_scratch(copy) || !import_copy_audio(copy, path, request->provider, id))
        return AZ_FAILED;
    int status = import_validate_copy(request, copy, format);
    if (status == AZ_OK && !import_publish_directory(copy, request->provider, id))
        return AZ_FAILED;
    return status;
}

int azspi_publish(const struct azspi_request *request, char relative[AZ_SOURCE_PATH_BYTES],
                  struct azspi_audio *format) {
    const char *path = azsp_stage_path(request->stage);
    const char *suffix = import_extension(path);
    if (!suffix || azsp_stage_mode(request->stage) != AZ_SOURCE_IMPORT)
        return AZ_INVALID;
    struct import_copy copy = {.root = import_open_root(request->source), .scratch = -1};
    if (copy.root < 0)
        return AZ_STALE;
    snprintf(copy.name, sizeof(copy.name), "track%s", suffix);
    struct azspi_audio measured;
    int status = import_publish_copy(request, &copy, path, &measured);
    if (status == AZ_OK) {
        snprintf(relative, AZ_SOURCE_PATH_BYTES, "/" IMPORT_DIRECTORY "/%s/%s", copy.digest, copy.published_name);
        *format = measured;
    }
    import_copy_end(&copy);
    return status;
}
