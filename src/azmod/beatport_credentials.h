/* SPDX-License-Identifier: GPL-3.0-only */
/* Beatport's private settings file. Call these only on a storage worker.
 * The caller creates the parent directory and serializes save/forget operations. */
#ifndef AZMOD_BEATPORT_CREDENTIALS_H
#define AZMOD_BEATPORT_CREDENTIALS_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BP_CRED_MAX 127
struct bp_credentials {
    char user[BP_CRED_MAX + 1];
    char pass[BP_CRED_MAX + 1];
};

/* The wrappers let the unit test exercise real short writes and failed commits. */
#ifndef BP_CRED_WRITE
#define BP_CRED_WRITE write
#endif
#ifndef BP_CRED_FSYNC
#define BP_CRED_FSYNC fsync
#endif
#ifndef BP_CRED_RENAME
#define BP_CRED_RENAME rename
#endif

static void bp_credentials_clear(void *data, size_t size) {
    volatile unsigned char *p = data;
    while (size--)
        *p++ = 0;
}

static int bp_credentials_valid_text(const char *s) {
    size_t n = strnlen(s, BP_CRED_MAX + 1);
    if (!n || n > BP_CRED_MAX)
        return 0;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char) s[i] < 0x20 || (unsigned char) s[i] == 0x7f)
            return 0;
    return 1;
}

static int bp_credentials_hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static int bp_credentials_unhex(const char *text, size_t n, char *out) {
    if (!n || (n & 1) || n > 2 * BP_CRED_MAX) return -1;
    for (size_t i = 0; i < n / 2; ++i) {
        int high = bp_credentials_hex_digit((unsigned char) text[2 * i]);
        int low = bp_credentials_hex_digit((unsigned char) text[2 * i + 1]);
        if (high < 0 || low < 0) return -1;
        unsigned value = (unsigned) high * 16 + (unsigned) low;
        if (value < 0x20 || value == 0x7f) return -1;
        out[i] = (char) value;
    }
    out[n / 2] = 0;
    return 0;
}
static int bp_credentials_buffer_valid(const char *buf, size_t used, size_t capacity) {
    return used && used < capacity && !memchr(buf, 0, used);
}
static int bp_credentials_read(int fd, char *buf, size_t capacity, size_t *used) {
    struct stat st;
    if (fstat(fd, &st) != 0) return errno;
    if (!S_ISREG(st.st_mode)) return EINVAL;
    while (*used < capacity) {
        ssize_t n = read(fd, buf + *used, capacity - *used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return errno;
        if (!n) break;
        *used += (size_t) n;
    }
    return bp_credentials_buffer_valid(buf, *used, capacity) ? 0 : EINVAL;
}
static int bp_credentials_field(const char *line, size_t n) {
    if (n < 5) return 0;
    if (!memcmp(line, "user=", 5)) return 1;
    return !memcmp(line, "pass=", 5) ? 2 : 0;
}
static int bp_credentials_parse(const char *buf, size_t used, struct bp_credentials *parsed) {
    int seen = 0;
    for (size_t pos = 0; pos < used;) {
        const char *end = memchr(buf + pos, '\n', used - pos);
        size_t n = end ? (size_t) (end - (buf + pos)) : used - pos;
        int field = bp_credentials_field(buf + pos, n);
        if (!field || (seen & field) || bp_credentials_unhex(buf + pos + 5, n - 5, field == 1 ? parsed->user : parsed->pass) != 0) return EINVAL;
        seen |= field;
        pos += n + (end != NULL);
    }
    return seen == 3 ? 0 : EINVAL;
}
static int bp_credentials_close(int fd, int error) {
    if (close(fd) != 0 && !error) return errno;
    return error;
}
static int bp_credentials_error(int error) {
    if (error) errno = error;
    return error ? -1 : 0;
}
/* 1 = valid pair, 0 = absent, -1 = I/O or malformed; never return partial fields. */
static int bp_credentials_load(const char *path, struct bp_credentials *out) {
    if (!out) return bp_credentials_error(EINVAL);
    bp_credentials_clear(out, sizeof *out);
    if (!path || !*path) return bp_credentials_error(EINVAL);
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    char buf[2 * (5 + 2 * BP_CRED_MAX + 1) + 1];
    struct bp_credentials parsed = {{0}, {0}};
    size_t used = 0;
    int error = bp_credentials_read(fd, buf, sizeof buf, &used);
    if (!error) error = bp_credentials_parse(buf, used, &parsed);
    error = bp_credentials_close(fd, error);
    if (!error) *out = parsed;
    bp_credentials_clear(&parsed, sizeof parsed);
    bp_credentials_clear(buf, sizeof buf);
    return error ? bp_credentials_error(error) : 1;
}

static int bp_credentials_directory(const char *path) {
    char *dir = strdup(path);
    if (!dir)
        return -1;
    char *slash = strrchr(dir, '/');
    if (!slash) {
        free(dir);
        return open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    slash[slash == dir ? 1 : 0] = 0;
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int error = errno;
    free(dir);
    errno = error;
    return fd;
}

static int bp_credentials_write_all(int fd, const char *buf, size_t n) {
    while (n) {
        ssize_t wrote = BP_CRED_WRITE(fd, buf, n);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            if (!wrote)
                errno = EIO;
            return -1;
        }
        buf += (size_t) wrote;
        n -= (size_t) wrote;
    }
    return 0;
}

static int bp_credentials_temp(const char *path, char **temp) {
    size_t length = strlen(path);
    *temp = malloc(length + sizeof ".tmp.XXXXXX");
    if (!*temp) return -1;
    memcpy(*temp, path, length);
    memcpy(*temp + length, ".tmp.XXXXXX", sizeof ".tmp.XXXXXX");
    int fd = mkstemp(*temp);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fchmod(fd, 0600) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}
static size_t bp_credentials_encode(char *buf, const struct bp_credentials *creds) {
    static const char hex[] = "0123456789abcdef";
    size_t used = 0;
    for (int field = 0; field < 2; ++field) {
        const unsigned char *text = (const unsigned char *) (field ? creds->pass : creds->user);
        memcpy(buf + used, field ? "pass=" : "user=", 5);
        used += 5;
        while (*text) {
            buf[used++] = hex[*text >> 4];
            buf[used++] = hex[*text++ & 15];
        }
        buf[used++] = '\n';
    }
    return used;
}
static int bp_credentials_store(int fd, const struct bp_credentials *creds) {
    char buf[2 * (5 + 2 * BP_CRED_MAX + 1)];
    size_t used = bp_credentials_encode(buf, creds);
    int error = 0;
    if (bp_credentials_write_all(fd, buf, used) != 0 || BP_CRED_FSYNC(fd) != 0) error = errno;
    error = bp_credentials_close(fd, error);
    bp_credentials_clear(buf, sizeof buf);
    return error;
}
static int bp_credentials_save_valid(const char *path, const struct bp_credentials *creds) {
    return path && *path && creds && bp_credentials_valid_text(creds->user) && bp_credentials_valid_text(creds->pass);
}
/* Before rename, every failure preserves the previous file. A directory-sync
 * failure after rename reports uncertain durability of the visible replacement. */
static int bp_credentials_save(const char *path, const struct bp_credentials *creds) {
    if (!bp_credentials_save_valid(path, creds)) return bp_credentials_error(EINVAL);
    int directory = bp_credentials_directory(path);
    if (directory < 0) return -1;
    char *temp = NULL;
    int fd = bp_credentials_temp(path, &temp);
    int error = fd < 0 ? errno : bp_credentials_store(fd, creds);
    int renamed = 0;
    if (!error) {
        if (BP_CRED_RENAME(temp, path) != 0) error = errno;
        else {
            renamed = 1;
            if (BP_CRED_FSYNC(directory) != 0) error = errno;
        }
    }
    if (temp && !renamed) unlink(temp);
    free(temp);
    return bp_credentials_error(bp_credentials_close(directory, error));
}

/* A successful removal is synced before reporting success. A failed directory
 * fsync reports uncertain durability, as with save. Missing files succeed. */
static int bp_credentials_remove(const char *path, int directory) {
    if (unlink(path) != 0 && errno != ENOENT) return errno;
    return BP_CRED_FSYNC(directory) != 0 ? errno : 0;
}
static int bp_credentials_forget(const char *path) {
    if (!path || !*path) return bp_credentials_error(EINVAL);
    int directory = bp_credentials_directory(path);
    if (directory < 0) return errno == ENOENT ? 0 : -1;
    int error = bp_credentials_remove(path, directory);
    return bp_credentials_error(bp_credentials_close(directory, error));
}

#endif
