/* SPDX-License-Identifier: GPL-3.0-only */
/* libfd.h: the file descriptors azmod's own read-only library connections hold, by owner.
 *
 * A stick's exportLibrary.db is open in EP147 several times over: EP147's own connections, and
 * azmod's (stem_marks.c's worker, browse_fields.c's worker). Each worker tells an eject from EP147
 * closing its connections, so it must count EP147's alone: an fd of another azmod worker read as
 * EP147's makes that worker's own idle close look like an eject to the other. Each worker
 * registers the fd its connection holds (libfd_of_db: SQLite's own file handle, checked against
 * the file's inode) and leaves the other owners' fds out of its count.
 * Lock-free: an fd is published with a compare-and-swap and read with acquire loads. */
#ifndef LIBFD_H
#define LIBFD_H
#include <stdint.h>
#include <dlfcn.h>
#include <sys/stat.h>

#define LIBFD_MAX 16
enum {
    LIBFD_STEMS = 1,
    LIBFD_FIELDS = 2,
    LIBFD_STATUS = 3
};
static int g_libfd[LIBFD_MAX]; /* (owner << 24) | (fd + 1), 0 free (__atomic only) */

static int libfd_add(int fd, int owner) {
    if (fd < 0 || fd >= (1 << 24) || owner <= 0 || owner > 127)
        return 0;
    int v = (owner << 24) | (fd + 1);
    for (int i = 0; i < LIBFD_MAX; i++) {
        int z = 0;
        if (__atomic_compare_exchange_n(&g_libfd[i], &z, v, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 1;
    }
    return 0;
}
static void libfd_del(int fd) {
    if (fd < 0 || fd >= (1 << 24))
        return;
    for (int i = 0; i < LIBFD_MAX; i++) {
        int v = __atomic_load_n(&g_libfd[i], __ATOMIC_ACQUIRE);
        if (v && (v & 0xffffff) == fd + 1)
            __atomic_compare_exchange_n(&g_libfd[i], &v, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
}
/* the owner that registered fd, 0 for none (an fd of EP147's, or one not ours) */
static int libfd_owner(int fd) {
    for (int i = 0; i < LIBFD_MAX; i++) {
        int v = __atomic_load_n(&g_libfd[i], __ATOMIC_ACQUIRE);
        if (v && (v & 0xffffff) == fd + 1)
            return v >> 24;
    }
    return 0;
}
/* SQLite's own file handle of a connection's main database (SQLITE_FCNTL_FILE_POINTER); for the
 * unix VFS that is a unixFile, whose fd follows three pointers. -1 when there is none. */
static int libfd_fd_of(void *db) {
    static int (*fc)(void *, const char *, int, void *);
    int (*f)(void *, const char *, int, void *) = __atomic_load_n(&fc, __ATOMIC_ACQUIRE);
    void *p = 0;
    if (!f) {
        *(void **) &f = dlsym(RTLD_DEFAULT, "sqlite3_file_control");
        __atomic_store_n(&fc, f, __ATOMIC_RELEASE);
    }
    if (!f || !db || f(db, "main", 7 /* SQLITE_FCNTL_FILE_POINTER */, &p) != 0 || !p)
        return -1;
    return *(const int *) ((const char *) p + 3 * sizeof(void *));
}
/* ... taken only when it is open on the same file as path (same device and inode) */
static int libfd_of_db(void *db, const char *path) {
    struct stat a, b;
    int fd = libfd_fd_of(db);
    if (fd < 0 || fstat(fd, &a) != 0 || stat(path, &b) != 0 || a.st_dev != b.st_dev || a.st_ino != b.st_ino)
        return -1;
    return fd;
}
#endif
