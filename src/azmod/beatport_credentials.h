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

static int bp_credentials_unhex(const char *text, size_t n, char *out) {
    if (!n || (n & 1) || n > 2 * BP_CRED_MAX)
        return -1;
    for (size_t i = 0; i < n / 2; ++i) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; ++j) {
            unsigned char c = (unsigned char) text[2 * i + j];
            unsigned d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return -1;
            value = value * 16 + d;
        }
        if (value < 0x20 || value == 0x7f)
            return -1;
        out[i] = (char) value;
    }
    out[n / 2] = 0;
    return 0;
}

/* 1 = complete valid pair, 0 = absent, -1 = I/O or malformed (errno=EINVAL).
 * No partially decoded credentials escape on any failure. */
static int bp_credentials_load(const char *path, struct bp_credentials *out) {
    char buf[2 * (5 + 2 * BP_CRED_MAX + 1) + 1];
    struct bp_credentials parsed = {{0}, {0}};
    size_t used = 0;
    int rc = -1, error = 0, seen = 0;
    if (!out) {
        errno = EINVAL;
        return -1;
    }
    bp_credentials_clear(out, sizeof *out);
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) != 0) {
        error = errno;
        goto done;
    }
    if (!S_ISREG(st.st_mode)) {
        error = EINVAL;
        goto done;
    }
    while (used < sizeof buf) {
        ssize_t n = read(fd, buf + used, sizeof buf - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            error = errno;
            goto done;
        }
        if (!n)
            break;
        used += (size_t) n;
    }
    if (!used || used == sizeof buf || memchr(buf, 0, used)) {
        error = EINVAL;
        goto done;
    }
    for (size_t pos = 0; pos < used;) {
        char *end = memchr(buf + pos, '\n', used - pos);
        size_t n = end ? (size_t) (end - (buf + pos)) : used - pos;
        int field = n >= 5 && !memcmp(buf + pos, "user=", 5) ? 1 : n >= 5 && !memcmp(buf + pos, "pass=", 5) ? 2
                                                                                                            : 0;
        if (!field || (seen & field) ||
            bp_credentials_unhex(buf + pos + 5, n - 5,
                                 field == 1 ? parsed.user : parsed.pass) != 0) {
            error = EINVAL;
            goto done;
        }
        seen |= field;
        pos += n + (end != NULL);
    }
    if (seen != 3) {
        error = EINVAL;
        goto done;
    }
    rc = 1;
done:
    if (close(fd) != 0 && !error) {
        error = errno;
        rc = -1;
    }
    if (rc == 1)
        *out = parsed;
    bp_credentials_clear(&parsed, sizeof parsed);
    bp_credentials_clear(buf, sizeof buf);
    if (error)
        errno = error;
    return rc;
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

/* 0 = durable replacement, -1 = failure. Before rename, failures preserve the
 * previous file. After rename, a directory-fsync failure reports uncertain
 * durability; the complete new file is already visible. */
static int bp_credentials_save(const char *path, const struct bp_credentials *creds) {
    static const char hex[] = "0123456789abcdef";
    char buf[2 * (5 + 2 * BP_CRED_MAX + 1)];
    size_t used = 0;
    int fd = -1, dirfd = -1, error = 0, renamed = 0;
    char *temp = NULL;
    if (!path || !*path || !creds || !bp_credentials_valid_text(creds->user) ||
        !bp_credentials_valid_text(creds->pass)) {
        errno = EINVAL;
        return -1;
    }
    dirfd = bp_credentials_directory(path);
    if (dirfd < 0)
        return -1;
    size_t path_len = strlen(path);
    temp = malloc(path_len + sizeof ".tmp.XXXXXX");
    if (!temp) {
        error = errno;
        goto done;
    }
    memcpy(temp, path, path_len);
    memcpy(temp + path_len, ".tmp.XXXXXX", sizeof ".tmp.XXXXXX");
    fd = mkstemp(temp);
    if (fd < 0) {
        error = errno;
        goto done;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0 || fchmod(fd, 0600) != 0) {
        error = errno;
        goto done;
    }
    for (int field = 0; field < 2; ++field) {
        const unsigned char *s = (const unsigned char *) (field ? creds->pass : creds->user);
        memcpy(buf + used, field ? "pass=" : "user=", 5);
        used += 5;
        while (*s) {
            buf[used++] = hex[*s >> 4];
            buf[used++] = hex[*s++ & 15];
        }
        buf[used++] = '\n';
    }
    if (bp_credentials_write_all(fd, buf, used) != 0 || BP_CRED_FSYNC(fd) != 0) {
        error = errno;
        goto done;
    }
    if (close(fd) != 0) {
        fd = -1;
        error = errno;
        goto done;
    }
    fd = -1;
    if (BP_CRED_RENAME(temp, path) != 0) {
        error = errno;
        goto done;
    }
    renamed = 1;
    if (BP_CRED_FSYNC(dirfd) != 0)
        error = errno;
done:
    if (fd >= 0)
        close(fd);
    if (temp && !renamed)
        unlink(temp);
    free(temp);
    if (close(dirfd) != 0 && !error)
        error = errno;
    bp_credentials_clear(buf, sizeof buf);
    if (error)
        errno = error;
    return error ? -1 : 0;
}

/* A successful removal is synced before reporting success. A failed directory
 * fsync reports uncertain durability, as with save. Missing files succeed. */
static int bp_credentials_forget(const char *path) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    int dirfd = bp_credentials_directory(path);
    if (dirfd < 0)
        return errno == ENOENT ? 0 : -1;
    int error = 0;
    if (unlink(path) != 0 && errno != ENOENT)
        error = errno;
    if (!error && BP_CRED_FSYNC(dirfd) != 0)
        error = errno;
    if (close(dirfd) != 0 && !error)
        error = errno;
    if (error)
        errno = error;
    return error ? -1 : 0;
}

#endif
