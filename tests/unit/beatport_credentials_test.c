/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_mode, write_calls, sync_fail_at, sync_calls, rename_fails;
static ssize_t test_write(int fd, const void *buf, size_t n) {
    ++write_calls;
    if (write_mode == 1 && write_calls == 1) {
        errno = EINTR;
        return -1;
    }
    if (write_mode == 2 && write_calls > 1) {
        errno = ENOSPC;
        return -1;
    }
    if (write_mode == 3)
        return 0;
    return write(fd, buf, write_mode && n > 3 ? 3 : n);
}
static int test_fsync(int fd) {
    if (++sync_calls == sync_fail_at) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}
static int test_rename(const char *old, const char *new) {
    if (rename_fails) {
        errno = EACCES;
        return -1;
    }
    return rename(old, new);
}
#define BP_CRED_WRITE test_write
#define BP_CRED_FSYNC test_fsync
#define BP_CRED_RENAME test_rename
#include "../../src/azmod/beatport_credentials.h"

static void reset_faults(void) {
    write_mode = write_calls = sync_fail_at = sync_calls = rename_fails = 0;
}
static void write_fixture(const char *path, const char *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, data, n) == (ssize_t) n);
    assert(close(fd) == 0);
}
static void assert_loaded(const char *path, const struct bp_credentials *expected) {
    struct bp_credentials got;
    memset(&got, 0xff, sizeof got);
    assert(bp_credentials_load(path, &got) == 1);
    assert(strcmp(got.user, expected->user) == 0);
    assert(strcmp(got.pass, expected->pass) == 0);
}
static void assert_empty(const struct bp_credentials *creds) {
    const unsigned char *p = (const unsigned char *) creds;
    for (size_t i = 0; i < sizeof *creds; ++i)
        assert(p[i] == 0);
}
static void assert_only_file(const char *dir, const char *file) {
    DIR *d = opendir(dir);
    assert(d);
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        assert(file && !strcmp(entry->d_name, file));
        ++count;
    }
    assert(count == (file != NULL));
    assert(closedir(d) == 0);
}

int main(void) {
    char dir[] = "/tmp/beatport-credentials.XXXXXX", path[256];
    assert(mkdtemp(dir));
    assert(snprintf(path, sizeof path, "%s/login.dat", dir) > 0);
    struct bp_credentials first = {"test@example.invalid", "space = and \xc3\xa9"};
    struct bp_credentials second = {"new@example.invalid", "changed password"};
    struct bp_credentials got;
    memset(&got, 0xff, sizeof got);
    assert(bp_credentials_load(path, &got) == 0);
    assert_empty(&got);
    assert(bp_credentials_forget(path) == 0);
    assert(bp_credentials_save(path, &first) == 0);
    assert_loaded(path, &first);
    struct stat st;
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    assert_only_file(dir, "login.dat");

    /* Exact legacy format, with upper-case hex and an optional final newline. */
    const char legacy[] = "pass=50C3A97373\nuser=55736572";
    write_fixture(path, legacy, sizeof legacy - 1);
    struct bp_credentials legacy_pair = {"User", "P\xc3\xa9ss"};
    assert_loaded(path, &legacy_pair);
    assert(chmod(path, 0644) == 0);
    assert(bp_credentials_save(path, &first) == 0);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    /* A full 127 UTF-8 bytes round-trip; never silently truncate the 128th. */
    struct bp_credentials longest;
    memset(&longest, 'x', sizeof longest);
    longest.user[BP_CRED_MAX] = longest.pass[BP_CRED_MAX] = 0;
    assert(bp_credentials_save(path, &longest) == 0);
    assert_loaded(path, &longest);
    longest.pass[BP_CRED_MAX] = 'x';
    assert(bp_credentials_save(path, &longest) == -1 && errno == EINVAL);
    longest.pass[BP_CRED_MAX] = 0;
    assert_loaded(path, &longest);

    const char *bad[] = {
        "", "user=61\n", "pass=62\n", "user=61\npass=\n", "user=\npass=62\n",
        "user=61\npass=6\n", "user=61\npass=zz\n", "user=61\npass=620063\n",
        "user=61\npass=620a63\n", "user=61\npass=7f\n", "user=61\npass=01\n",
        "user=61\npass=62\nuser=63\n", "user=61\npass=62\nunknown=63\n",
        "user=61\npass=62\n\n", "user=61\npass=62x", "user=61\r\npass=62\n"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        write_fixture(path, bad[i], strlen(bad[i]));
        memset(&got, 0xff, sizeof got);
        assert(bp_credentials_load(path, &got) == -1 && errno == EINVAL);
        assert_empty(&got);
    }
    const char embedded_nul[] = "user=61\npass=62\n\0ignored";
    write_fixture(path, embedded_nul, sizeof embedded_nul - 1);
    assert(bp_credentials_load(path, &got) == -1 && errno == EINVAL);
    assert_empty(&got);
    char large[1024];
    memset(large, 'a', sizeof large);
    write_fixture(path, large, sizeof large);
    assert(bp_credentials_load(path, &got) == -1 && errno == EINVAL);
    assert_empty(&got);
    /* Overlong second field below the total file limit must also be rejected. */
    strcpy(large, "user=61\npass=");
    memset(large + strlen(large), '6', 256);
    write_fixture(path, large, 13 + 256);
    assert(bp_credentials_load(path, &got) == -1 && errno == EINVAL);
    assert_empty(&got);

    assert(bp_credentials_save(path, &first) == 0);
    reset_faults();
    write_mode = 1;
    assert(bp_credentials_save(path, &second) == 0);
    assert(write_calls > 2 && sync_calls == 2);
    assert_loaded(path, &second);
    reset_faults();
    assert(bp_credentials_save(path, &first) == 0);

    /* Every pre-commit failure preserves the previous login and removes temp. */
    for (int fault = 0; fault < 4; ++fault) {
        reset_faults();
        if (fault == 0) write_mode = 2;
        if (fault == 1) write_mode = 3;
        if (fault == 2) sync_fail_at = 1;
        if (fault == 3) rename_fails = 1;
        assert(bp_credentials_save(path, &second) == -1);
        assert_loaded(path, &first);
        assert_only_file(dir, "login.dat");
    }
    /* A failed directory sync cannot roll back a completed rename: report it. */
    reset_faults();
    sync_fail_at = 2;
    assert(bp_credentials_save(path, &second) == -1 && errno == EIO);
    assert_loaded(path, &second);
    assert_only_file(dir, "login.dat");
    reset_faults();
    assert(bp_credentials_forget(path) == 0);
    assert(bp_credentials_load(path, &got) == 0);
    assert_only_file(dir, NULL);

    /* Nonregular inputs fail without blocking or disclosing another file. */
    assert(mkfifo(path, 0600) == 0);
    assert(bp_credentials_load(path, &got) == -1 && errno == EINVAL);
    assert_empty(&got);
    assert(unlink(path) == 0);
    assert(symlink("/dev/null", path) == 0);
    assert(bp_credentials_load(path, &got) == -1);
    assert_empty(&got);
    assert(unlink(path) == 0);
    assert(mkdir(path, 0700) == 0);
    assert(bp_credentials_save(path, &first) == -1);
    assert(bp_credentials_forget(path) == -1);
    assert(rmdir(path) == 0);
    assert_only_file(dir, NULL);
    assert(rmdir(dir) == 0);
    memset(&got, 0xff, sizeof got);
    assert(bp_credentials_load(NULL, &got) == -1 && errno == EINVAL);
    assert_empty(&got);
    puts("Beatport credential storage: passed");
    return 0;
}
