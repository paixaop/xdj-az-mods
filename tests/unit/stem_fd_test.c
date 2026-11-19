/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "libfd.h"
static int fail_open, close_during_scan = -1;
extern DIR *__real_opendir(const char *);
extern ssize_t __real_readlink(const char *, char *, size_t);
DIR *__wrap_opendir(const char *path) {
    if (fail_open && !strcmp(path, "/proc/self/fd")) {
        errno = EMFILE;
        return NULL;
    }
    return __real_opendir(path);
}
ssize_t __wrap_readlink(const char *path, char *target, size_t size) {
    char closing[64];
    snprintf(closing, sizeof closing, "/proc/self/fd/%d", close_during_scan);
    if (close_during_scan >= 0 && !strcmp(closing, path)) {
        assert(!close(close_during_scan));
        close_during_scan = -1;
    }
    return __real_readlink(path, target, size);
}
#include STEM_FD_IMPL_INC

int main(void) {
    char path[] = "/tmp/az-stem-fd.XXXXXX", other[] = "/tmp/az-stem-fd-other.XXXXXX";
    int native = mkostemp(path, O_CLOEXEC), unrelated = mkostemp(other, O_CLOEXEC);
    assert(native >= 0 && unrelated >= 0);
    int stems = open(path, O_RDONLY | O_CLOEXEC);
    int fields = open(path, O_RDONLY | O_CLOEXEC);
    int status = open(path, O_RDONLY | O_CLOEXEC);
    assert(stems >= 0 && fields >= 0 && status >= 0);
    assert(libfd_add(stems, LIBFD_STEMS));
    assert(libfd_add(fields, LIBFD_FIELDS));
    assert(libfd_add(status, LIBFD_STATUS));
    assert(sm_fds_on(path) == 2); /* native owner and this worker; other workers excluded */
    libfd_del(fields);
    assert(sm_fds_on(path) == 3);
    assert(libfd_add(fields, LIBFD_FIELDS));
    assert(sm_fds_on(path) == 2);
    close_during_scan = native;
    assert(sm_fds_on(path) == 1 && close_during_scan == -1); /* concurrent native eject */
    libfd_del(stems);
    assert(!close(stems));
    assert(sm_fds_on(path) == 0);
    fail_open = 1;
    assert(sm_fds_on(path) == -1); /* unknown must not be misreported as ejected */
    fail_open = 0;
    assert(sm_fds_on(other) == 1);
    libfd_del(fields);
    libfd_del(status);
    assert(!close(fields) && !close(status) && !close(unrelated));
    assert(!unlink(path) && !unlink(other));
    puts("PASS stem fd ownership: real procfs, independent workers, concurrent close and unknown scan");
    return 0;
}
